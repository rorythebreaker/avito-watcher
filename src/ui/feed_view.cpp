#include "ui/feed_view.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>

#include "ui/theme.h"
#include "util/strings.h"

namespace ui {
namespace {

constexpr const wchar_t* kClassName = L"AvitoWatcherFeed";
// Written for 96 DPI and scaled at use.
constexpr int kCardMargin = 6;
constexpr int kPad = 12;
constexpr int kMenuOpen = 1;
constexpr int kMenuCopy = 2;
constexpr int kMenuCopyAll = 3;

std::wstring ago_text(double first_seen) {
    double delta = core::now_seconds() - first_seen;
    if (delta < 0) delta = 0;
    long long seconds = static_cast<long long>(delta);
    if (seconds < 60) return L"только что";
    if (seconds < 3600) return std::to_wstring(seconds / 60) + L" мин назад";
    if (seconds < 86400) return std::to_wstring(seconds / 3600) + L" ч назад";

    FILETIME file_time;
    ULARGE_INTEGER value;
    value.QuadPart = static_cast<unsigned long long>((first_seen + 11644473600.0) * 10000000.0);
    file_time.dwLowDateTime = value.LowPart;
    file_time.dwHighDateTime = value.HighPart;
    SYSTEMTIME utc = {}, local = {};
    FileTimeToSystemTime(&file_time, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);

    wchar_t buffer[32];
    swprintf_s(buffer, L"%02d.%02d %02d:%02d", local.wDay, local.wMonth, local.wHour,
               local.wMinute);
    return buffer;
}

void set_clipboard(HWND window, const std::wstring& text) {
    if (!OpenClipboard(window)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* target = GlobalLock(memory);
        if (target) {
            std::memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            SetClipboardData(CF_UNICODETEXT, memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

}  // namespace

void FeedView::register_class(HINSTANCE instance) {
    WNDCLASSEXW description = {};
    description.cbSize = sizeof(description);
    description.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    description.lpfnWndProc = &FeedView::proc;
    description.hInstance = instance;
    description.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    description.hbrBackground = nullptr;
    description.lpszClassName = kClassName;
    RegisterClassExW(&description);
}

HWND FeedView::create(HWND parent, int control_id, ImageLoader* loader, HINSTANCE instance) {
    loader_ = loader;
    window_ = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL,
                              0, 0, 100, 100, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
                              instance, this);
    return window_;
}

LRESULT CALLBACK FeedView::proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    FeedView* self = reinterpret_cast<FeedView*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<FeedView*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wparam, lparam);
    return self->handle_message(message, wparam, lparam);
}

int FeedView::card_height() const {
    return (loader_ ? loader_->thumb_height() : scale(96)) + scale(24);
}

void FeedView::set_limit(size_t limit) {
    limit_ = limit < 50 ? 50 : limit;
    if (items_.size() > limit_) items_.resize(limit_);
    refresh();
}

void FeedView::set_items(std::vector<core::Listing> items) {
    items_ = std::move(items);
    if (items_.size() > limit_) items_.resize(limit_);
    scroll_ = 0;
    selected_ = -1;
    hovered_ = -1;
    update_scrollbar();
    refresh();
}

void FeedView::prepend(const std::vector<core::Listing>& items) {
    if (items.empty()) return;
    items_.insert(items_.begin(), items.begin(), items.end());
    if (items_.size() > limit_) items_.resize(limit_);
    if (selected_ >= 0) selected_ += static_cast<int>(items.size());
    update_scrollbar();
    refresh();
}

void FeedView::clear() {
    items_.clear();
    scroll_ = 0;
    selected_ = -1;
    hovered_ = -1;
    update_scrollbar();
    refresh();
}

void FeedView::refresh() {
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void FeedView::update_scrollbar() {
    if (!window_) return;
    RECT client = {};
    GetClientRect(window_, &client);
    int visible = client.bottom - client.top;
    int total = static_cast<int>(items_.size()) * card_height();

    SCROLLINFO info = {};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = total > 0 ? total - 1 : 0;
    info.nPage = static_cast<UINT>(visible > 0 ? visible : 1);
    info.nPos = scroll_;
    SetScrollInfo(window_, SB_VERT, &info, TRUE);

    int maximum = std::max(0, total - visible);
    if (scroll_ > maximum) scroll_ = maximum;
    if (scroll_ < 0) scroll_ = 0;
}

void FeedView::scroll_to(int offset) {
    RECT client = {};
    GetClientRect(window_, &client);
    int visible = client.bottom - client.top;
    int total = static_cast<int>(items_.size()) * card_height();
    int maximum = std::max(0, total - visible);

    offset = std::clamp(offset, 0, maximum);
    if (offset == scroll_) return;
    scroll_ = offset;

    SCROLLINFO info = {};
    info.cbSize = sizeof(info);
    info.fMask = SIF_POS;
    info.nPos = scroll_;
    SetScrollInfo(window_, SB_VERT, &info, TRUE);
    refresh();
}

int FeedView::index_at(int y) const {
    int index = (y + scroll_) / card_height();
    if (index < 0 || index >= static_cast<int>(items_.size())) return -1;
    return index;
}

void FeedView::open_selected() {
    if (selected_ < 0 || selected_ >= static_cast<int>(items_.size())) return;
    const std::string& url = items_[selected_].url;
    if (url.empty()) return;
    ShellExecuteW(nullptr, L"open", util::widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void FeedView::copy_link(bool all) {
    if (all) {
        std::string text;
        for (const core::Listing& listing : items_) {
            text += listing.url;
            text += "\r\n";
        }
        set_clipboard(window_, util::widen(text));
        return;
    }
    if (selected_ < 0 || selected_ >= static_cast<int>(items_.size())) return;
    set_clipboard(window_, util::widen(items_[selected_].url));
}

void FeedView::show_menu(int x, int y) {
    if (selected_ < 0) return;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuOpen, L"Открыть на Авито");
    AppendMenuW(menu, MF_STRING, kMenuCopy, L"Копировать ссылку");
    AppendMenuW(menu, MF_STRING, kMenuCopyAll, L"Копировать все ссылки в ленте");

    POINT point = {x, y};
    ClientToScreen(window_, &point);
    int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0,
                                 window_, nullptr);
    DestroyMenu(menu);

    switch (command) {
        case kMenuOpen: open_selected(); break;
        case kMenuCopy: copy_link(false); break;
        case kMenuCopyAll: copy_link(true); break;
        default: break;
    }
}

void FeedView::paint() {
    PAINTSTRUCT paint = {};
    HDC screen_dc = BeginPaint(window_, &paint);
    RECT client = {};
    GetClientRect(window_, &client);
    render(screen_dc, client);
    EndPaint(window_, &paint);
}

// Painting goes through a target DC so the same code serves WM_PAINT and
// WM_PRINTCLIENT; without the latter the control stays blank in screenshots
// and in anything else that renders the window off-screen.
void FeedView::render(HDC screen_dc, const RECT& client) {
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;

    // Draw into a bitmap first: the cards overlap a lot of small pieces and
    // painting them straight to the window flickers badly while scrolling.
    HDC dc = CreateCompatibleDC(screen_dc);
    HBITMAP buffer = CreateCompatibleBitmap(screen_dc, width, height);
    HGDIOBJ old_bitmap = SelectObject(dc, buffer);

    fill_rect(dc, client, color::kPanel);

    if (items_.empty()) {
        RECT text_area = client;
        text_area.top += scale(40);
        draw_text(dc, text_area,
                  L"Пока пусто. Найденные объявления появятся здесь.",
                  color::kTextDim, font_ui(), DT_CENTER | DT_TOP | DT_WORDBREAK);
    }

    const int card_h = card_height();
    int first = std::max(0, scroll_ / card_h);
    int last = std::min(static_cast<int>(items_.size()),
                        (scroll_ + height) / card_h + 1);

    for (int index = first; index < last; ++index) {
        const core::Listing& listing = items_[index];

        RECT card = {};
        card.left = client.left + scale(kCardMargin);
        card.right = client.right - scale(kCardMargin);
        card.top = index * card_h - scroll_ + scale(4);
        card.bottom = card.top + card_h - scale(8);

        COLORREF background = color::kCard;
        if (index == selected_) background = color::kCardActive;
        else if (index == hovered_) background = color::kCardHover;
        fill_round_rect(dc, card, scale(10), background,
                        index == selected_ ? color::kAccent : color::kBorder);

        // Photo. The thumbnail is decoded at device resolution, so it is blitted
        // one to one rather than stretched.
        const int thumb_w = loader_ ? loader_->thumb_width() : scale(128);
        const int thumb_h = loader_ ? loader_->thumb_height() : scale(96);
        RECT photo = {card.left + scale(10), card.top + scale(10),
                      card.left + scale(10) + thumb_w, card.top + scale(10) + thumb_h};
        const int photo_radius = scale(7);
        HBITMAP thumbnail = loader_ ? loader_->get(listing.image_url) : nullptr;
        if (thumbnail) {
            // Clip to the same rounded shape the placeholder and the card use,
            // so the picture does not sit as a hard-edged square inside them.
            HRGN clip = CreateRoundRectRgn(photo.left, photo.top, photo.right + 1,
                                           photo.bottom + 1, photo_radius * 2,
                                           photo_radius * 2);
            SelectClipRgn(dc, clip);

            HDC source = CreateCompatibleDC(dc);
            HGDIOBJ old_source = SelectObject(source, thumbnail);
            BitBlt(dc, photo.left, photo.top, thumb_w, thumb_h, source, 0, 0, SRCCOPY);
            SelectObject(source, old_source);
            DeleteDC(source);

            SelectClipRgn(dc, nullptr);
            DeleteObject(clip);
        } else {
            fill_round_rect(dc, photo, photo_radius, RGB(0x2a, 0x2f, 0x36),
                            RGB(0x2a, 0x2f, 0x36));
            draw_text(dc, photo, listing.image_url.empty() ? L"нет фото" : L"…",
                      color::kTextDim, font_small(), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        const int left = photo.right + scale(kPad);
        int badge_right = card.right - scale(kPad);

        // Badges: the task name and, for "similar" tasks, the match percentage.
        if (listing.score < 100) {
            std::wstring text = std::to_wstring(listing.score) + L"%";
            int badge_width = text_width(dc, text, font_small()) + scale(14);
            RECT badge = {badge_right - badge_width, card.top + scale(10), badge_right,
                          card.top + scale(28)};
            draw_pill(dc, badge, text, color::kAccent, RGB(0x0b, 0x16, 0x20));
            badge_right = badge.left - scale(6);
        }
        if (!listing.task_name.empty()) {
            std::wstring text =
                elide(dc, util::widen(listing.task_name), scale(150), font_small());
            int badge_width = text_width(dc, text, font_small()) + scale(14);
            RECT badge = {badge_right - badge_width, card.top + scale(10), badge_right,
                          card.top + scale(28)};
            draw_pill(dc, badge, text, RGB(0x32, 0x3a, 0x45), color::kTextMuted);
            badge_right = badge.left - scale(6);
        }

        const int title_width = std::max(scale(60), badge_right - left - scale(8));

        RECT title_area = {left, card.top + scale(9), left + title_width,
                           card.top + scale(31)};
        draw_text(dc, title_area,
                  elide(dc, util::widen(listing.title.empty() ? "Без названия" : listing.title),
                        title_width, font_ui_bold()),
                  color::kText, font_ui_bold(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT price_area = {left, card.top + scale(34), card.right - scale(kPad),
                           card.top + scale(60)};
        draw_text(dc, price_area, util::widen(listing.price_label()), color::kPrice,
                  font_title(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        std::string meta;
        for (const std::string& part : {listing.location, listing.date_text, listing.seller}) {
            if (part.empty()) continue;
            if (!meta.empty()) meta += " · ";
            meta += part;
        }
        const int meta_width = card.right - scale(kPad) - left;
        RECT meta_area = {left, card.top + scale(62), card.right - scale(kPad),
                          card.top + scale(80)};
        draw_text(dc, meta_area, elide(dc, util::widen(meta), meta_width, font_small()),
                  color::kTextMuted, font_small(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT found_area = {left, card.top + scale(80), card.right - scale(kPad),
                           card.top + scale(98)};
        draw_text(dc, found_area, L"найдено " + ago_text(listing.first_seen),
                  color::kTextDim, font_small(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    BitBlt(screen_dc, 0, 0, width, height, dc, 0, 0, SRCCOPY);

    SelectObject(dc, old_bitmap);
    DeleteObject(buffer);
    DeleteDC(dc);
}

LRESULT FeedView::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_PAINT:
            paint();
            return 0;

        case WM_PRINTCLIENT: {
            RECT client = {};
            GetClientRect(window_, &client);
            render(reinterpret_cast<HDC>(wparam), client);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;  // the paint handler fills everything

        case WM_SIZE:
            update_scrollbar();
            return 0;

        case WM_VSCROLL: {
            SCROLLINFO info = {};
            info.cbSize = sizeof(info);
            info.fMask = SIF_ALL;
            GetScrollInfo(window_, SB_VERT, &info);
            int position = info.nPos;
            switch (LOWORD(wparam)) {
                case SB_LINEUP: position -= scale(40); break;
                case SB_LINEDOWN: position += scale(40); break;
                case SB_PAGEUP: position -= info.nPage; break;
                case SB_PAGEDOWN: position += info.nPage; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: position = info.nTrackPos; break;
                default: break;
            }
            scroll_to(position);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            scroll_to(scroll_ - delta / WHEEL_DELTA * scale(60));
            return 0;
        }

        case WM_MOUSEMOVE: {
            int index = index_at(GET_Y_LPARAM(lparam));
            if (index != hovered_) {
                hovered_ = index;
                TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window_, 0};
                TrackMouseEvent(&track);
                refresh();
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            hovered_ = -1;
            refresh();
            return 0;

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN: {
            SetFocus(window_);
            int index = index_at(GET_Y_LPARAM(lparam));
            if (index != selected_) {
                selected_ = index;
                refresh();
            }
            if (message == WM_RBUTTONDOWN && index >= 0) {
                show_menu(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            }
            return 0;
        }

        case WM_LBUTTONDBLCLK:
            selected_ = index_at(GET_Y_LPARAM(lparam));
            open_selected();
            return 0;

        case WM_KEYDOWN:
            if (wparam == VK_RETURN) open_selected();
            if (wparam == VK_UP && selected_ > 0) { --selected_; refresh(); }
            if (wparam == VK_DOWN && selected_ + 1 < static_cast<int>(items_.size())) {
                ++selected_;
                refresh();
            }
            return 0;

        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;

        default:
            break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

}  // namespace ui
