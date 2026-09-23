#include "ui/theme.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <map>
#include <mutex>
#include <string>

namespace ui {
namespace {

std::mutex& cache_mutex() {
    static std::mutex mutex;
    return mutex;
}

unsigned g_dpi = 96;

}  // namespace

void set_ui_dpi(unsigned dpi) {
    if (dpi >= 48 && dpi <= 960) g_dpi = dpi;
}

unsigned ui_dpi() { return g_dpi; }

int scale(int value) {
    return MulDiv(value, static_cast<int>(g_dpi), 96);
}

unsigned window_dpi(HWND window) {
    // GetDpiForWindow exists from Windows 10 1607; older systems have one
    // DPI for the whole desktop, which the screen DC reports.
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn resolver = []() -> GetDpiForWindowFn {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<GetDpiForWindowFn>(
                            GetProcAddress(user32, "GetDpiForWindow"))
                      : nullptr;
    }();
    if (window && resolver) {
        UINT dpi = resolver(window);
        if (dpi >= 48) return dpi;
    }
    HDC screen = GetDC(nullptr);
    UINT dpi = static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSX));
    ReleaseDC(nullptr, screen);
    return dpi >= 48 ? dpi : 96;
}

HFONT font(int point_size, bool bold) {
    std::lock_guard<std::mutex> lock(cache_mutex());
    // Keyed by DPI as well: the same point size is a different pixel height on
    // a scaled monitor, and a window can move between two of them.
    static std::map<std::tuple<int, bool, unsigned>, HFONT> cache;

    auto key = std::make_tuple(point_size, bold, g_dpi);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    int height = -MulDiv(point_size, static_cast<int>(g_dpi), 72);

    LOGFONTW description = {};
    description.lfHeight = height;
    description.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
    description.lfCharSet = DEFAULT_CHARSET;
    description.lfQuality = CLEARTYPE_QUALITY;
    description.lfOutPrecision = OUT_TT_PRECIS;
    wcscpy_s(description.lfFaceName, L"Segoe UI");

    HFONT created = CreateFontIndirectW(&description);
    cache[key] = created;
    return created;
}

HFONT font_ui() { return font(9, false); }
HFONT font_ui_bold() { return font(9, true); }
HFONT font_small() { return font(8, false); }
HFONT font_title() { return font(10, true); }

HBRUSH brush(COLORREF color) {
    std::lock_guard<std::mutex> lock(cache_mutex());
    static std::map<COLORREF, HBRUSH> cache;
    auto it = cache.find(color);
    if (it != cache.end()) return it->second;
    HBRUSH created = CreateSolidBrush(color);
    cache[color] = created;
    return created;
}

void fill_rect(HDC dc, const RECT& rect, COLORREF fill) {
    FillRect(dc, &rect, brush(fill));
}

void fill_round_rect(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border) {
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, brush(fill));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

void draw_pill(HDC dc, const RECT& rect, const std::wstring& text, COLORREF back,
               COLORREF fore) {
    fill_round_rect(dc, rect, (rect.bottom - rect.top) / 2, back, back);
    draw_text(dc, rect, text, fore, font_small(), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void draw_text(HDC dc, const RECT& rect, const std::wstring& text, COLORREF text_color,
               HFONT text_font, UINT format) {
    if (text.empty()) return;
    HGDIOBJ old_font = SelectObject(dc, text_font);
    int old_mode = SetBkMode(dc, TRANSPARENT);
    COLORREF old_color = SetTextColor(dc, text_color);

    RECT bounds = rect;
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &bounds, format);

    SetTextColor(dc, old_color);
    SetBkMode(dc, old_mode);
    SelectObject(dc, old_font);
}

int text_width(HDC dc, const std::wstring& text, HFONT text_font) {
    HGDIOBJ old_font = SelectObject(dc, text_font);
    SIZE size = {};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, old_font);
    return size.cx;
}

std::wstring elide(HDC dc, const std::wstring& text, int width, HFONT text_font) {
    if (text.empty() || width <= 0) return {};
    if (text_width(dc, text, text_font) <= width) return text;

    const std::wstring ellipsis = L"…";
    int ellipsis_width = text_width(dc, ellipsis, text_font);
    if (ellipsis_width > width) return {};

    // Binary search for the longest prefix that still fits.
    size_t low = 0;
    size_t high = text.size();
    while (low < high) {
        size_t middle = (low + high + 1) / 2;
        if (text_width(dc, text.substr(0, middle), text_font) + ellipsis_width <= width) {
            low = middle;
        } else {
            high = middle - 1;
        }
    }
    return text.substr(0, low) + ellipsis;
}

