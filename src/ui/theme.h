// Colours, fonts and small drawing helpers for the dark interface.
//
// Everything is painted by hand with GDI, so the look does not depend on the
// system theme and stays the same on every Windows build.
#pragma once

#include <windows.h>

#include <string>

namespace ui {

namespace color {
constexpr COLORREF kWindow      = RGB(0x13, 0x17, 0x20);
constexpr COLORREF kPanel       = RGB(0x17, 0x1b, 0x21);
constexpr COLORREF kBar         = RGB(0x1b, 0x20, 0x28);
constexpr COLORREF kCard        = RGB(0x1e, 0x22, 0x28);
constexpr COLORREF kCardHover   = RGB(0x25, 0x2b, 0x33);
constexpr COLORREF kCardActive  = RGB(0x24, 0x30, 0x40);
constexpr COLORREF kBorder      = RGB(0x2e, 0x34, 0x3d);
constexpr COLORREF kAccent      = RGB(0x00, 0xaa, 0xff);
constexpr COLORREF kAccentDark  = RGB(0x00, 0x77, 0xcc);
constexpr COLORREF kText        = RGB(0xe8, 0xec, 0xf1);
constexpr COLORREF kTextMuted   = RGB(0x8a, 0x93, 0xa0);
constexpr COLORREF kTextDim     = RGB(0x6f, 0x78, 0x85);
constexpr COLORREF kPrice       = RGB(0x4a, 0xde, 0x80);
constexpr COLORREF kWarning     = RGB(0xff, 0xb5, 0x45);
constexpr COLORREF kError       = RGB(0xff, 0x6b, 0x5c);
constexpr COLORREF kInput       = RGB(0x12, 0x16, 0x1c);
constexpr COLORREF kButton      = RGB(0x26, 0x2d, 0x37);
constexpr COLORREF kButtonHover = RGB(0x2f, 0x38, 0x44);
}  // namespace color

// The DPI every layout constant and font is measured against. The window sets
// it once it knows which monitor it is on, and again on WM_DPICHANGED.
void set_ui_dpi(unsigned dpi);
unsigned ui_dpi();

// Converts a length written for a 96 DPI screen into device pixels.
int scale(int value);

// DPI of the monitor the window is on, 96 when it cannot be determined.
unsigned window_dpi(HWND window);

// Fonts are created once and shared; never delete the returned handles.
HFONT font(int point_size, bool bold = false);
HFONT font_ui();        // 9 pt regular
HFONT font_ui_bold();   // 9 pt bold
HFONT font_small();     // 8 pt regular
HFONT font_title();     // 10 pt bold

// Solid brushes kept alive for the whole run.
HBRUSH brush(COLORREF color);

void fill_rect(HDC dc, const RECT& rect, COLORREF fill);
void fill_round_rect(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border);
void draw_pill(HDC dc, const RECT& rect, const std::wstring& text, COLORREF back,
               COLORREF fore);

// Draws text clipped to the rectangle. `format` takes DT_* flags.
void draw_text(HDC dc, const RECT& rect, const std::wstring& text, COLORREF text_color,
               HFONT text_font, UINT format);

// Shortens the text with an ellipsis so it fits the given width.
std::wstring elide(HDC dc, const std::wstring& text, int width, HFONT text_font);

int text_width(HDC dc, const std::wstring& text, HFONT text_font);

// Scales a value from 96 DPI to the DPI of the given window.
int dpi_scale(HWND window, int value);

// Switches the process into dark mode. Until this is done the dark common
// control themes are ignored and scrollbars, list rows and menus keep their
// light colours whatever SetWindowTheme is asked for. Must be called before
// the first window is created.
void enable_dark_mode_for_app();

// Paints the window's title bar dark. Supported from Windows 10 2004; on older
// builds the call is simply ignored.
void enable_dark_titlebar(HWND window);

// Asks the common controls to use their dark scrollbars and borders.
// `theme` picks the variant: the default suits edits and scrolling panes,
// while list and tree views want DarkMode_ItemsView, whose explorer
// counterpart would draw column separators across the empty area.
void enable_dark_control(HWND control, const wchar_t* theme = L"DarkMode_Explorer");

// Replaces a combo box's system-drawn frame and arrow with our own, so the
// control stops showing a light border on the dark background.
void make_dark_combo(HWND combo);

// Draws the frame we put around text fields, which have no border of their own.
void draw_field_frame(HDC dc, const RECT& field, bool focused);

}  // namespace ui
