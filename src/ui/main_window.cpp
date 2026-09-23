#include "ui/main_window.h"

#include <commctrl.h>
#include <commoncontrols.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>

#include "core/autostart.h"
#include "core/config.h"
#include "core/store.h"
#include "ui/dialog.h"
#include "ui/icon.h"
#include "ui/settings_dialog.h"
#include "ui/task_dialog.h"
#include "ui/theme.h"
#include "util/log.h"
#include "util/paths.h"
#include "util/strings.h"

namespace ui {
namespace {

constexpr const wchar_t* kClassName = L"AvitoWatcherMain";
constexpr const wchar_t* kWindowTitle = L"Avito Watcher";

constexpr UINT WM_ENGINE_MESSAGE = WM_APP + 20;
constexpr UINT WM_TASK_STARTED   = WM_APP + 21;
constexpr UINT WM_TASK_FINISHED  = WM_APP + 22;
constexpr UINT WM_NEW_LISTINGS   = WM_APP + 23;
constexpr UINT WM_NOTIFY_ERRORS  = WM_APP + 24;
constexpr UINT WM_RUNNING        = WM_APP + 25;
constexpr UINT WM_IMAGE_READY    = WM_APP + 26;
constexpr UINT WM_TRAY           = WM_APP + 27;

constexpr int kIdTaskList = 2000;
constexpr int kIdFilter = 2001;
constexpr int kIdClear = 2002;
constexpr int kIdJournal = 2003;
constexpr int kIdFeed = 2004;

constexpr int kBtnWatch = 1;
constexpr int kBtnAdd = 2;
constexpr int kBtnEdit = 3;
constexpr int kBtnCheck = 4;
constexpr int kBtnDelete = 5;
constexpr int kBtnOpen = 6;
constexpr int kBtnSettings = 7;

constexpr int kTrayId = 1;
constexpr int kMenuShow = 40001;
constexpr int kMenuWatch = 40002;
constexpr int kMenuQuit = 40003;

constexpr int kToolbarHeight = 48;
constexpr int kStatusHeight = 26;
constexpr int kLeftWidth = 440;
constexpr int kJournalHeight = 150;
constexpr int kTimerRefresh = 1;

struct TaskFinishedPayload {
    int task_id = 0;
    core::TaskStatus status = core::TaskStatus::Ok;
    std::string error;
};

struct NewListingsPayload {
    int task_id = 0;
    std::vector<core::Listing> items;
};

COLORREF status_color(core::TaskStatus status) {
    switch (status) {
        case core::TaskStatus::Ok: return color::kPrice;
        case core::TaskStatus::Running: return color::kAccent;
        case core::TaskStatus::Blocked: return color::kWarning;
        case core::TaskStatus::Error: return color::kError;
        default: return color::kTextMuted;
    }
}

std::wstring interval_label(int seconds) {
    if (seconds < 3600) return std::to_wstring(seconds / 60) + L" мин";
    int hours = seconds / 3600;
    if (seconds % 3600 == 0) return std::to_wstring(hours) + L" ч";
    wchar_t buffer[16];
    swprintf_s(buffer, L"%.1f ч", seconds / 3600.0);
    return buffer;
}

// The header sends its custom-draw notifications to its own parent, the list
// view, so they never reach the main window. Subclassing the list lets us catch
// them and paint the header dark like everything else.
LRESULT CALLBACK task_list_subclass(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                    UINT_PTR id, DWORD_PTR data) {
    (void)id;
    (void)data;
    if (message == WM_NOTIFY) {
        auto* header = reinterpret_cast<NMHDR*>(lparam);
        if (header && header->code == NM_CUSTOMDRAW &&
            header->hwndFrom == ListView_GetHeader(window)) {
            auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(lparam);
            if (draw->dwDrawStage == CDDS_PREPAINT) {
                // Fill the whole strip, including the empty part past the last
                // column, which no per-item draw would ever cover.
                fill_rect(draw->hdc, draw->rc, color::kBar);
                return CDRF_NOTIFYITEMDRAW;
            }
            if (draw->dwDrawStage == CDDS_ITEMPREPAINT) {
                fill_rect(draw->hdc, draw->rc, color::kBar);
                RECT line = draw->rc;
                line.top = line.bottom - 1;
                fill_rect(draw->hdc, line, color::kBorder);

                wchar_t buffer[128] = {};
                HDITEMW item = {};
                item.mask = HDI_TEXT;
                item.pszText = buffer;
                item.cchTextMax = 128;
                Header_GetItem(header->hwndFrom, static_cast<int>(draw->dwItemSpec), &item);

                RECT text_area = draw->rc;
                text_area.left += 7;
                draw_text(draw->hdc, text_area, buffer, color::kTextMuted, font_small(),
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                return CDRF_SKIPDEFAULT;
            }
            return CDRF_DODEFAULT;
        }
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, task_list_subclass, 1);
    return DefSubclassProc(window, message, wparam, lparam);
}

}  // namespace

MainWindow::MainWindow() = default;

MainWindow::~MainWindow() {
    shutdown();
}

bool MainWindow::create(HINSTANCE instance, bool start_hidden) {
    instance_ = instance;

    WNDCLASSEXW description = {};
    description.cbSize = sizeof(description);
    description.style = CS_HREDRAW | CS_VREDRAW;
    description.lpfnWndProc = &MainWindow::proc;
    description.hInstance = instance;
    description.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    description.hbrBackground = brush(color::kWindow);
    description.lpszClassName = kClassName;
    description.hIcon = app_icon();
    description.hIconSm = app_icon_small();
    RegisterClassExW(&description);
    FeedView::register_class(instance);

    core::Settings settings = core::settings().get();

    window_ = CreateWindowExW(0, kClassName, kWindowTitle, WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1180, 780, nullptr, nullptr,
                              instance, this);
    if (!window_) return false;

    // Restore where the window was last time.
    if (!settings.window_placement.empty()) {
        std::vector<unsigned char> raw = util::base64_decode(settings.window_placement);
        if (raw.size() == sizeof(WINDOWPLACEMENT)) {
            WINDOWPLACEMENT placement = {};
            std::memcpy(&placement, raw.data(), sizeof(placement));
            placement.length = sizeof(placement);
            placement.showCmd = SW_HIDE;
            SetWindowPlacement(window_, &placement);
        }
    }

    enable_dark_titlebar(window_);
    build_children(instance);
    layout();

    engine_ = std::make_unique<avito::Engine>(this);
    images_->set_proxy(settings.proxy);

    reload_tasks();
    reload_feed();
    add_tray_icon();
    SetTimer(window_, kTimerRefresh, 60000, nullptr);

    if (!start_hidden && !settings.start_minimized) {
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
    }
    return true;
}

void MainWindow::build_children(HINSTANCE instance) {
    buttons_ = {
        {kBtnWatch, L"▶  Начать слежение", {}, true, true},
        {kBtnAdd, L"Новая задача", {}, true, false},
        {kBtnEdit, L"Изменить", {}, false, false},
        {kBtnCheck, L"Проверить сейчас", {}, false, false},
        {kBtnDelete, L"Удалить", {}, false, false},
        {kBtnOpen, L"Открыть на Авито", {}, false, false},
        {kBtnSettings, L"Настройки", {}, true, false},
    };

    task_list_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL |
                                     LVS_SHOWSELALWAYS | WS_TABSTOP,
                                 0, 0, 100, 100, window_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdTaskList)),
                                 instance, nullptr);
    ListView_SetExtendedListViewStyle(task_list_, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES |
                                                      LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(task_list_, color::kInput);
    ListView_SetTextBkColor(task_list_, color::kInput);
    ListView_SetTextColor(task_list_, color::kText);
    SendMessageW(task_list_, WM_SETFONT, reinterpret_cast<WPARAM>(font_ui()), TRUE);
    SetWindowSubclass(task_list_, task_list_subclass, 1, 0);
    // The dark list themes draw column separators down the whole empty
    // area, which looks like a broken table when there are few tasks, so
    // the list keeps the plain theme and gets its colours from the
    // ListView_Set*Color calls above and the custom-drawn header.

    // Widths add up to the list's inner width so no horizontal scrollbar
    // appears: kLeftWidth minus margins minus the vertical scrollbar.
    const struct { const wchar_t* title; int width; } columns[] = {
        {L"Задача", 150}, {L"Тип", 84}, {L"Интервал", 62},
        {L"Статус", 80}, {L"Найдено", 48},
    };
    for (int i = 0; i < 5; ++i) {
        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(columns[i].title);
        column.cx = columns[i].width;
        column.iSubItem = i;
        ListView_InsertColumn(task_list_, i, &column);
    }

    filter_combo_ = CreateWindowExW(0, L"ComboBox", L"",
                                    WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL |
                                        WS_TABSTOP,
                                    0, 0, 220, 240, window_,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdFilter)),
                                    instance, nullptr);
    SendMessageW(filter_combo_, WM_SETFONT, reinterpret_cast<WPARAM>(font_ui()), TRUE);
    make_dark_combo(filter_combo_);

    clear_button_ = CreateWindowExW(0, L"Button", L"Очистить ленту",
                                    WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                                    0, 0, 130, 26, window_,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdClear)),
                                    instance, nullptr);
    SendMessageW(clear_button_, WM_SETFONT, reinterpret_cast<WPARAM>(font_ui()), TRUE);

    journal_ = CreateWindowExW(0, L"Edit", L"",
                               WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                                   ES_READONLY | ES_AUTOVSCROLL,
                               0, 0, 100, 100, window_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdJournal)),
                               instance, nullptr);
    SendMessageW(journal_, WM_SETFONT, reinterpret_cast<WPARAM>(font(8)), TRUE);
    SendMessageW(journal_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(7, 7));
    enable_dark_control(journal_);

    images_ = std::make_unique<ImageLoader>(window_, WM_IMAGE_READY);
    feed_ = std::make_unique<FeedView>();
    feed_->create(window_, kIdFeed, images_.get(), instance);
}

