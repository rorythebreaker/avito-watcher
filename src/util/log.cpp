#include "util/log.h"

#include <windows.h>

#include <cstdio>
#include <mutex>

#include "util/paths.h"
#include "util/strings.h"

namespace util {
namespace {

constexpr long long kMaxLogBytes = 1024 * 1024;

std::mutex& log_mutex() {
    static std::mutex mutex;
    return mutex;
}

const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Warning: return "WARNING";
        case LogLevel::Error: return "ERROR";
        default: return "INFO";
    }
}

std::string timestamp() {
    SYSTEMTIME now;
    GetLocalTime(&now);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
                  now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    return buffer;
}

// Keeps one previous file so a long-running session cannot fill the disk.
void rotate_if_needed(const std::wstring& path) {
    if (file_size(path) < kMaxLogBytes) return;
    std::wstring previous = path + L".1";
    DeleteFileW(previous.c_str());
    MoveFileW(path.c_str(), previous.c_str());
}

}  // namespace

void log_init() {
    std::lock_guard<std::mutex> lock(log_mutex());
    rotate_if_needed(log_path());
}

void log_write(LogLevel level, std::string_view source, std::string_view message) {
    std::lock_guard<std::mutex> lock(log_mutex());
    const std::wstring path = log_path();
    rotate_if_needed(path);

    std::string line = timestamp();
    line += "  ";
    line += level_name(level);
    line += "  ";
    line.append(source);
    line += ": ";
    line.append(message);
    line += "\r\n";

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}

std::string clock_string() {
    SYSTEMTIME now;
    GetLocalTime(&now);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", now.wHour, now.wMinute, now.wSecond);
    return buffer;
}

}  // namespace util
