#include "ui/theme.h"

#include <map>
#include <mutex>

namespace ui {
namespace {

std::mutex& cache_mutex() {
    static std::mutex mutex;
    return mutex;
}

}  // namespace

HFONT font(int point_size, bool bold) {
    std::lock_guard<std::mutex> lock(cache_mutex());
    static std::map<std::pair<int, bool>, HFONT> cache;

    auto key = std::make_pair(point_size, bold);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    HDC screen = GetDC(nullptr);
    int height = -MulDiv(point_size, GetDeviceCaps(screen, LOGPIXELSY), 72);
    ReleaseDC(nullptr, screen);

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

}  // namespace ui
