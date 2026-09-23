#include "core/autostart.h"

#include <windows.h>

#include <string>

#include "util/paths.h"

namespace core {
namespace {

constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kValueName = L"AvitoWatcher";

// Started from the Run key the app should come up already in the tray.
std::wstring run_command() {
    return L"\"" + util::executable_path() + L"\" --tray";
}

}  // namespace

bool autostart_enabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t buffer[1024] = {};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    LSTATUS status = RegQueryValueExW(key, kValueName, nullptr, &type,
                                      reinterpret_cast<LPBYTE>(buffer), &size);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_SZ && buffer[0] != L'\0';
}

bool set_autostart(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LSTATUS status;
    if (enabled) {
        std::wstring command = run_command();
        status = RegSetValueExW(key, kValueName, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, kValueName);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

}  // namespace core
