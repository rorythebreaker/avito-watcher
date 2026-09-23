#include "notify/notify.h"

#include "net/http.h"
#include "util/json.h"
#include "util/log.h"
#include "util/strings.h"

namespace notify {
namespace telegram {
namespace {

using util::Json;

constexpr size_t kCaptionLimit = 1024;
constexpr size_t kMessageLimit = 4096;
// Never fire off more than this many cards at once, whatever arrived.
constexpr size_t kMaxCards = 10;

std::string api_url(const core::Settings& settings, const std::string& method) {
    return "https://api.telegram.org/bot" + util::trim(settings.telegram_token) + "/" + method;
}

// Cuts UTF-8 text without splitting a character in half.
std::string clip_utf8(const std::string& text, size_t limit) {
    if (text.size() <= limit) return text;
    size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end);
}

bool call(const core::Settings& settings, const std::string& method, const Json& payload,
          Json& result, std::string& error) {
    net::HttpClient client;
    client.set_proxy(settings.proxy);
    client.set_timeout_seconds(20);
    client.set_cookies_enabled(false);

    net::Response response = client.post(api_url(settings, method), payload.dump(),
                                         "application/json");
    if (!response.ok) {
        error = response.error.empty() ? "нет связи с Telegram" : response.error;
        return false;
    }

    Json body = Json::parse(response.body);
    if (!body.is_object()) {
        error = "Telegram ответил не по протоколу (код " + std::to_string(response.status) + ")";
        return false;
    }
    if (!body["ok"].as_bool(false)) {
        error = body["description"].as_string(
            "Ошибка Telegram (" + std::to_string(response.status) + ")");
        return false;
    }
    result = body["result"];
    return true;
}

std::string format_listing(const core::Listing& listing, const std::string& task_name) {
    std::string text = "<b>" +
                       util::html_escape(listing.title.empty() ? "Без названия" : listing.title) +
                       "</b>\n";
    text += "\xF0\x9F\x92\xB0 " + util::html_escape(listing.price_label()) + "\n";  // 💰

    std::string where_when = listing.location;
    if (!listing.date_text.empty()) {
        if (!where_when.empty()) where_when += " · ";
        where_when += listing.date_text;
    }
    if (!where_when.empty()) {
        text += "\xF0\x9F\x93\x8D " + util::html_escape(where_when) + "\n";          // 📍
    }
    if (!listing.seller.empty()) {
        text += "\xF0\x9F\x91\xA4 " + util::html_escape(listing.seller) + "\n";      // 👤
    }
    if (listing.score < 100) {
        text += "\xF0\x9F\x8E\xAF Похожесть: " + std::to_string(listing.score) + "%\n";  // 🎯
    }
    text += "\xF0\x9F\x94\x8E " + util::html_escape(task_name) + "\n";               // 🔎
    text += "\n<a href=\"" + util::html_escape(listing.url) + "\">Открыть на Авито</a>";
    return text;
}

bool send_one(const core::Settings& settings, const core::Listing& listing,
              const std::string& task_name, std::string& error) {
    const std::string text = format_listing(listing, task_name);
    const std::string chat_id = util::trim(settings.telegram_chat_id);

    if (settings.telegram_with_photo && !listing.image_url.empty()) {
        Json payload = Json::object();
        payload["chat_id"] = Json(chat_id);
        payload["photo"] = Json(listing.image_url);
        payload["caption"] = Json(clip_utf8(text, kCaptionLimit));
        payload["parse_mode"] = Json("HTML");

        Json result;
        std::string photo_error;
        if (call(settings, "sendPhoto", payload, result, photo_error)) return true;
        // Telegram may fail to fetch the picture; that is no reason to lose the
        // notification, so fall through to a plain message.
        util::log_info("telegram", "sendPhoto не прошёл (" + photo_error + "), шлю текстом");
    }

    Json payload = Json::object();
    payload["chat_id"] = Json(chat_id);
    payload["text"] = Json(clip_utf8(text, kMessageLimit));
    payload["parse_mode"] = Json("HTML");
    payload["disable_web_page_preview"] = Json(false);

    Json result;
    return call(settings, "sendMessage", payload, result, error);
}

}  // namespace

bool send_batch(const core::Settings& settings, const std::vector<core::Listing>& listings,
                const std::string& task_name, std::string& error) {
    size_t sent = 0;
    for (const core::Listing& listing : listings) {
        if (sent >= kMaxCards) break;
        if (!send_one(settings, listing, task_name, error)) return false;
        ++sent;
    }

    if (listings.size() > kMaxCards) {
        Json payload = Json::object();
        payload["chat_id"] = Json(util::trim(settings.telegram_chat_id));
        payload["text"] = Json("…и ещё " + std::to_string(listings.size() - kMaxCards) +
                               " объявлений по задаче «" + task_name +
                               "». Смотрите в приложении.");
        Json result;
        if (!call(settings, "sendMessage", payload, result, error)) return false;
    }
    return true;
}

bool check(const core::Settings& settings, std::string& info, std::string& error) {
    if (util::trim(settings.telegram_token).empty()) {
        error = "Не заполнен токен бота";
        return false;
    }
    if (util::trim(settings.telegram_chat_id).empty()) {
        error = "Не заполнен chat_id";
        return false;
    }

    Json me;
    if (!call(settings, "getMe", Json::object(), me, error)) return false;

    Json payload = Json::object();
    payload["chat_id"] = Json(util::trim(settings.telegram_chat_id));
    payload["text"] = Json("\xE2\x9C\x85 Avito Watcher подключён. "
                           "Сюда будут приходить новые объявления.");
    Json result;
    if (!call(settings, "sendMessage", payload, result, error)) return false;

    info = me["username"].as_string("бот");
    return true;
}

bool resolve_chat_id(const core::Settings& settings, std::string& chat_id, std::string& error) {
    if (util::trim(settings.telegram_token).empty()) {
        error = "Сначала заполните токен бота";
        return false;
    }

    Json payload = Json::object();
    payload["limit"] = Json(10);
    Json updates;
    if (!call(settings, "getUpdates", payload, updates, error)) return false;

    for (size_t i = updates.size(); i > 0; --i) {
        const Json& update = updates.at(i - 1);
        const Json& message = update.has("message") ? update["message"] : update["channel_post"];
        const Json& chat = message["chat"];
        if (chat.has("id")) {
            chat_id = std::to_string(chat["id"].as_int(0));
            return true;
        }
    }

    error =
        "Не вижу сообщений. Откройте своего бота в Telegram, нажмите «Старт» "
        "и попробуйте снова.";
    return false;
}

}  // namespace telegram
}  // namespace notify
