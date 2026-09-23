#include "util/paths.h"

#include <windows.h>
#include <shlobj.h>

#include "util/strings.h"

namespace util {
namespace {

std::wstring join_path(const std::wstring& base, const std::wstring& leaf) {
    if (base.empty()) return leaf;
    if (base.back() == L'\\' || base.back() == L'/') return base + leaf;
    return base + L"\\" + leaf;
}

std::wstring roaming_dir() {
    PWSTR raw = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw))) {
        result.assign(raw);
        CoTaskMemFree(raw);
    }
    if (result.empty()) {
        wchar_t buffer[MAX_PATH] = {};
        DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, MAX_PATH);
        if (length > 0 && length < MAX_PATH) result.assign(buffer, length);
    }
    return result;
}

}  // namespace

std::wstring data_dir() {
    static std::wstring cached = [] {
        std::wstring path = join_path(roaming_dir(), L"AvitoWatcher");
        ensure_dir(path);
        return path;
    }();
    return cached;
}

std::wstring cache_dir() {
    static std::wstring cached = [] {
        std::wstring path = join_path(data_dir(), L"cache");
        ensure_dir(path);
        return path;
    }();
    return cached;
}

std::wstring image_cache_dir() {
    static std::wstring cached = [] {
        std::wstring path = join_path(cache_dir(), L"img");
        ensure_dir(path);
        return path;
    }();
    return cached;
}

std::wstring browser_profile_dir() {
    return join_path(cache_dir(), L"browser-profile");
}

std::wstring settings_path()  { return join_path(data_dir(), L"settings.json"); }
std::wstring tasks_path()     { return join_path(data_dir(), L"tasks.json"); }
std::wstring listings_path()  { return join_path(data_dir(), L"listings.json"); }
std::wstring seen_path()      { return join_path(data_dir(), L"seen.txt"); }
std::wstring cookies_path()   { return join_path(cache_dir(), L"cookies.json"); }
std::wstring log_path()       { return join_path(data_dir(), L"watcher.log"); }

std::wstring executable_path() {
    wchar_t buffer[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::wstring(buffer, length);
}

bool ensure_dir(const std::wstring& path) {
    if (path.empty()) return false;
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS) return true;
    if (error == ERROR_PATH_NOT_FOUND) {
        size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos && slash > 0) {
            if (!ensure_dir(path.substr(0, slash))) return false;
            return CreateDirectoryW(path.c_str(), nullptr) ||
                   GetLastError() == ERROR_ALREADY_EXISTS;
        }
    }
    return false;
}

bool file_exists(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

long long file_size(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return -1;
    return (static_cast<long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

bool delete_file(const std::wstring& path) {
    return DeleteFileW(path.c_str()) != 0;
}

bool read_file(const std::wstring& path, std::string& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    out.clear();
    char buffer[64 * 1024];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        out.append(buffer, read);
    }
    CloseHandle(file);

    // Strip a UTF-8 byte order mark if some editor added one.
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

bool write_file(const std::wstring& path, const void* data, size_t size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const char* cursor = static_cast<const char*>(data);
    size_t left = size;
    bool ok = true;
    while (left > 0) {
        DWORD chunk = static_cast<DWORD>(left > 1u << 20 ? 1u << 20 : left);
        DWORD written = 0;
        if (!WriteFile(file, cursor, chunk, &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        cursor += written;
        left -= written;
    }
    CloseHandle(file);
    return ok;
}

bool write_file_atomic(const std::wstring& path, std::string_view data) {
    // Write beside the target and rename, so a crash mid-write cannot leave a
    // half-written settings or task file behind.
    std::wstring temp = path + L".tmp";
    if (!write_file(temp, data.data(), data.size())) return false;
    if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(temp.c_str());
    return false;
}

}  // namespace util
