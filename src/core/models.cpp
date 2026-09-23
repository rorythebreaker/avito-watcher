#include "core/models.h"

#include <windows.h>

#include "util/strings.h"

namespace core {

const char* kind_title(TaskKind kind) {
    return kind == TaskKind::Similar ? "Похожие" : "По запросу";
}

const char* status_title(TaskStatus status) {
    switch (status) {
        case TaskStatus::Running: return "Проверка…";
        case TaskStatus::Ok: return "Работает";
        case TaskStatus::Blocked: return "Авито блокирует";
        case TaskStatus::Error: return "Ошибка";
        default: return "Ожидание";
    }
}

std::string kind_to_string(TaskKind kind) {
    return kind == TaskKind::Similar ? "similar" : "search";
}

TaskKind kind_from_string(const std::string& text) {
    return text == "similar" ? TaskKind::Similar : TaskKind::Search;
}

std::vector<std::string> Task::exclude_words() const {
    std::vector<std::string> words;
    for (const std::string& part : util::split(exclude, ',')) {
        std::string word = util::to_lower(util::trim(part));
        if (!word.empty()) words.push_back(word);
    }
    return words;
}

std::string Listing::price_label() const {
    if (!price_text.empty()) return price_text;
    if (price > 0) return util::format_thousands(price) + " \xE2\x82\xBD";  // ₽
    return "Цена не указана";
}

double now_seconds() {
    FILETIME file_time;
    GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER value;
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    // FILETIME counts 100-nanosecond ticks since 1601-01-01.
    const double kTicksPerSecond = 10000000.0;
    const double kEpochOffset = 11644473600.0;
    return static_cast<double>(value.QuadPart) / kTicksPerSecond - kEpochOffset;
}

}  // namespace core
