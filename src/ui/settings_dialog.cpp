#include "ui/settings_dialog.h"

#include <commctrl.h>

#include <memory>
#include <thread>

#include "net/browser.h"
#include "notify/notify.h"
#include "ui/theme.h"
#include "util/strings.h"

namespace ui {
namespace {

enum Ids {
    kTab0 = 1200, kTab1, kTab2, kTab3,
    kInterval, kFeedItems, kHistoryDays,
    kTray, kStartMin, kAutostart, kAutowatch, kToast, kSound,
    kTgEnabled, kTgToken, kTgChat, kTgPhoto, kTgResolve, kTgTest,
    kMailEnabled, kSmtpUser, kSmtpPassword, kMailTo, kSmtpHost, kSmtpPort,
    kSmtpSecurity, kMailTest,
    kDelayMin, kDelayMax, kTimeout, kProxy, kBrowserFallback, kBrowserFirst,
};

constexpr UINT WM_TEST_DONE = WM_APP + 12;

enum TestKind { kTestTelegram = 1, kTestMail = 2, kTestResolve = 3 };

struct TestPayload {
    int kind = 0;
    bool ok = false;
    std::string info;
    std::string error;
};

double parse_double(const std::wstring& text, double fallback) {
    std::string narrow = util::replace_all(util::narrow(text), ",", ".");
    try {
        return std::stod(narrow);
    } catch (...) {
        return fallback;
    }
}

int parse_int(const std::wstring& text, int fallback) {
    long long value = 0;
    if (util::parse_number(util::narrow(text), value)) return static_cast<int>(value);
    return fallback;
}

}  // namespace

SettingsDialog::SettingsDialog(const core::Settings& settings) : settings_(settings) {}

void SettingsDialog::build() {
    const wchar_t* titles[] = {L"Общее", L"Telegram", L"Почта", L"Сеть"};
    HDC dc = GetDC(window());
    int x = 16;
    for (int i = 0; i < 4; ++i) {
        int width = text_width(dc, titles[i], font_ui()) + 28;
        tab_buttons_[i] = button(titles[i], x, 12, width, 30, kTab0 + i);
        x += width + 4;
    }
    ReleaseDC(window(), dc);

    const int top = 56;
    build_general(top);
    build_telegram(top);
    build_mail(top);
    build_network(top);

    button(L"Сохранить", 396, 508, 110, 30, IDOK, true);
    button(L"Отмена", 516, 508, 100, 30, IDCANCEL);

    select_page(0);
}

void SettingsDialog::build_general(int top) {
    auto& controls = page_controls_[0];
    const int left = 24;
    const int field = 200;

    controls.push_back(label(L"Поведение окна", left, top, 240, 18));
    tray_ = check(L"Сворачивать в область уведомлений вместо закрытия", left, top + 24, 420, 22,
                  kTray, settings_.minimize_to_tray);
    start_min_ = check(L"Запускаться свёрнутым в трей", left, top + 50, 420, 22, kStartMin,
                       settings_.start_minimized);
    autostart_ = check(L"Запускать вместе с Windows", left, top + 76, 420, 22, kAutostart,
                       settings_.autostart);
    autowatch_ = check(L"Начинать слежение сразу при запуске", left, top + 102, 420, 22,
                       kAutowatch, settings_.autostart_watching);
    controls.insert(controls.end(), {tray_, start_min_, autostart_, autowatch_});

    controls.push_back(label(L"Уведомления в приложении", left, top + 140, 300, 18));
    toast_ = check(L"Показывать всплывающее уведомление Windows", left, top + 164, 420, 22,
                   kToast, settings_.notify_desktop);
    sound_ = check(L"Звуковой сигнал при новом объявлении", left, top + 190, 420, 22, kSound,
                   settings_.notify_sound);
    controls.insert(controls.end(), {toast_, sound_});

    controls.push_back(label(L"Расписание и история", left, top + 228, 300, 18));

    controls.push_back(label(L"Интервал по умолчанию, сек:", left, top + 258, 220, 20));
    interval_ = number(settings_.default_interval, left + 230, top + 254, 110, 24, kInterval);
    controls.push_back(interval_);

    controls.push_back(label(L"Объявлений в ленте:", left, top + 288, 220, 20));
    feed_items_ = number(settings_.max_feed_items, left + 230, top + 284, 110, 24, kFeedItems);
    controls.push_back(feed_items_);

    controls.push_back(label(L"Помнить просмотренные, дней:", left, top + 318, 220, 20));
    history_days_ = number(settings_.keep_history_days, left + 230, top + 314, 110, 24,
                           kHistoryDays);
    controls.push_back(history_days_);
    (void)field;
}

void SettingsDialog::build_telegram(int top) {
    auto& controls = page_controls_[1];
    const int left = 24;
    const int label_width = 150;
    const int field_left = left + label_width + 8;

    HWND hint = label(
        L"Как подключить: напишите в Telegram боту @BotFather команду /newbot — он выдаст "
        L"токен. Затем откройте своего нового бота, нажмите «Старт» и нажмите здесь "
        L"«Определить chat_id».",
        left, top, 570, 50);
    mark_hint(hint);
    controls.push_back(hint);

    tg_enabled_ = check(L"Присылать уведомления в Telegram", left, top + 58, 420, 22,
                        kTgEnabled, settings_.telegram_enabled);
    controls.push_back(tg_enabled_);

    controls.push_back(label(L"Токен бота:", left, top + 94, label_width, 20));
    tg_token_ = edit(util::widen(settings_.telegram_token), field_left, top + 90, 400, 24,
                     kTgToken, true);
    controls.push_back(tg_token_);

    controls.push_back(label(L"Chat ID:", left, top + 126, label_width, 20));
    tg_chat_ = edit(util::widen(settings_.telegram_chat_id), field_left, top + 122, 220, 24,
                    kTgChat);
    tg_resolve_ = button(L"Определить chat_id", field_left + 230, top + 121, 170, 26,
                         kTgResolve);
    controls.insert(controls.end(), {tg_chat_, tg_resolve_});

    tg_photo_ = check(L"Прикладывать фотографию объявления", left, top + 156, 420, 22,
                      kTgPhoto, settings_.telegram_with_photo);
    controls.push_back(tg_photo_);

    tg_test_ = button(L"Отправить пробное сообщение", left, top + 196, 240, 30, kTgTest);
    controls.push_back(tg_test_);
}

void SettingsDialog::build_mail(int top) {
    auto& controls = page_controls_[2];
    const int left = 24;
    const int label_width = 150;
    const int field_left = left + label_width + 8;

    HWND hint = label(
        L"Для Gmail, Яндекса и Mail.ru обычный пароль не подойдёт — создайте в настройках "
        L"почты пароль приложения. Сервер и порт подставятся сами, как только вы введёте "
        L"свой адрес.",
        left, top, 570, 50);
    mark_hint(hint);
    controls.push_back(hint);

    mail_enabled_ = check(L"Присылать уведомления на почту", left, top + 58, 420, 22,
                          kMailEnabled, settings_.email_enabled);
    controls.push_back(mail_enabled_);

    controls.push_back(label(L"Ваш адрес (логин):", left, top + 94, label_width, 20));
    smtp_user_ = edit(util::widen(settings_.smtp_user), field_left, top + 90, 400, 24,
                      kSmtpUser);
    controls.push_back(smtp_user_);

    controls.push_back(label(L"Пароль приложения:", left, top + 126, label_width, 20));
    smtp_password_ = edit(util::widen(settings_.smtp_password), field_left, top + 122, 400, 24,
                          kSmtpPassword, true);
    controls.push_back(smtp_password_);

    controls.push_back(label(L"Получатель:", left, top + 158, label_width, 20));
    mail_to_ = edit(util::widen(settings_.email_to), field_left, top + 154, 400, 24, kMailTo);
    controls.push_back(mail_to_);

    controls.push_back(label(L"SMTP-сервер:", left, top + 190, label_width, 20));
    smtp_host_ = edit(util::widen(settings_.smtp_host), field_left, top + 186, 260, 24,
                      kSmtpHost);
    controls.push_back(smtp_host_);
    controls.push_back(label(L"порт", field_left + 270, top + 190, 40, 20));
    smtp_port_ = number(settings_.smtp_port, field_left + 312, top + 186, 88, 24, kSmtpPort);
    controls.push_back(smtp_port_);

    controls.push_back(label(L"Шифрование:", left, top + 222, label_width, 20));
    smtp_security_ = combo(field_left, top + 218, 260, 26, kSmtpSecurity,
                           {L"SSL (обычно порт 465)", L"STARTTLS (обычно порт 587)"},
                           settings_.smtp_ssl ? 0 : 1);
    controls.push_back(smtp_security_);

    mail_test_ = button(L"Отправить пробное письмо", left, top + 258, 240, 30, kMailTest);
    controls.push_back(mail_test_);
}

void SettingsDialog::build_network(int top) {
    auto& controls = page_controls_[3];
    const int left = 24;
    const int label_width = 190;
    const int field_left = left + label_width + 8;

    HWND hint = label(
        L"Авито ограничивает частые обращения. Не ставьте паузы меньше нескольких секунд "
        L"и интервал проверки меньше минуты — иначе сайт начнёт отдавать проверку на робота.",
        left, top, 570, 36);
    mark_hint(hint);
    controls.push_back(hint);

    controls.push_back(label(L"Пауза между запросами, сек:", left, top + 50, label_width, 20));
    wchar_t buffer[32];
    swprintf_s(buffer, L"%.1f", settings_.request_delay_min);
    delay_min_ = edit(buffer, field_left, top + 46, 80, 24, kDelayMin);
    controls.push_back(delay_min_);
    controls.push_back(label(L"до", field_left + 90, top + 50, 24, 20));
    swprintf_s(buffer, L"%.1f", settings_.request_delay_max);
    delay_max_ = edit(buffer, field_left + 118, top + 46, 80, 24, kDelayMax);
    controls.push_back(delay_max_);

    controls.push_back(label(L"Таймаут запроса, сек:", left, top + 82, label_width, 20));
    timeout_ = number(settings_.request_timeout, field_left, top + 78, 80, 24, kTimeout);
    controls.push_back(timeout_);

    controls.push_back(label(L"Прокси:", left, top + 114, label_width, 20));
    proxy_ = edit(util::widen(settings_.proxy), field_left, top + 110, 330, 24, kProxy);
    controls.push_back(proxy_);
    HWND proxy_hint = label(L"например http://user:pass@host:port — необязательно",
                            field_left, top + 138, 330, 18);
    mark_hint(proxy_hint);
    controls.push_back(proxy_hint);

    browser_fallback_ = check(L"При блокировке повторять запрос через установленный браузер",
                              left, top + 166, 540, 22, kBrowserFallback,
                              settings_.browser_fallback);
    browser_first_ = check(L"Всегда ходить через браузер (медленнее, но надёжнее)", left,
                           top + 192, 540, 22, kBrowserFirst, settings_.browser_first);
    controls.insert(controls.end(), {browser_fallback_, browser_first_});

    std::string found = net::browser_name();
    std::wstring status =
        found.empty()
            ? L"Chromium-браузер не найден. Установите Microsoft Edge или Google Chrome, "
              L"иначе запасной режим работать не будет."
            : L"Найден браузер для запасного режима: " + util::widen(found);
    browser_status_ = label(status, left, top + 226, 570, 40);
    if (found.empty()) mark_error(browser_status_); else mark_hint(browser_status_);
    controls.push_back(browser_status_);
}

void SettingsDialog::select_page(int index) {
    page_ = index;
    for (int i = 0; i < 4; ++i) {
        for (HWND control : page_controls_[i]) show(control, i == index);
        if (tab_buttons_[i]) InvalidateRect(tab_buttons_[i], nullptr, TRUE);
    }
    InvalidateRect(window(), nullptr, TRUE);
}

void SettingsDialog::apply_mail_preset() {
    if (!util::trim(util::narrow(text_of(smtp_host_))).empty()) return;

    std::string host;
    int port = 0;
    bool ssl = true;
    if (!core::smtp_preset_for(util::narrow(text_of(smtp_user_)), host, port, ssl)) return;

    set_text(smtp_host_, util::widen(host));
    set_text(smtp_port_, std::to_wstring(port));
    SendMessageW(smtp_security_, CB_SETCURSEL, ssl ? 0 : 1, 0);
    if (util::trim(util::narrow(text_of(mail_to_))).empty()) {
        set_text(mail_to_, text_of(smtp_user_));
    }
}

void SettingsDialog::collect() {
    settings_.minimize_to_tray = checked(tray_);
    settings_.start_minimized = checked(start_min_);
    settings_.autostart = checked(autostart_);
    settings_.autostart_watching = checked(autowatch_);
    settings_.notify_desktop = checked(toast_);
    settings_.notify_sound = checked(sound_);
    settings_.default_interval = parse_int(text_of(interval_), settings_.default_interval);
    settings_.max_feed_items = parse_int(text_of(feed_items_), settings_.max_feed_items);
    settings_.keep_history_days = parse_int(text_of(history_days_), settings_.keep_history_days);

    settings_.telegram_enabled = checked(tg_enabled_);
    settings_.telegram_token = util::trim(util::narrow(text_of(tg_token_)));
    settings_.telegram_chat_id = util::trim(util::narrow(text_of(tg_chat_)));
    settings_.telegram_with_photo = checked(tg_photo_);

    settings_.email_enabled = checked(mail_enabled_);
    settings_.smtp_user = util::trim(util::narrow(text_of(smtp_user_)));
    settings_.smtp_password = util::narrow(text_of(smtp_password_));
    settings_.email_to = util::trim(util::narrow(text_of(mail_to_)));
    settings_.smtp_host = util::trim(util::narrow(text_of(smtp_host_)));
    settings_.smtp_port = parse_int(text_of(smtp_port_), settings_.smtp_port);
    settings_.smtp_ssl = SendMessageW(smtp_security_, CB_GETCURSEL, 0, 0) == 0;

    settings_.request_delay_min = parse_double(text_of(delay_min_), settings_.request_delay_min);
    settings_.request_delay_max = parse_double(text_of(delay_max_), settings_.request_delay_max);
    if (settings_.request_delay_min < 0.5) settings_.request_delay_min = 0.5;
    if (settings_.request_delay_max < settings_.request_delay_min) {
        settings_.request_delay_max = settings_.request_delay_min;
    }
    settings_.request_timeout = parse_int(text_of(timeout_), settings_.request_timeout);
    settings_.proxy = util::trim(util::narrow(text_of(proxy_)));
    settings_.browser_fallback = checked(browser_fallback_);
    settings_.browser_first = checked(browser_first_);
}

void SettingsDialog::start_test(int which) {
    if (testing_) return;
    collect();
    testing_ = true;

    HWND button_control = which == kTestTelegram ? tg_test_
                          : which == kTestMail   ? mail_test_
                                                 : tg_resolve_;
    enable(button_control, false);
    set_text(button_control, which == kTestResolve ? L"Ищу…" : L"Отправляю…");

    HWND target = window();
    core::Settings snapshot = settings_;
    std::thread([target, snapshot, which] {
        auto* payload = new TestPayload();
        payload->kind = which;
        if (which == kTestTelegram) {
            payload->ok = notify::telegram::check(snapshot, payload->info, payload->error);
        } else if (which == kTestMail) {
            payload->ok = notify::mail::check(snapshot, payload->info, payload->error);
        } else {
            payload->ok =
                notify::telegram::resolve_chat_id(snapshot, payload->info, payload->error);
        }
        PostMessageW(target, WM_TEST_DONE, 0, reinterpret_cast<LPARAM>(payload));
    }).detach();
}

bool SettingsDialog::on_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result) {
    if (message == WM_DRAWITEM) {
        auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (!item) return false;

        int tab = -1;
        for (int i = 0; i < 4; ++i) {
            if (tab_buttons_[i] == item->hwndItem) tab = i;
        }
        if (tab < 0) return false;

        const bool active = tab == page_;
        fill_rect(item->hDC, item->rcItem, color::kWindow);
        if (active) {
            RECT underline = item->rcItem;
            underline.top = underline.bottom - 2;
            fill_rect(item->hDC, underline, color::kAccent);
        }

        wchar_t buffer[64] = {};
        GetWindowTextW(item->hwndItem, buffer, 64);
        draw_text(item->hDC, item->rcItem, buffer,
                  active ? color::kText : color::kTextMuted, font_ui(),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        result = TRUE;
        return true;
    }

    if (message != WM_TEST_DONE) return false;
    (void)wparam;
    result = 0;

    std::unique_ptr<TestPayload> payload(reinterpret_cast<TestPayload*>(lparam));
    testing_ = false;
    if (!payload) return true;

    if (payload->kind == kTestTelegram) {
        enable(tg_test_, true);
        set_text(tg_test_, L"Отправить пробное сообщение");
        if (payload->ok) {
            show_info(window(), L"Готово",
                      L"Сообщение отправлено ботом: " + util::widen(payload->info));
        } else {
            show_warning(window(), L"Не получилось", util::widen(payload->error));
        }
    } else if (payload->kind == kTestMail) {
        enable(mail_test_, true);
        set_text(mail_test_, L"Отправить пробное письмо");
        if (payload->ok) {
            show_info(window(), L"Готово",
                      L"Письмо отправлено на " + util::widen(payload->info));
        } else {
            show_warning(window(), L"Не получилось", util::widen(payload->error));
        }
    } else {
        enable(tg_resolve_, true);
        set_text(tg_resolve_, L"Определить chat_id");
        if (payload->ok) {
            set_text(tg_chat_, util::widen(payload->info));
            set_checked(tg_enabled_, true);
        } else {
            show_warning(window(), L"Не получилось", util::widen(payload->error));
        }
    }
    return true;
}

void SettingsDialog::on_command(int control_id, int notification) {
    if (control_id >= kTab0 && control_id <= kTab3) {
        if (notification == BN_CLICKED) select_page(control_id - kTab0);
        return;
    }
    switch (control_id) {
        case kTgTest: start_test(kTestTelegram); break;
        case kMailTest: start_test(kTestMail); break;
        case kTgResolve: start_test(kTestResolve); break;
        case kSmtpUser:
            if (notification == EN_KILLFOCUS) apply_mail_preset();
            break;
        default:
            break;
    }
}

bool SettingsDialog::on_accept() {
    collect();
    return true;
}

}  // namespace ui
