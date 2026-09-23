// Application settings, stored as JSON in %APPDATA%\AvitoWatcher\settings.json.
#pragma once

#include <mutex>
#include <string>

namespace core {

struct Settings {
    // Schedule and history
    int default_interval = 300;
    double request_delay_min = 3.0;
    double request_delay_max = 8.0;
    int max_feed_items = 500;
    int keep_history_days = 30;

    // Window behaviour
    bool minimize_to_tray = true;
    bool start_minimized = false;
    bool autostart = false;
    bool autostart_watching = true;

    // Network
    std::string proxy;
    bool browser_fallback = true;
    bool browser_first = false;   // skip the HTTP attempt entirely
    int request_timeout = 30;

    // In-app notifications
    bool notify_desktop = true;
    bool notify_sound = true;

    // Telegram
    bool telegram_enabled = false;
    std::string telegram_token;
    std::string telegram_chat_id;
    bool telegram_with_photo = true;

    // Email
    bool email_enabled = false;
    std::string smtp_host;
    int smtp_port = 465;
    bool smtp_ssl = true;         // true: implicit TLS (465), false: STARTTLS (587)
    std::string smtp_user;
    std::string smtp_password;
    std::string email_to;

    // Window placement, serialised as base64 of WINDOWPLACEMENT.
    std::string window_placement;
};

// Single shared settings object. Reads are cheap copies so worker threads never
// hold a lock while doing network calls.
class SettingsStore {
public:
    SettingsStore();

    Settings get() const;
    void set(const Settings& settings);
    bool save() const;

private:
    mutable std::mutex mutex_;
    Settings settings_;
};

SettingsStore& settings();

// Suggests SMTP host/port for well known mail providers. Returns false when the
// domain is unknown and the user has to fill the server in by hand.
bool smtp_preset_for(const std::string& address, std::string& host, int& port, bool& ssl);

}  // namespace core
