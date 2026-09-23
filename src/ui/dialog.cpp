#include "ui/dialog.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>

#include "ui/icon.h"
#include "ui/theme.h"
#include "util/strings.h"

namespace ui {
namespace {

constexpr const wchar_t* kClassName = L"AvitoWatcherDialog";
bool g_class_registered = false;

void ensure_class(HINSTANCE instance, WNDPROC proc) {
    if (g_class_registered) return;
    WNDCLASSEXW description = {};
    description.cbSize = sizeof(description);
    description.lpfnWndProc = proc;
    description.hInstance = instance;
    description.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    description.hbrBackground = brush(color::kWindow);
    description.lpszClassName = kClassName;
    RegisterClassExW(&description);
    g_class_registered = true;
}


// The common trackbar has no dark variant: its track stays white and its thumb
// keeps the system accent shape, which is glaring next to everything else. This
// is a minimal replacement - a track, a filled part and a round knob - that
// reports changes the same way through WM_HSCROLL.
constexpr const wchar_t* kSliderClass = L"AvitoWatcherSlider";

struct SliderState {
    int low = 0;
    int high = 100;
    int value = 0;
    bool dragging = false;
    bool hovered = false;
};

SliderState* slider_state(HWND window) {
    return reinterpret_cast<SliderState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

int slider_knob_radius() { return scale(8); }

void slider_set_value(HWND window, int value, bool notify) {
    SliderState* state = slider_state(window);
    if (!state) return;
    if (value < state->low) value = state->low;
    if (value > state->high) value = state->high;
    if (value == state->value) return;
    state->value = value;
    InvalidateRect(window, nullptr, FALSE);
    if (notify) {
        SendMessageW(GetParent(window), WM_HSCROLL,
                     MAKEWPARAM(TB_THUMBTRACK, static_cast<WORD>(value)),
                     reinterpret_cast<LPARAM>(window));
    }
}

int slider_value_at(HWND window, int x) {
    SliderState* state = slider_state(window);
    RECT client = {};
    GetClientRect(window, &client);
    const int radius = slider_knob_radius();
    const int left = client.left + radius;
    const int right = client.right - radius;
    if (right <= left || !state) return state ? state->low : 0;

    double ratio = static_cast<double>(x - left) / static_cast<double>(right - left);
    ratio = ratio < 0.0 ? 0.0 : (ratio > 1.0 ? 1.0 : ratio);
    return state->low + static_cast<int>(ratio * (state->high - state->low) + 0.5);
}

LRESULT CALLBACK slider_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    SliderState* state = slider_state(window);

    switch (message) {
        case WM_NCCREATE: {
            auto* created = new SliderState();
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
            break;
        }

        case WM_NCDESTROY:
            delete state;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            HDC screen = BeginPaint(window, &ps);
            RECT client = {};
            GetClientRect(window, &client);

            HDC dc = CreateCompatibleDC(screen);
            HBITMAP buffer = CreateCompatibleBitmap(screen, client.right, client.bottom);
            HGDIOBJ old_bitmap = SelectObject(dc, buffer);

            fill_rect(dc, client, color::kWindow);

            const int radius = slider_knob_radius();
            const int middle = (client.top + client.bottom) / 2;
            const int left = client.left + radius;
            const int right = client.right - radius;
            const int span = right - left;
            const int range = state && state->high > state->low ? state->high - state->low : 1;
            const int knob_x =
                state ? left + MulDiv(state->value - state->low, span, range) : left;

            RECT track = {left, middle - scale(2), right, middle + scale(2)};
            fill_round_rect(dc, track, scale(2), RGB(0x26, 0x2d, 0x37), RGB(0x26, 0x2d, 0x37));

            RECT filled = {left, track.top, knob_x, track.bottom};
            if (filled.right > filled.left) {
                fill_round_rect(dc, filled, scale(2), color::kAccent, color::kAccent);
            }

            RECT knob = {knob_x - radius, middle - radius, knob_x + radius, middle + radius};
            const bool active = state && (state->dragging || state->hovered);
            fill_round_rect(dc, knob, radius, active ? color::kAccent : color::kText,
                            active ? color::kAccent : color::kText);

            BitBlt(screen, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
            SelectObject(dc, old_bitmap);
            DeleteObject(buffer);
            DeleteDC(dc);
            EndPaint(window, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN:
            SetCapture(window);
            SetFocus(window);
            if (state) state->dragging = true;
            slider_set_value(window, slider_value_at(window, GET_X_LPARAM(lparam)), true);
            return 0;

        case WM_MOUSEMOVE: {
            if (state && !state->hovered) {
                state->hovered = true;
                TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window, 0};
                TrackMouseEvent(&track);
                InvalidateRect(window, nullptr, FALSE);
            }
            if (state && state->dragging) {
                slider_set_value(window, slider_value_at(window, GET_X_LPARAM(lparam)), true);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            if (state) {
                state->hovered = false;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            if (state) state->dragging = false;
            ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            return 0;

        case WM_KEYDOWN:
            if (!state) break;
            if (wparam == VK_LEFT || wparam == VK_DOWN) {
                slider_set_value(window, state->value - 1, true);
                return 0;
            }
            if (wparam == VK_RIGHT || wparam == VK_UP) {
                slider_set_value(window, state->value + 1, true);
                return 0;
            }
            if (wparam == VK_HOME) { slider_set_value(window, state->low, true); return 0; }
            if (wparam == VK_END) { slider_set_value(window, state->high, true); return 0; }
            break;

        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;

        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(window, nullptr, FALSE);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void ensure_slider_class(HINSTANCE instance) {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW description = {};
    description.cbSize = sizeof(description);
    description.style = CS_HREDRAW | CS_VREDRAW;
    description.lpfnWndProc = &slider_proc;
    description.hInstance = instance;
    description.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    description.hbrBackground = nullptr;
    description.lpszClassName = kSliderClass;
    RegisterClassExW(&description);
    registered = true;
}

}  // namespace

bool ModalDialog::run(HWND parent, const std::wstring& title, int width, int height) {
    parent_ = parent;
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
    if (!instance) instance = GetModuleHandleW(nullptr);
    ensure_class(instance, &ModalDialog::proc);

    RECT desired = {0, 0, scale(width), scale(height)};
    AdjustWindowRectEx(&desired, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int full_width = desired.right - desired.left;
    int full_height = desired.bottom - desired.top;

    RECT owner = {};
    GetWindowRect(parent ? parent : GetDesktopWindow(), &owner);
    int x = owner.left + ((owner.right - owner.left) - full_width) / 2;
    int y = owner.top + ((owner.bottom - owner.top) - full_height) / 2;

    window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, title.c_str(),
                              WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, full_width, full_height,
                              parent, nullptr, instance, this);
    if (!window_) return false;

    enable_dark_titlebar(window_);
    SendMessageW(window_, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(app_icon_small()));
    build();

    if (parent) EnableWindow(parent, FALSE);
    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);

    MSG message;
    while (!finished_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.hwnd == window_ || IsChild(window_, message.hwnd)) {
            if (message.message == WM_KEYDOWN) {
                if (message.wParam == VK_ESCAPE) {
                    finished_ = true;
                    continue;
                }
                if (message.wParam == VK_RETURN) {
                    // Enter inside a multi-line edit should insert a newline.
                    wchar_t class_name[32] = {};
                    GetClassNameW(message.hwnd, class_name, 32);
                    LONG_PTR style = GetWindowLongPtrW(message.hwnd, GWL_STYLE);
                    if (_wcsicmp(class_name, L"Edit") != 0 || !(style & ES_MULTILINE)) {
                        if (on_accept()) {
                            accepted_ = true;
                            finished_ = true;
                        }
                        continue;
                    }
                }
                if (message.wParam == VK_TAB) {
                    HWND next = GetNextDlgTabItem(window_, GetFocus(),
                                                  GetKeyState(VK_SHIFT) < 0);
                    if (next) {
                        SetFocus(next);
                        continue;
                    }
                }
            }
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (parent) EnableWindow(parent, TRUE);
    if (window_) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (parent) SetActiveWindow(parent);
    return accepted_;
}

LRESULT CALLBACK ModalDialog::proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    ModalDialog* self = reinterpret_cast<ModalDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<ModalDialog*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wparam, lparam);
    return self->handle(message, wparam, lparam);
}

LRESULT ModalDialog::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    LRESULT handled = 0;
    if (on_message(message, wparam, lparam, handled)) return handled;

    switch (message) {
        case WM_COMMAND: {
            int id = LOWORD(wparam);
            int notification = HIWORD(wparam);
            if (id == IDOK) {
                if (on_accept()) {
                    accepted_ = true;
                    finished_ = true;
                }
                return 0;
            }
            if (id == IDCANCEL) {
                finished_ = true;
                return 0;
            }
            if (notification == EN_SETFOCUS || notification == EN_KILLFOCUS) {
                InvalidateRect(window_, nullptr, FALSE);
            }
            HWND clicked = reinterpret_cast<HWND>(lparam);
            if (notification == BN_CLICKED && toggles_.count(clicked)) toggle_clicked(clicked);
            on_command(id, notification);
            return 0;
        }

        case WM_HSCROLL:
        case WM_VSCROLL:
            on_scroll(reinterpret_cast<HWND>(lparam));
            return 0;

        case WM_DRAWITEM:
            draw_item(reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
            return TRUE;

        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            HDC dc = BeginPaint(window_, &ps);
            for (HWND field : fields_) {
                if (!IsWindowVisible(field)) continue;
                RECT bounds = {};
                GetWindowRect(field, &bounds);
                MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&bounds), 2);
                draw_field_frame(dc, bounds, GetFocus() == field);
            }
            EndPaint(window_, &ps);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            HWND control = reinterpret_cast<HWND>(lparam);
            SetBkMode(dc, TRANSPARENT);
            COLORREF text = color::kText;
            if (std::find(hints_.begin(), hints_.end(), control) != hints_.end()) {
                text = color::kTextMuted;
            }
            if (std::find(errors_.begin(), errors_.end(), control) != errors_.end()) {
                text = color::kError;
            }
            SetTextColor(dc, text);
            return reinterpret_cast<LRESULT>(brush(color::kWindow));
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkMode(dc, OPAQUE);
            SetTextColor(dc, color::kText);
            SetBkColor(dc, color::kInput);
            return reinterpret_cast<LRESULT>(brush(color::kInput));
        }

        case WM_CLOSE:
            finished_ = true;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

void ModalDialog::draw_item(DRAWITEMSTRUCT* item) {
    if (!item) return;

    wchar_t class_name[32] = {};
    GetClassNameW(item->hwndItem, class_name, 32);
    const bool is_combo = _wcsicmp(class_name, L"ComboBox") == 0;

    std::wstring text;
    if (is_combo) {
        if (item->itemID != static_cast<UINT>(-1)) {
            int length = static_cast<int>(
                SendMessageW(item->hwndItem, CB_GETLBTEXTLEN, item->itemID, 0));
            if (length > 0) {
                text.resize(static_cast<size_t>(length));
                SendMessageW(item->hwndItem, CB_GETLBTEXT, item->itemID,
                             reinterpret_cast<LPARAM>(text.data()));
            }
        }
    } else {
        int length = GetWindowTextLengthW(item->hwndItem);
        text.resize(static_cast<size_t>(length) + 1);
        GetWindowTextW(item->hwndItem, text.data(), length + 1);
        text.resize(static_cast<size_t>(length));
    }

    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool selected = (item->itemState & ODS_SELECTED) != 0;
    const LONG_PTR style = GetWindowLongPtrW(item->hwndItem, GWL_STYLE);

    if (is_combo) {
        COLORREF background = (item->itemState & ODS_COMBOBOXEDIT) ? color::kInput
                              : (item->itemState & ODS_SELECTED) ? color::kCardActive
                                                                 : color::kInput;
        fill_rect(item->hDC, item->rcItem, background);
        RECT text_area = item->rcItem;
        text_area.left += scale(8);
        draw_text(item->hDC, text_area, text, disabled ? color::kTextDim : color::kText,
                  font_ui(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    (void)style;
    const bool is_radio = radio_group_.count(item->hwndItem) > 0;
    const bool is_toggle = toggles_.count(item->hwndItem) > 0;

    if (is_toggle) {
        fill_rect(item->hDC, item->rcItem, color::kWindow);

        // Inset by the ring width: drawn hard against the left edge, the focus
        // ring would be clipped by the control and wrap only three sides.
        const int box = scale(16);
        const int inset = scale(3);
        RECT mark = {item->rcItem.left + inset,
                     item->rcItem.top + (item->rcItem.bottom - item->rcItem.top - box) / 2,
                     item->rcItem.left + inset + box, 0};
        mark.bottom = mark.top + box;

        const bool on = toggles_.at(item->hwndItem);
        COLORREF fill = on ? color::kAccent : color::kInput;
        COLORREF border = on ? color::kAccent : RGB(0x45, 0x50, 0x5e);
        if (is_radio) {
            fill_round_rect(item->hDC, mark, box / 2, fill, border);
        } else {
            fill_round_rect(item->hDC, mark, 4, fill, border);
        }
        if (on && is_radio) {
            RECT dot = mark;
            InflateRect(&dot, -scale(5), -scale(5));
            fill_round_rect(item->hDC, dot, (dot.bottom - dot.top) / 2, RGB(0x08, 0x12, 0x1c),
                            RGB(0x08, 0x12, 0x1c));
        } else if (on) {
            draw_text(item->hDC, mark, L"✓", RGB(0x08, 0x12, 0x1c), font_ui_bold(),
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        RECT text_area = item->rcItem;
        text_area.left += inset + box + scale(8);
        draw_text(item->hDC, text_area, text, disabled ? color::kTextDim : color::kText,
                  font_ui(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_WORDBREAK);

        // A dotted focus rectangle around the whole control reads as a stray
        // border here; outlining the mark alone is enough of a cue.
        if (item->itemState & ODS_FOCUS) {
            RECT ring = mark;
            InflateRect(&ring, inset, inset);
            // Keep it inside the control so every side is drawn.
            if (ring.top < item->rcItem.top) ring.top = item->rcItem.top;
            if (ring.bottom > item->rcItem.bottom) ring.bottom = item->rcItem.bottom;
            HPEN pen = CreatePen(PS_SOLID, 1, color::kAccent);
            HGDIOBJ old_pen = SelectObject(item->hDC, pen);
            HGDIOBJ old_brush = SelectObject(item->hDC, GetStockObject(NULL_BRUSH));
            RoundRect(item->hDC, ring.left, ring.top, ring.right, ring.bottom,
                      is_radio ? ring.right - ring.left : 8,
                      is_radio ? ring.bottom - ring.top : 8);
            SelectObject(item->hDC, old_brush);
            SelectObject(item->hDC, old_pen);
            DeleteObject(pen);
        }
        return;
    }

    // Push button
    const bool primary = std::find(primary_buttons_.begin(), primary_buttons_.end(),
                                   item->hwndItem) != primary_buttons_.end();
    COLORREF face = primary ? color::kAccent : color::kButton;
    COLORREF fore = primary ? RGB(0x08, 0x12, 0x1c) : color::kText;
    if (disabled) {
        face = RGB(0x1c, 0x21, 0x29);
        fore = color::kTextDim;
    } else if (selected) {
        face = primary ? color::kAccentDark : color::kAccent;
        fore = RGB(0x08, 0x12, 0x1c);
    }
    // Fill first: RoundRect leaves the corners outside the rounded shape
    // untouched, and whatever the button had underneath shows through as a
    // white notch.
    fill_rect(item->hDC, item->rcItem, color::kWindow);
    fill_round_rect(item->hDC, item->rcItem, scale(7), face,
                    primary ? face : RGB(0x33, 0x3c, 0x48));
    draw_text(item->hDC, item->rcItem, text, fore, font_ui(),
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

HWND ModalDialog::add(const wchar_t* class_name, const std::wstring& text, DWORD style,
                      DWORD ex_style, int x, int y, int w, int h, int id) {
    HINSTANCE instance =
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window_, GWLP_HINSTANCE));
    // Every control in every dialog is placed through here, so this is the one
    // spot that has to turn the 96 DPI coordinates of the layout into pixels.
    HWND control = CreateWindowExW(
        ex_style, class_name, text.c_str(), WS_CHILD | WS_VISIBLE | style, scale(x), scale(y),
        scale(w), scale(h), window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        instance, nullptr);
    if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_ui()), TRUE);
    return control;
}

HWND ModalDialog::label(const std::wstring& text, int x, int y, int w, int h, int id) {
    return add(L"Static", text, SS_LEFT, 0, x, y, w, h, id);
}

HWND ModalDialog::edit(const std::wstring& text, int x, int y, int w, int h, int id,
                       bool password, bool multiline) {
    DWORD style = WS_TABSTOP | ES_AUTOHSCROLL;
    if (password) style |= ES_PASSWORD;
    if (multiline) style |= ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL;
    HWND control = add(L"Edit", text, style, 0, x, y, w, h, id);
    if (control) {
        SendMessageW(control, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(scale(7), scale(7)));
        enable_dark_control(control);
        fields_.push_back(control);
    }
    return control;
}

HWND ModalDialog::number(int value, int x, int y, int w, int h, int id) {
    // Zero stands for "no limit", and an empty field says that better than "0".
    std::wstring text = value != 0 ? std::to_wstring(value) : std::wstring();
    HWND control = add(L"Edit", text, WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 0, x, y, w, h, id);
    if (control) {
        SendMessageW(control, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(7, 7));
        enable_dark_control(control);
        fields_.push_back(control);
    }
    return control;
}

HWND ModalDialog::button(const std::wstring& text, int x, int y, int w, int h, int id,
                         bool primary) {
    HWND control = add(L"Button", text, WS_TABSTOP | BS_OWNERDRAW, 0, x, y, w, h, id);
    if (primary && control) primary_buttons_.push_back(control);
    return control;
}

HWND ModalDialog::check(const std::wstring& text, int x, int y, int w, int h, int id,
                        bool value) {
    HWND control = add(L"Button", text, WS_TABSTOP | BS_OWNERDRAW, 0, x, y, w, h, id);
    if (control) toggles_[control] = value;
    return control;
}

HWND ModalDialog::radio(const std::wstring& text, int x, int y, int w, int h, int id,
                        bool value, bool group_start) {
    DWORD style = WS_TABSTOP | BS_OWNERDRAW;
    if (group_start) {
        style |= WS_GROUP;
        ++current_radio_group_;
    }
    HWND control = add(L"Button", text, style, 0, x, y, w, h, id);
    if (control) {
        toggles_[control] = value;
        radio_group_[control] = current_radio_group_;
    }
    return control;
}

HWND ModalDialog::combo(int x, int y, int w, int h, int id,
                        const std::vector<std::wstring>& items, int selected) {
    HWND control = add(L"ComboBox", L"",
                       WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS |
                           WS_VSCROLL,
                       0, x, y, w, h * 8, id);
    if (!control) return nullptr;
    SendMessageW(control, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), scale(h - 8));
    for (const std::wstring& item : items) {
        SendMessageW(control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
    }
    SendMessageW(control, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
    make_dark_combo(control);
    return control;
}

HWND ModalDialog::slider(int x, int y, int w, int h, int id, int low, int high, int value) {
    ensure_slider_class(
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window_, GWLP_HINSTANCE)));
    HWND control = add(kSliderClass, L"", WS_TABSTOP, 0, x, y, w, h, id);
    if (!control) return nullptr;
    if (SliderState* state = slider_state(control)) {
        state->low = low;
        state->high = high;
        state->value = value < low ? low : (value > high ? high : value);
    }
    return control;
}

HWND ModalDialog::group(const std::wstring& text, int x, int y, int w, int h) {
    HWND control = add(L"Static", text, SS_LEFT, 0, x, y, w, h, -1);
    mark_hint(control);
    return control;
}

std::wstring ModalDialog::text_of(HWND control) const {
    if (!control) return {};
    int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

void ModalDialog::set_text(HWND control, const std::wstring& text) {
    if (control) SetWindowTextW(control, text.c_str());
}

bool ModalDialog::checked(HWND control) const {
    auto it = toggles_.find(control);
    return it != toggles_.end() && it->second;
}

void ModalDialog::set_checked(HWND control, bool value) {
    auto it = toggles_.find(control);
    if (it == toggles_.end()) return;
    it->second = value;
    InvalidateRect(control, nullptr, TRUE);
}

// A click flips a checkbox; for a radio button it also clears its siblings.
void ModalDialog::toggle_clicked(HWND control) {
    auto it = toggles_.find(control);
    if (it == toggles_.end()) return;

    auto group = radio_group_.find(control);
    if (group == radio_group_.end()) {
        it->second = !it->second;
        InvalidateRect(control, nullptr, TRUE);
        return;
    }
    for (auto& entry : toggles_) {
        auto other = radio_group_.find(entry.first);
        if (other == radio_group_.end() || other->second != group->second) continue;
        const bool wanted = entry.first == control;
        if (entry.second != wanted) {
            entry.second = wanted;
            InvalidateRect(entry.first, nullptr, TRUE);
        }
    }
}

int ModalDialog::slider_value(HWND control) const {
    SliderState* state = control ? slider_state(control) : nullptr;
    return state ? state->value : 0;
}

void ModalDialog::enable(HWND control, bool value) {
    if (control) EnableWindow(control, value);
}

void ModalDialog::show(HWND control, bool value) {
    if (control) ShowWindow(control, value ? SW_SHOW : SW_HIDE);
}

void ModalDialog::mark_hint(HWND control) {
    if (control) hints_.push_back(control);
}

void ModalDialog::mark_error(HWND control) {
    if (!control) return;
    errors_.push_back(control);
    InvalidateRect(control, nullptr, TRUE);
}

void show_info(HWND parent, const std::wstring& title, const std::wstring& text) {
    MessageBoxW(parent, text.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
}

void show_warning(HWND parent, const std::wstring& title, const std::wstring& text) {
    MessageBoxW(parent, text.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
}

bool ask_yes_no(HWND parent, const std::wstring& title, const std::wstring& text) {
    return MessageBoxW(parent, text.c_str(), title.c_str(),
                       MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
}

}  // namespace ui
