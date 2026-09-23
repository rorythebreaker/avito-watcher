// Rotating text log written to %APPDATA%\AvitoWatcher\watcher.log.
#pragma once

#include <string>
#include <string_view>

namespace util {

enum class LogLevel { Info, Warning, Error };

void log_init();
void log_write(LogLevel level, std::string_view source, std::string_view message);

inline void log_info(std::string_view source, std::string_view message) {
    log_write(LogLevel::Info, source, message);
}
inline void log_warn(std::string_view source, std::string_view message) {
    log_write(LogLevel::Warning, source, message);
}
inline void log_error(std::string_view source, std::string_view message) {
    log_write(LogLevel::Error, source, message);
}

// Human readable "HH:MM:SS" for the in-app journal.
std::string clock_string();

}  // namespace util