void MainWindow::layout() {
    RECT client = {};
    GetClientRect(window_, &client);
    const int width = client.right;
    const int height = client.bottom;

    const int content_top = kToolbarHeight + 8;
    const int content_bottom = height - kStatusHeight - 8;

    // Left column: the task list with a hint underneath.
    const int list_top = content_top + 22;
    const int hint_height = 54;
    MoveWindow(task_list_, 8, list_top, kLeftWidth - 16,
               std::max(60, content_bottom - list_top - hint_height), TRUE);

    // Right column.
    const int right_left = kLeftWidth;
    const int right_width = std::max(200, width - right_left - 8);
    const int header_top = content_top;

    MoveWindow(clear_button_, right_left + right_width - 130, header_top - 2, 130, 26, TRUE);
    MoveWindow(filter_combo_, right_left + right_width - 130 - 230, header_top - 2, 220, 240,
               TRUE);

    const int feed_top = header_top + 30;
    const int journal_top = content_bottom - kJournalHeight;
    MoveWindow(feed_->handle(), right_left, feed_top, right_width,
               std::max(80, journal_top - feed_top - 24), TRUE);
    MoveWindow(journal_, right_left, journal_top, right_width, kJournalHeight, TRUE);

    // Toolbar buttons are painted by the parent; compute their boxes here.
    HDC dc = GetDC(window_);
    int x = 10;
    for (ToolButton& tool : buttons_) {
        int text_size = text_width(dc, tool.text, font_ui());
        int button_width = text_size + (tool.wide ? 34 : 26);
        if (tool.id == kBtnSettings) x = width - button_width - 10;
        tool.rect = {x, 8, x + button_width, 8 + 32};
        x += button_width + 6;
    }
    ReleaseDC(window_, dc);

    InvalidateRect(window_, nullptr, TRUE);
}

