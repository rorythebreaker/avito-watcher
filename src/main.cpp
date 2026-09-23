// Entry point.
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <gdiplus.h>

#include <memory>

#include "core/config.h"
#include "core/autostart.h"
#include "core/store.h"
#include "ui/main_window.h"
#include "ui/theme.h"
#include "util/log.h"
#include "util/paths.h"
#include "util/strings.h"

namespace {

constexpr const wchar_t* kMutexName = L"Local\\AvitoWatcherSingleInstance";
constexpr const wchar_t* kShowMessage = L"AvitoWatcherShowWindow";

bool has_flag(int argc, wchar_t** argv, const wchar_t* flag) {
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], flag) == 0) return true;
    }
    return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    // A second copy should just bring the running one to the front.
    const UINT show_message = RegisterWindowMessageW(kShowMessage);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        PostMessageW(HWND_BROADCAST, show_message, 0, 0);
        CloseHandle(mutex);
        return 0;
    }

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool start_in_tray = argv && has_flag(argc, argv, L"--tray");
    if (argv) LocalFree(argv);

    util::log_init();
    util::log_info("app", "Запуск Avito Watcher 1.0.0");

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES |
                     ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    // Has to happen before any window exists, or the controls created
    // first keep their light theme.
    ui::enable_dark_mode_for_app();

    Gdiplus::GdiplusStartupInput gdiplus_input;
    ULONG_PTR gdiplus_token = 0;
    Gdiplus::GdiplusStartup(&gdiplus_token, &gdiplus_input, nullptr);

    const core::Settings settings = core::settings().get();

    // The registry entry may have drifted from the setting, for instance when
    // the executable was moved; bring it back in line.
    if (core::autostart_enabled() != settings.autostart) {
        core::set_autostart(settings.autostart);
    }

    int exit_code = 0;
    {
        auto window = std::make_unique<ui::MainWindow>();
        if (!window->create(instance, start_in_tray)) {
            MessageBoxW(nullptr, L"Не удалось создать окно приложения.", L"Avito Watcher",
                        MB_OK | MB_ICONERROR);
            exit_code = 1;
        } else {
            window->log("Avito Watcher 1.0.0 готов к работе");
            if (core::store().tasks().empty()) {
                window->log("Создайте задачу кнопкой «Новая задача» на панели сверху");
            } else {
                window->start_if_configured();
            }

            MSG message;
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                if (message.message == show_message) {
                    window->restore();
                    continue;
                }
                if (!IsDialogMessageW(window->handle(), &message)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            exit_code = static_cast<int>(message.wParam);
        }
    }

    Gdiplus::GdiplusShutdown(gdiplus_token);
    CoUninitialize();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return exit_code;
}
