// Application settings: schedule, notification channels and network.
#pragma once

#include <vector>

#include "core/config.h"
#include "ui/dialog.h"

namespace ui {

class SettingsDialog : public ModalDialog {
public:
    explicit SettingsDialog(const core::Settings& settings);

    // Valid after run() returned true.
    const core::Settings& result() const { return settings_; }

protected:
    void build() override;
    void on_command(int control_id, int notification) override;
    bool on_accept() override;
    bool on_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result) override;

private:
    void build_general(int top);
    void build_telegram(int top);
    void build_mail(int top);
    void build_network(int top);
    void select_page(int index);
    void apply_mail_preset();
    void collect();
    void start_test(int which);

    core::Settings settings_;
    // The common tab control cannot be themed dark without white edges
    // showing through, so the strip is four owner-drawn buttons instead.
    HWND tab_buttons_[4] = {nullptr, nullptr, nullptr, nullptr};
    int page_ = 0;
    bool testing_ = false;

    std::vector<HWND> page_controls_[4];

    // General
    HWND interval_ = nullptr, feed_items_ = nullptr, history_days_ = nullptr;
    HWND tray_ = nullptr, start_min_ = nullptr, autostart_ = nullptr, autowatch_ = nullptr;
    HWND toast_ = nullptr, sound_ = nullptr;

    // Telegram
    HWND tg_enabled_ = nullptr, tg_token_ = nullptr, tg_chat_ = nullptr;
    HWND tg_photo_ = nullptr, tg_resolve_ = nullptr, tg_test_ = nullptr;

    // Mail
    HWND mail_enabled_ = nullptr, smtp_user_ = nullptr, smtp_password_ = nullptr;
    HWND mail_to_ = nullptr, smtp_host_ = nullptr, smtp_port_ = nullptr;
    HWND smtp_security_ = nullptr, mail_test_ = nullptr;

    // Network
    HWND delay_min_ = nullptr, delay_max_ = nullptr, timeout_ = nullptr, proxy_ = nullptr;
    HWND browser_fallback_ = nullptr, browser_first_ = nullptr, browser_status_ = nullptr;
};

}  // namespace ui