LRESULT CALLBACK MainWindow::proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wparam, lparam);
    return self->handle(message, wparam, lparam);
}

LRESULT MainWindow::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED) layout();
            return 0;

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = 900;
            info->ptMinTrackSize.y = 560;
            return 0;
        }

        case WM_PAINT:
            paint();
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_TIMER:
            if (wparam == kTimerRefresh) {
                feed_->refresh();
                update_task_hint();
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEMOVE: {
            int index = button_at(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (index != hovered_button_) {
                hovered_button_ = index;
                TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window_, 0};
                TrackMouseEvent(&track);
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            hovered_button_ = -1;
            InvalidateRect(window_, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN:
            // A modal dialog disables this window; ignore whatever still reaches
            // us so a second copy of the dialog cannot be opened.
            if (!IsWindowEnabled(window_)) return 0;
            pressed_button_ = button_at(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            InvalidateRect(window_, nullptr, FALSE);
            return 0;

        case WM_LBUTTONUP: {
            if (!IsWindowEnabled(window_)) return 0;
            int index = button_at(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            int pressed = pressed_button_;
            pressed_button_ = -1;
            InvalidateRect(window_, nullptr, FALSE);
            if (index >= 0 && index == pressed) handle_toolbar_click(index);
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wparam);
            int notification = HIWORD(wparam);
            if (id == kIdFilter && notification == CBN_SELCHANGE) {
                reload_feed();
            } else if (id == kIdClear && notification == BN_CLICKED) {
                clear_feed();
            } else if (id == kMenuShow) {
                restore();
            } else if (id == kMenuWatch) {
                toggle_watching();
            } else if (id == kMenuQuit) {
                quitting_ = true;
                DestroyWindow(window_);
            }
            return 0;
        }

        case WM_DRAWITEM: {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (item && item->hwndItem == clear_button_) {
                bool down = (item->itemState & ODS_SELECTED) != 0;
                fill_rect(item->hDC, item->rcItem, color::kWindow);
                fill_round_rect(item->hDC, item->rcItem, 7,
                                down ? color::kAccent : color::kButton,
                                RGB(0x33, 0x3c, 0x48));
                draw_text(item->hDC, item->rcItem, L"Очистить ленту",
                          down ? RGB(0x08, 0x12, 0x1c) : color::kText, font_ui(),
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            return 0;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, message == WM_CTLCOLORSTATIC ? color::kTextMuted : color::kText);
            SetBkColor(dc, color::kInput);
            return reinterpret_cast<LRESULT>(brush(color::kInput));
        }

        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lparam);
            if (!header) return 0;

            if (header->hwndFrom == task_list_ && header->code == NM_CUSTOMDRAW) {
                auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lparam);
                switch (draw->nmcd.dwDrawStage) {
                    case CDDS_PREPAINT:
                        return CDRF_NOTIFYITEMDRAW;
                    case CDDS_ITEMPREPAINT:
                        draw->clrTextBk = (draw->nmcd.uItemState & CDIS_SELECTED)
                                              ? color::kCardActive
                                              : color::kInput;
                        draw->clrText = color::kText;
                        return CDRF_NOTIFYSUBITEMDRAW;
                    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
                        if (draw->iSubItem == 3) {
                            core::Task task;
                            int id = static_cast<int>(draw->nmcd.lItemlParam);
                            if (core::store().task(id, task)) {
                                draw->clrText = status_color(task.status);
                            }
                        } else {
                            draw->clrText = color::kText;
                        }
                        return CDRF_NEWFONT;
                    }
                    default:
                        return CDRF_DODEFAULT;
                }
            }

            if (header->hwndFrom == task_list_ && header->code == LVN_ITEMCHANGED) {
                auto* changed = reinterpret_cast<NMLISTVIEW*>(lparam);
                if (changed->uChanged & LVIF_STATE) {
                    // Checkbox state lives in the item's state image bits.
                    if ((changed->uOldState & LVIS_STATEIMAGEMASK) &&
                        (changed->uNewState & LVIS_STATEIMAGEMASK) &&
                        (changed->uOldState & LVIS_STATEIMAGEMASK) !=
                            (changed->uNewState & LVIS_STATEIMAGEMASK)) {
                        core::Task task;
                        int id = static_cast<int>(changed->lParam);
                        if (core::store().task(id, task)) {
                            bool enabled = ((changed->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
                            if (enabled != task.enabled) {
                                task.enabled = enabled;
                                core::store().update_task(task);
                                log("«" + task.name + "»: " +
                                    (enabled ? "включена" : "выключена"));
                                if (enabled) engine_->schedule_soon(task.id);
                                update_counters();
                            }
                        }
                    }
                    update_buttons();
                    update_task_hint();
                }
                return 0;
            }

            if (header->hwndFrom == task_list_ && header->code == NM_DBLCLK) {
                edit_task();
                return 0;
            }
            return 0;
        }

        case WM_ENGINE_MESSAGE: {
            std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lparam));
            if (text) log(*text);
            return 0;
        }

        case WM_TASK_STARTED:
            set_task_row_status(static_cast<int>(wparam), core::TaskStatus::Running, {});
            return 0;

        case WM_TASK_FINISHED: {
            std::unique_ptr<TaskFinishedPayload> payload(
                reinterpret_cast<TaskFinishedPayload*>(lparam));
            if (payload) {
                set_task_row_status(payload->task_id, payload->status, payload->error);
                update_task_hint();
                update_counters();
            }
            return 0;
        }

        case WM_NEW_LISTINGS: {
            std::unique_ptr<NewListingsPayload> payload(
                reinterpret_cast<NewListingsPayload*>(lparam));
            if (!payload || payload->items.empty()) return 0;

            int filter = filter_combo_ ? static_cast<int>(
                                             SendMessageW(filter_combo_, CB_GETCURSEL, 0, 0))
                                       : 0;
            int filter_id = (filter >= 0 && filter < static_cast<int>(filter_ids_.size()))
                                ? filter_ids_[filter]
                                : 0;
            if (filter_id == 0 || filter_id == payload->task_id) {
                feed_->prepend(payload->items);
            }
            update_counters();

            core::Settings settings = core::settings().get();
            last_listing_ = payload->items.front();
            has_last_listing_ = true;
            if (settings.notify_sound) MessageBeep(MB_OK);

            if (settings.notify_desktop) {
                const core::Listing& first = payload->items.front();
                std::wstring title;
                std::wstring body;
                if (payload->items.size() == 1) {
                    title = L"Новое объявление";
                    body = util::widen(first.title) + L"\n" + util::widen(first.price_label());
                } else {
                    title = L"Новых объявлений: " + std::to_wstring(payload->items.size());
                    body = util::widen(first.title) + L"\n" + util::widen(first.price_label()) +
                           L" и ещё " + std::to_wstring(payload->items.size() - 1);
                }
                show_balloon(title, body);
            }

            if (GetForegroundWindow() != window_) {
                unseen_ += static_cast<int>(payload->items.size());
                update_tray(true);
            }
            return 0;
        }

        case WM_NOTIFY_ERRORS: {
            std::unique_ptr<std::vector<std::string>> errors(
                reinterpret_cast<std::vector<std::string>*>(lparam));
            if (errors) {
                for (const std::string& error : *errors) {
                    log("⚠ Уведомление не отправлено — " + error);
                }
            }
            return 0;
        }

        case WM_RUNNING: {
            running_ = wparam != 0;
            buttons_[0].text = running_ ? L"■  Остановить слежение" : L"▶  Начать слежение";
            status_text_ = running_ ? L"Слежение работает" : L"Слежение остановлено";
            layout();
            return 0;
        }

        case WM_IMAGE_READY:
            feed_->refresh();
            return 0;

        case WM_TRAY:
            handle_tray(lparam);
            return 0;

        case WM_ACTIVATE:
            if (LOWORD(wparam) != WA_INACTIVE) {
                unseen_ = 0;
                update_tray(false);
            }
            return 0;

        case WM_CLOSE:
            if (!quitting_ && core::settings().get().minimize_to_tray) {
                ShowWindow(window_, SW_HIDE);
                show_balloon(kWindowTitle,
                             L"Приложение продолжает следить за объявлениями. "
                             L"Чтобы выйти — правый щелчок по значку в трее.");
                return 0;
            }
            DestroyWindow(window_);
            return 0;

        case WM_DESTROY:
            shutdown();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

void MainWindow::paint() {
    PAINTSTRUCT ps = {};
    HDC screen = BeginPaint(window_, &ps);

    RECT client = {};
    GetClientRect(window_, &client);

    HDC dc = CreateCompatibleDC(screen);
    HBITMAP buffer = CreateCompatibleBitmap(screen, client.right, client.bottom);
    HGDIOBJ old = SelectObject(dc, buffer);

    fill_rect(dc, client, color::kWindow);
    paint_toolbar(dc, client);

    // Section captions and the task hint.
    RECT caption = {8, kToolbarHeight + 8, kLeftWidth - 16, kToolbarHeight + 28};
    draw_text(dc, caption, L"Задачи", color::kTextMuted, font_ui(),
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT feed_caption = {kLeftWidth, kToolbarHeight + 8, kLeftWidth + 300, kToolbarHeight + 28};
    draw_text(dc, feed_caption, L"Найденные объявления", color::kTextMuted, font_ui(),
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT list_rect = {};
    GetWindowRect(task_list_, &list_rect);
    MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&list_rect), 2);
    RECT hint = {8, list_rect.bottom + 6, kLeftWidth - 16, list_rect.bottom + 60};
    draw_text(dc, hint, task_hint_, color::kTextDim, font_small(),
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);

    RECT journal_rect = {};
    GetWindowRect(journal_, &journal_rect);
    MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&journal_rect), 2);
    RECT journal_caption = {kLeftWidth, journal_rect.top - 22, kLeftWidth + 200,
                            journal_rect.top - 4};
    draw_text(dc, journal_caption, L"Журнал", color::kTextMuted, font_ui(),
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Frames around the borderless children.
    for (HWND framed : {journal_, feed_->handle(), task_list_}) {
        if (!framed) continue;
        RECT bounds = {};
        GetWindowRect(framed, &bounds);
        MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&bounds), 2);
        draw_field_frame(dc, bounds, false);
    }

    paint_status(dc, client);

    BitBlt(screen, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(buffer);
    DeleteDC(dc);
    EndPaint(window_, &ps);
}

