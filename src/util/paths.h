// Where the app keeps its files. Everything lives under %APPDATA%\AvitoWatcher.
#pragma once

#include <string>
#include <vector>

namespace util {

std::wstring data_dir();
std::wstring cache_dir();
std::wstring image_cache_dir();
std::wstring browser_profile_dir();

std::wstring settings_path();
std::wstring tasks_path();
std::wstring listings_path();
std::wstring seen_path();
std::wstring cookies_path();
std::wstring log_path();

std::wstring executable_path();

bool read_file(const std::wstring& path, std::string& out);
bool write_file_atomic(const std::wstring& path, std::string_view data);
bool write_file(const std::wstring& path, const void* data, size_t size);
bool file_exists(const std::wstring& path);
bool ensure_dir(const std::wstring& path);
bool delete_file(const std::wstring& path);
long long file_size(const std::wstring& path);

}  // namespace util
