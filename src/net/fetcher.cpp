#include "net/fetcher.h"

#include <windows.h>

#include <chrono>
#include <random>
#include <thread>

#include "net/browser.h"
#include "util/log.h"
#include "util/paths.h"
#include "util/strings.h"

namespace net {
namespace {

// Titles Avito puts on the pages it serves instead of real results.
const char* kBlockTitles[] = {
    "доступ ограничен",
    "проблема с ip",
    "проверка безопасности",
    "вы не робот",
    "подтвердите, что вы не робот",
    "access denied",
    "attention required",
};

const char* kEmptyMarkers[] = {
    "ничего не найдено",
    "не найдено ни одного объявления",
};

const Headers& avito_headers() {
    static const Headers headers = {
        {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,"
                   "image/avif,image/webp,*/*;q=0.8"},
        {"Accept-Language", "ru-RU,ru;q=0.9,en-US;q=0.8,en;q=0.7"},
        {"Upgrade-Insecure-Requests", "1"},
        {"Sec-Fetch-Dest", "document"},
        {"Sec-Fetch-Mode", "navigate"},
        {"Sec-Fetch-Site", "same-origin"},
        {"Sec-Fetch-User", "?1"},
        {"Cache-Control", "max-age=0"},
        {"Referer", "https://www.avito.ru/"},
    };
    return headers;
}

double monotonic_seconds() {
    return static_cast<double>(GetTickCount64()) / 1000.0;
}

}  // namespace

std::string page_title(const std::string& html) {
    size_t open = util::to_lower(html.substr(0, std::min<size_t>(html.size(), 200000))).find("<title");
    if (open == std::string::npos) return {};
    size_t close = html.find('>', open);
    if (close == std::string::npos) return {};
    size_t end = html.find("</title", close);
    if (end == std::string::npos) return {};
    return util::to_lower(util::trim(html.substr(close + 1, end - close - 1)));
}

bool is_empty_result(const std::string& html) {
    std::string lowered = util::to_lower(html);
    for (const char* marker : kEmptyMarkers) {
        if (lowered.find(marker) != std::string::npos) return true;
    }
    return false;
}

bool looks_blocked(const std::string& html, int status, const std::string& require) {
    if (status == 403 || status == 429 || status == 503) return true;

    std::string title = page_title(html);
    for (const char* marker : kBlockTitles) {
        if (title.find(marker) != std::string::npos) return true;
    }
    // A genuine "nothing found" page carries no listings and that is fine.
    if (is_empty_result(html)) return false;
    if (!require.empty() && html.find(require) == std::string::npos) return true;
    return html.size() < 5000;
}

Fetcher::Fetcher() {
    std::string cookies;
    if (util::read_file(util::cookies_path(), cookies)) client_.load_cookies(cookies);
}

void Fetcher::apply_settings(const core::Settings& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = value;
    client_.set_proxy(value.proxy);
    client_.set_timeout_seconds(value.request_timeout);
}

void Fetcher::wait_turn(const std::atomic<bool>* stop) {
    double low = settings_.request_delay_min;
    double high = settings_.request_delay_max;
    if (low < 0.5) low = 0.5;
    if (high < low + 0.1) high = low + 0.1;

    static thread_local std::mt19937 generator{std::random_device{}()};
    std::uniform_real_distribution<double> distribution(low, high);
    double gap = distribution(generator);

    double wait_until = last_request_ + gap;
    while (monotonic_seconds() < wait_until) {
        if (stop && stop->load()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    last_request_ = monotonic_seconds();
}

FetchResult Fetcher::get(const std::string& url, const std::string& require,
                         const std::atomic<bool>* stop) {
    FetchResult result;

    core::Settings current;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current = settings_;
        wait_turn(stop);
    }
    if (stop && stop->load()) {
        result.error = "Проверка отменена";
        return result;
    }

    std::string http_error;
    if (!current.browser_first) {
        Response response = client_.get(url, avito_headers());
        if (response.ok) {
            if (!looks_blocked(response.body, response.status, require)) {
                result.status = FetchStatus::Ok;
                result.html = std::move(response.body);
                result.transport = "http";
                last_transport_ = "http";
                util::write_file_atomic(util::cookies_path(), client_.dump_cookies());
                return result;
            }
        } else {
            http_error = response.error;
            util::log_warn("fetcher", "HTTP-запрос не удался: " + http_error);
        }
    }

    if (!current.browser_fallback) {
        result.status = http_error.empty() ? FetchStatus::Blocked : FetchStatus::Failed;
        result.error = http_error.empty()
                           ? "Авито не отдал страницу (защита от ботов). "
                             "Включите запасной браузер в настройках."
                           : "Не удалось загрузить страницу: " + http_error;
        return result;
    }

    util::log_info("fetcher", "Переключаюсь на браузер для " + url);
    BrowserResult page = browser_fetch(url, std::max(60, current.request_timeout * 2),
                                       current.proxy);
    if (!page.ok) {
        result.status = FetchStatus::Blocked;
        result.error = http_error.empty()
                           ? "Авито блокирует запросы, браузер тоже не помог: " + page.error
                           : "HTTP: " + http_error + "; браузер: " + page.error;
        return result;
    }

    if (looks_blocked(page.html, 200, require)) {
        result.status = FetchStatus::Blocked;
        result.error =
            "Авито показывает проверку на робота. Сделайте паузу, "
            "увеличьте интервал проверки или укажите прокси в настройках.";
        return result;
    }

    result.status = FetchStatus::Ok;
    result.html = std::move(page.html);
    result.transport = "browser";
    last_transport_ = "browser";
    return result;
}

}  // namespace net