int dpi_scale(HWND window, int value) {
    UINT dpi = 96;
    if (window) {
        // GetDpiForWindow exists from Windows 10 1607; fall back to the desktop DC.
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        static GetDpiForWindowFn resolver = []() -> GetDpiForWindowFn {
            HMODULE user32 = GetModuleHandleW(L"user32.dll");
            return user32 ? reinterpret_cast<GetDpiForWindowFn>(
                                GetProcAddress(user32, "GetDpiForWindow"))
                          : nullptr;
        }();
        if (resolver) dpi = resolver(window);
    }
    if (dpi == 0) {
        HDC screen = GetDC(nullptr);
        dpi = static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSX));
        ReleaseDC(nullptr, screen);
    }
    return MulDiv(value, static_cast<int>(dpi), 96);
}


void enable_dark_mode_for_app() {
    // uxtheme exports these by ordinal only. They are undocumented, which is why
    // everything here is resolved at run time and simply skipped when missing:
    // the app then looks as it did before, with light scrollbars.
    enum PreferredAppMode { kDefault = 0, kAllowDark = 1, kForceDark = 2 };
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();

    HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;

    auto set_mode = reinterpret_cast<SetPreferredAppModeFn>(
        GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
    auto flush = reinterpret_cast<FlushMenuThemesFn>(
        GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));

    if (set_mode) set_mode(kForceDark);
    if (flush) flush();
}

void enable_dark_titlebar(HWND window) {
    if (!window) return;
    // DWMWA_USE_IMMERSIVE_DARK_MODE. Attribute 20 since Windows 10 2004; the
    // earlier builds that shipped it used 19, so try both and ignore failures.
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(window, 19, &dark, sizeof(dark));
}

void enable_dark_control(HWND control, const wchar_t* theme) {
    if (!control) return;
    // The dark variants of the common control themes give us dark scrollbars
    // and borders. Unknown theme names are ignored, so this is safe everywhere.
    SetWindowTheme(control, theme, nullptr);
}

void draw_field_frame(HDC dc, const RECT& field, bool focused) {
    RECT frame = field;
    InflateRect(&frame, 1, 1);
    HPEN pen = CreatePen(PS_SOLID, 1, focused ? color::kAccent : RGB(0x33, 0x3c, 0x48));
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, frame.left, frame.top, frame.right, frame.bottom, 8, 8);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

namespace {

// A drop-down list draws its own frame and arrow in the system colours, which
// no amount of owner drawing of the items can change. Painting the closed state
// ourselves is the only way to keep it dark.
LRESULT CALLBACK combo_subclass(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                UINT_PTR id, DWORD_PTR data) {
    (void)id;
    (void)data;

    switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            HDC dc = BeginPaint(window, &ps);

            RECT client = {};
            GetClientRect(window, &client);

            const bool focused = GetFocus() == window;
            const bool disabled = !IsWindowEnabled(window);

            fill_round_rect(dc, client, 7, color::kInput,
                            focused ? color::kAccent : RGB(0x33, 0x3c, 0x48));

            // Current selection.
            std::wstring text;
            int selected = static_cast<int>(SendMessageW(window, CB_GETCURSEL, 0, 0));
            if (selected >= 0) {
                int length = static_cast<int>(SendMessageW(window, CB_GETLBTEXTLEN, selected, 0));
                if (length > 0) {
                    text.resize(static_cast<size_t>(length) + 1);
                    SendMessageW(window, CB_GETLBTEXT, selected,
                                 reinterpret_cast<LPARAM>(text.data()));
                    text.resize(static_cast<size_t>(length));
                }
            }

            RECT text_area = client;
            text_area.left += 10;
            text_area.right -= 26;
            draw_text(dc, text_area, elide(dc, text, text_area.right - text_area.left, font_ui()),
                      disabled ? color::kTextDim : color::kText, font_ui(),
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Chevron.
            const int cx = client.right - 15;
            const int cy = (client.top + client.bottom) / 2 - 1;
            HPEN pen = CreatePen(PS_SOLID, 2, disabled ? color::kTextDim : color::kTextMuted);
            HGDIOBJ old_pen = SelectObject(dc, pen);
            MoveToEx(dc, cx - 4, cy - 2, nullptr);
            LineTo(dc, cx, cy + 2);
            LineTo(dc, cx + 5, cy - 3);
            SelectObject(dc, old_pen);
            DeleteObject(pen);

            EndPaint(window, &ps);
            return 0;
        }

        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(window, nullptr, TRUE);
            break;

        case WM_NCDESTROY:
            RemoveWindowSubclass(window, combo_subclass, 1);
            break;

        default:
            break;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

}  // namespace

void make_dark_combo(HWND combo) {
    if (!combo) return;
    enable_dark_control(combo);
    SetWindowSubclass(combo, combo_subclass, 1, 0);
}

}  // namespace ui
