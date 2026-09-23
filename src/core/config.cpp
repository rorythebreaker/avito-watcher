#include "core/config.h"

#include <map>

#include "core/secrets.h"
#include "util/json.h"
#include "util/paths.h"
#include "util/strings.h"

namespace core {
namespace {

using util::Json;

Settings from_json(const Json& root) {
    Settings s;
    if (!root.is_object()) return s;

    auto num = [&](const char* key, auto& field) {
        if (root.has(key)) field = static_cast<std::remove_reference_t<decltype(field)>>(
            root[key].as_double(static_cast<double>(field)));
    };
    auto flag = [&](const char* key, bool& field) {
        if (root.has(key)) field = root[key].as_bool(field);
    };
    auto text = [&](const char* key, std::string& field) {
        if (root.has(key)) field = root[key].as_string(field);
    };

    num("default_interval", s.default_interval);
    num("request_delay_min", s.request_delay_min);
    num("request_delay_max", s.request_delay_max);
    num("max_feed_items", s.max_feed_items);
    num("keep_history_days", s.keep_history_days);

    flag("minimize_to_tray", s.minimize_to_tray);
    flag("start_minimized", s.start_minimized);
    flag("autostart", s.autostart);
    flag("autostart_watching", s.autostart_watching);

    text("proxy", s.proxy);
    flag("browser_fallback", s.browser_fallback);
    flag("browser_first", s.browser_first);
    num("request_timeout", s.request_timeout);

    flag("notify_desktop", s.notify_desktop);
    flag("notify_sound", s.notify_sound);

    flag("telegram_enabled", s.telegram_enabled);
    text("telegram_token", s.telegram_token);
    text("telegram_chat_id", s.telegram_chat_id);
    flag("telegram_with_photo", s.telegram_with_photo);

    flag("email_enabled", s.email_enabled);
    text("smtp_host", s.smtp_host);
    num("smtp_port", s.smtp_port);
    flag("smtp_ssl", s.smtp_ssl);
    text("smtp_user", s.smtp_user);
    text("smtp_password", s.smtp_password);
    text("email_to", s.email_to);

    text("window_placement", s.window_placement);

    s.telegram_token = decrypt_secret(s.telegram_token);
    s.smtp_password = decrypt_secret(s.smtp_password);
    return s;
}

Json to_json(const Settings& s) {
    Json root = Json::object();
    root["default_interval"] = Json(s.default_interval);
    root["request_delay_min"] = Json(s.request_delay_min);
    root["request_delay_max"] = Json(s.request_delay_max);
    root["max_feed_items"] = Json(s.max_feed_items);
    root["keep_history_days"] = Json(s.keep_history_days);

    root["minimize_to_tray"] = Json(s.minimize_to_tray);
    root["start_minimized"] = Json(s.start_minimized);
    root["autostart"] = Json(s.autostart);
    root["autostart_watching"] = Json(s.autostart_watching);

    root["proxy"] = Json(s.proxy);
    root["browser_fallback"] = Json(s.browser_fallback);
    root["browser_first"] = Json(s.browser_first);
    root["request_timeout"] = Json(s.request_timeout);

    root["notify_desktop"] = Json(s.notify_desktop);
    root["notify_sound"] = Json(s.notify_sound);

    root["telegram_enabled"] = Json(s.telegram_enabled);
    root["telegram_token"] = Json(encrypt_secret(s.telegram_token));
    root["telegram_chat_id"] = Json(s.telegram_chat_id);
    root["telegram_with_photo"] = Json(s.telegram_with_photo);

    root["email_enabled"] = Json(s.email_enabled);
    root["smtp_host"] = Json(s.smtp_host);
    root["smtp_port"] = Json(s.smtp_port);
    root["smtp_ssl"] = Json(s.smtp_ssl);
    root["smtp_user"] = Json(s.smtp_user);
    root["smtp_password"] = Json(encrypt_secret(s.smtp_password));
    root["email_to"] = Json(s.email_to);

    root["window_placement"] = Json(s.window_placement);
    return root;
}

}  // namespace

SettingsStore::SettingsStore() {
    std::string text;
    if (util::read_file(util::settings_path(), text)) {
        settings_ = from_json(Json::parse(text));
    }
}

Settings SettingsStore::get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_;
}

void SettingsStore::set(const Settings& value) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings_ = value;
    }
    save();
}

bool SettingsStore::save() const {
    Settings copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        copy = settings_;
    }
    return util::write_file_atomic(util::settings_path(), to_json(copy).dump(2));
}

SettingsStore& settings() {
    static SettingsStore store;
    return store;
}

bool smtp_preset_for(const std::string& address, std::string& host, int& port, bool& ssl) {
    static const std::map<std::string, std::tuple<const char*, int, bool>> kPresets = {
        {"gmail.com",   {"smtp.gmail.com", 465, true}},
        {"yandex.ru",   {"smtp.yandex.ru", 465, true}},
        {"ya.ru",       {"smtp.yandex.ru", 465, true}},
        {"mail.ru",     {"smtp.mail.ru", 465, true}},
        {"bk.ru",       {"smtp.mail.ru", 465, true}},
        {"inbox.ru",    {"smtp.mail.ru", 465, true}},
        {"list.ru",     {"smtp.mail.ru", 465, true}},
        {"outlook.com", {"smtp-mail.outlook.com", 587, false}},
        {"hotmail.com", {"smtp-mail.outlook.com", 587, false}},
        {"rambler.ru",  {"smtp.rambler.ru", 465, true}},
    };

    size_t at = address.rfind('@');
    if (at == std::string::npos) return false;
    std::string domain = util::to_lower(util::trim(address.substr(at + 1)));
    auto it = kPresets.find(domain);
    if (it == kPresets.end()) return false;
    host = std::get<0>(it->second);
    port = std::get<1>(it->second);
    ssl = std::get<2>(it->second);
    return true;
}

}  // namespace core