void MainWindow::paint_toolbar(HDC dc, const RECT& client) {
    RECT bar = {0, 0, client.right, kToolbarHeight};
    fill_rect(dc, bar, color::kBar);
    RECT line = {0, kToolbarHeight - 1, client.right, kToolbarHeight};
    fill_rect(dc, line, color::kBorder);

    for (size_t i = 0; i < buttons_.size(); ++i) {
        const ToolButton& tool = buttons_[i];
        COLORREF face = color::kBar;
        COLORREF text = tool.enabled ? color::kText : color::kTextDim;

        if (tool.enabled && static_cast<int>(i) == pressed_button_) {
            face = color::kAccent;
            text = RGB(0x08, 0x12, 0x1c);
        } else if (tool.enabled && static_cast<int>(i) == hovered_button_) {
            face = color::kButtonHover;
        }
        if (face != color::kBar) fill_round_rect(dc, tool.rect, 7, face, face);
        draw_text(dc, tool.rect, tool.text, text, font_ui(),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

void MainWindow::paint_status(HDC dc, const RECT& client) {
    RECT bar = {0, client.bottom - kStatusHeight, client.right, client.bottom};
    fill_rect(dc, bar, color::kBar);

    RECT left = bar;
    left.left += 12;
    draw_text(dc, left, status_text_, color::kTextMuted, font_small(),
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT right = bar;
    right.right -= 12;
    draw_text(dc, right, counter_text_, color::kTextMuted, font_small(),
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

int MainWindow::button_at(int x, int y) const {
    POINT point = {x, y};
    for (size_t i = 0; i < buttons_.size(); ++i) {
        if (buttons_[i].enabled && PtInRect(&buttons_[i].rect, point)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void MainWindow::handle_toolbar_click(int index) {
    switch (buttons_[static_cast<size_t>(index)].id) {
        case kBtnWatch: toggle_watching(); break;
        case kBtnAdd: add_task(); break;
        case kBtnEdit: edit_task(); break;
        case kBtnCheck: check_now(); break;
        case kBtnDelete: delete_task(); break;
        case kBtnOpen: open_task_url(); break;
        case kBtnSettings: open_settings(); break;
        default: break;
    }
}

// --- tasks ---------------------------------------------------------------

void MainWindow::reload_tasks() {
    int selected = selected_task_id();
    ListView_DeleteAllItems(task_list_);

    std::vector<core::Task> tasks = core::store().tasks();
    for (size_t i = 0; i < tasks.size(); ++i) {
        const core::Task& task = tasks[i];
        std::wstring name = util::widen(task.name);

        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = name.data();
        item.lParam = task.id;
        ListView_InsertItem(task_list_, &item);

        std::wstring kind = util::widen(core::kind_title(task.kind));
        std::wstring interval = interval_label(task.interval);
        std::wstring status = util::widen(core::status_title(task.status));
        std::wstring found = std::to_wstring(task.found_total);
        ListView_SetItemText(task_list_, static_cast<int>(i), 1, kind.data());
        ListView_SetItemText(task_list_, static_cast<int>(i), 2, interval.data());
        ListView_SetItemText(task_list_, static_cast<int>(i), 3, status.data());
        ListView_SetItemText(task_list_, static_cast<int>(i), 4, found.data());
        ListView_SetCheckState(task_list_, static_cast<int>(i), task.enabled);
    }

    if (selected > 0) select_task(selected);
    else if (!tasks.empty()) ListView_SetItemState(task_list_, 0, LVIS_SELECTED | LVIS_FOCUSED,
                                                   LVIS_SELECTED | LVIS_FOCUSED);

    rebuild_filter();
    update_counters();
    update_buttons();
    update_task_hint();
}

void MainWindow::rebuild_filter() {
    int previous = 0;
    int current = static_cast<int>(SendMessageW(filter_combo_, CB_GETCURSEL, 0, 0));
    if (current >= 0 && current < static_cast<int>(filter_ids_.size())) {
        previous = filter_ids_[current];
    }

    SendMessageW(filter_combo_, CB_RESETCONTENT, 0, 0);
    filter_ids_.clear();

    SendMessageW(filter_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Все задачи"));
    filter_ids_.push_back(0);

    int selected_index = 0;
    for (const core::Task& task : core::store().tasks()) {
        std::wstring name = util::widen(task.name);
        SendMessageW(filter_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        filter_ids_.push_back(task.id);
        if (task.id == previous) selected_index = static_cast<int>(filter_ids_.size()) - 1;
    }
    SendMessageW(filter_combo_, CB_SETCURSEL, selected_index, 0);
}

void MainWindow::update_counters() {
    std::vector<core::Task> tasks = core::store().tasks();
    int active = 0;
    long long total = 0;
    for (const core::Task& task : tasks) {
        if (task.enabled) ++active;
        total += task.found_total;
    }
    counter_text_ = L"Задач: " + std::to_wstring(tasks.size()) + L" (активных " +
                    std::to_wstring(active) + L") · найдено объявлений: " +
                    std::to_wstring(total);
    InvalidateRect(window_, nullptr, FALSE);
}

void MainWindow::update_task_hint() {
    core::Task task;
    if (!selected_task(task)) {
        task_hint_ =
            L"Задач пока нет. Нажмите «Новая задача»: можно следить за поисковым "
            L"запросом или за объявлениями, похожими на выбранное.";
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }

    std::wstring text;
    if (task.last_check > 0) {
        FILETIME file_time;
        ULARGE_INTEGER value;
        value.QuadPart =
            static_cast<unsigned long long>((task.last_check + 11644473600.0) * 10000000.0);
        file_time.dwLowDateTime = value.LowPart;
        file_time.dwHighDateTime = value.HighPart;
        SYSTEMTIME utc = {}, local = {};
        FileTimeToSystemTime(&file_time, &utc);
        SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
        wchar_t buffer[32];
        swprintf_s(buffer, L"%02d:%02d:%02d", local.wHour, local.wMinute, local.wSecond);
        text = L"Последняя проверка: " + std::wstring(buffer);
    } else {
        text = L"Ещё ни разу не проверялась";
    }

    if (task.kind == core::TaskKind::Similar && !task.source_title.empty()) {
        text += L"\nОбразец: " + util::widen(task.source_title);
    }
    if (!task.last_error.empty()) text += L"\n⚠ " + util::widen(task.last_error);

    task_hint_ = text;
    InvalidateRect(window_, nullptr, FALSE);
}

void MainWindow::update_buttons() {
    bool has_task = selected_task_id() > 0;
    for (ToolButton& tool : buttons_) {
        if (tool.id == kBtnEdit || tool.id == kBtnDelete || tool.id == kBtnCheck ||
            tool.id == kBtnOpen) {
            tool.enabled = has_task;
        }
    }
    InvalidateRect(window_, nullptr, FALSE);
}

int MainWindow::selected_task_id() const {
    int index = ListView_GetNextItem(task_list_, -1, LVNI_SELECTED);
    if (index < 0) return 0;
    LVITEMW item = {};
    item.mask = LVIF_PARAM;
    item.iItem = index;
    if (!ListView_GetItem(task_list_, &item)) return 0;
    return static_cast<int>(item.lParam);
}

bool MainWindow::selected_task(core::Task& out) const {
    int id = selected_task_id();
    return id > 0 && core::store().task(id, out);
}

void MainWindow::select_task(int task_id) {
    int count = ListView_GetItemCount(task_list_);
    for (int i = 0; i < count; ++i) {
        LVITEMW item = {};
        item.mask = LVIF_PARAM;
        item.iItem = i;
        if (ListView_GetItem(task_list_, &item) && static_cast<int>(item.lParam) == task_id) {
            ListView_SetItemState(task_list_, i, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            return;
        }
    }
}

void MainWindow::set_task_row_status(int task_id, core::TaskStatus status,
                                     const std::string& error) {
    (void)error;
    int count = ListView_GetItemCount(task_list_);
    for (int i = 0; i < count; ++i) {
        LVITEMW item = {};
        item.mask = LVIF_PARAM;
        item.iItem = i;
        if (!ListView_GetItem(task_list_, &item)) continue;
        if (static_cast<int>(item.lParam) != task_id) continue;

        std::wstring text = util::widen(core::status_title(status));
        ListView_SetItemText(task_list_, i, 3, text.data());

        core::Task task;
        if (core::store().task(task_id, task)) {
            std::wstring found = std::to_wstring(task.found_total);
            ListView_SetItemText(task_list_, i, 4, found.data());
        }
        return;
    }
}

void MainWindow::add_task() {
    TaskDialog dialog(engine_.get(), nullptr, core::settings().get().default_interval);
    if (!dialog.run(window_, L"Новая задача", 620, 548)) return;

    core::Task task = core::store().add_task(dialog.result());
    log("Добавлена задача «" + task.name + "»");
    reload_tasks();
    select_task(task.id);

    if (task.enabled) {
        if (!engine_->running()) engine_->start();
        engine_->check_now(task.id);
    }
}

void MainWindow::edit_task() {
    core::Task task;
    if (!selected_task(task)) return;

    TaskDialog dialog(engine_.get(), &task, task.interval);
    if (!dialog.run(window_, L"Изменить задачу", 620, 548)) return;

    core::Task updated = dialog.result();
    core::store().update_task(updated);
    engine_->schedule_soon(updated.id);
    log("Задача «" + updated.name + "» изменена");
    reload_tasks();
}

void MainWindow::delete_task() {
    core::Task task;
    if (!selected_task(task)) return;

    if (!ask_yes_no(window_, L"Удалить задачу",
                    L"Удалить задачу «" + util::widen(task.name) + L"»?\n\n"
                    L"Её объявления пропадут из ленты, а история просмотренных "
                    L"объявлений будет забыта.")) {
        return;
    }

    core::store().delete_task(task.id);
    engine_->forget_task(task.id);
    log("Задача «" + task.name + "» удалена");
    reload_tasks();
    reload_feed();
}

void MainWindow::check_now() {
    core::Task task;
    if (!selected_task(task)) return;
    if (!engine_->running()) engine_->start();
    engine_->check_now(task.id);
    log("«" + task.name + "»: проверяю…");
}

void MainWindow::open_task_url() {
    core::Task task;
    if (!selected_task(task)) return;

    std::string url = task.search_url();
    if (url.empty()) url = task.url;
    if (url.empty()) {
        show_info(window_, L"Ссылки пока нет",
                  L"Ссылка на поиск появится после первой проверки задачи.");
        return;
    }
    ShellExecuteW(nullptr, L"open", util::widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// --- feed ----------------------------------------------------------------

void MainWindow::reload_feed() {
    core::Settings settings = core::settings().get();
    feed_->set_limit(static_cast<size_t>(settings.max_feed_items));

    int index = static_cast<int>(SendMessageW(filter_combo_, CB_GETCURSEL, 0, 0));
    int task_id = (index >= 0 && index < static_cast<int>(filter_ids_.size()))
                      ? filter_ids_[index]
                      : 0;
    feed_->set_items(
        core::store().recent_listings(static_cast<size_t>(settings.max_feed_items), task_id));
}

void MainWindow::clear_feed() {
    int index = static_cast<int>(SendMessageW(filter_combo_, CB_GETCURSEL, 0, 0));
    int task_id = (index >= 0 && index < static_cast<int>(filter_ids_.size()))
                      ? filter_ids_[index]
                      : 0;

    if (!ask_yes_no(window_, L"Очистить ленту",
                    task_id ? L"Убрать объявления из ленты по выбранной задаче?\n\n"
                              L"Список просмотренных объявлений сохранится, повторных "
                              L"уведомлений не будет."
                            : L"Убрать объявления из ленты полностью?\n\n"
                              L"Список просмотренных объявлений сохранится, повторных "
                              L"уведомлений не будет.")) {
        return;
    }
    core::store().clear_listings(task_id);
    reload_feed();
}

// --- settings ------------------------------------------------------------

void MainWindow::open_settings() {
    SettingsDialog dialog(core::settings().get());
    if (!dialog.run(window_, L"Настройки", 640, 560)) return;

    core::Settings updated = dialog.result();
    core::settings().set(updated);
    engine_->settings_changed();
    images_->set_proxy(updated.proxy);

    if (core::autostart_enabled() != updated.autostart) {
        if (!core::set_autostart(updated.autostart)) {
            log("⚠ Не удалось изменить автозапуск");
        }
    }
    reload_feed();
    log("Настройки сохранены");
}

void MainWindow::toggle_watching() {
    if (engine_->running()) {
        engine_->stop();
        return;
    }
    if (core::store().tasks().empty()) {
        show_info(window_, L"Нет задач", L"Сначала создайте хотя бы одну задачу слежения.");
        return;
    }
    engine_->start();
}

// --- tray ----------------------------------------------------------------

void MainWindow::add_tray_icon() {
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = WM_TRAY;
    data.hIcon = app_icon_small();
    wcscpy_s(data.szTip, kWindowTitle);
    tray_added_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
}

void MainWindow::remove_tray_icon() {
    if (!tray_added_) return;
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    tray_added_ = false;
}

void MainWindow::update_tray(bool alert) {
    if (!tray_added_) return;
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayId;
    data.uFlags = NIF_ICON | NIF_TIP;
    data.hIcon = app_icon_small(alert);
    std::wstring tip = kWindowTitle;
    if (alert && unseen_ > 0) tip += L" — новых объявлений: " + std::to_wstring(unseen_);
    wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void MainWindow::show_balloon(const std::wstring& title, const std::wstring& text) {
    if (!tray_added_) return;
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_NONE;
    wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void MainWindow::handle_tray(LPARAM lparam) {
    switch (LOWORD(lparam)) {
        case WM_LBUTTONUP:
            if (IsWindowVisible(window_) && !IsIconic(window_)) {
                ShowWindow(window_, SW_HIDE);
            } else {
                restore();
            }
            break;
        case WM_RBUTTONUP:
            show_tray_menu();
            break;
        case NIN_BALLOONUSERCLICK:
            if (has_last_listing_ && !last_listing_.url.empty()) {
                ShellExecuteW(nullptr, L"open", util::widen(last_listing_.url).c_str(), nullptr,
                              nullptr, SW_SHOWNORMAL);
            } else {
                restore();
            }
            break;
        default:
            break;
    }
}

void MainWindow::show_tray_menu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuShow, L"Показать окно");
    AppendMenuW(menu, MF_STRING, kMenuWatch,
                running_ ? L"Остановить слежение" : L"Начать слежение");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Выход");

    POINT point = {};
    GetCursorPos(&point);
    SetForegroundWindow(window_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::start_if_configured() {
    if (!engine_) return;
    if (!core::settings().get().autostart_watching) return;
    if (core::store().tasks().empty()) return;
    engine_->start();
}

void MainWindow::restore() {
    ShowWindow(window_, SW_SHOW);
    if (IsIconic(window_)) ShowWindow(window_, SW_RESTORE);
    SetForegroundWindow(window_);
    unseen_ = 0;
    update_tray(false);
}

void MainWindow::log(const std::string& text) {
    if (!journal_) return;
    std::wstring line = util::widen(util::clock_string() + "  " + text) + L"\r\n";

    int length = GetWindowTextLengthW(journal_);
    // Keep the journal from growing without bound during long runs.
    if (length > 60000) {
        SetWindowTextW(journal_, L"");
        length = 0;
    }
    SendMessageW(journal_, EM_SETSEL, length, length);
    SendMessageW(journal_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(journal_, EM_SCROLLCARET, 0, 0);
}

void MainWindow::shutdown() {
    if (quitting_) return;
    quitting_ = true;

    KillTimer(window_, kTimerRefresh);

    // Remember where the window was.
    WINDOWPLACEMENT placement = {};
    placement.length = sizeof(placement);
    if (GetWindowPlacement(window_, &placement)) {
        core::Settings settings = core::settings().get();
        settings.window_placement = util::base64_encode(
            reinterpret_cast<const unsigned char*>(&placement), sizeof(placement));
        core::settings().set(settings);
    }

    if (engine_) engine_->stop();
    if (images_) images_->shutdown();
    remove_tray_icon();
}

// --- engine callbacks (worker thread) ------------------------------------

void MainWindow::on_task_started(int task_id) {
    PostMessageW(window_, WM_TASK_STARTED, static_cast<WPARAM>(task_id), 0);
}

void MainWindow::on_task_finished(int task_id, core::TaskStatus status,
                                  const std::string& error) {
    auto* payload = new TaskFinishedPayload{task_id, status, error};
    PostMessageW(window_, WM_TASK_FINISHED, 0, reinterpret_cast<LPARAM>(payload));
}

void MainWindow::on_new_listings(int task_id, const std::vector<core::Listing>& items) {
    auto* payload = new NewListingsPayload{task_id, items};
    PostMessageW(window_, WM_NEW_LISTINGS, 0, reinterpret_cast<LPARAM>(payload));
}

void MainWindow::on_notify_errors(const std::vector<std::string>& errors) {
    auto* payload = new std::vector<std::string>(errors);
    PostMessageW(window_, WM_NOTIFY_ERRORS, 0, reinterpret_cast<LPARAM>(payload));
}

void MainWindow::on_message(const std::string& text) {
    auto* payload = new std::string(text);
    PostMessageW(window_, WM_ENGINE_MESSAGE, 0, reinterpret_cast<LPARAM>(payload));
}

void MainWindow::on_running_changed(bool running) {
    PostMessageW(window_, WM_RUNNING, running ? 1 : 0, 0);
}

}  // namespace ui
