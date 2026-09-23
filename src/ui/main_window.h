// The main window: task list, feed of found listings, journal and tray icon.
#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "avito/engine.h"
#include "core/models.h"
#include "ui/feed_view.h"
#include "ui/images.h"

namespace ui {

class MainWindow : public avito::EngineListener {
public:
    MainWindow();
    ~MainWindow() override;

    bool create(HINSTANCE instance, bool start_hidden);
    HWND handle() const { return window_; }

    void restore();
    void log(const std::string& text);
    // Starts watching when the settings ask for it and tasks exist.
    void start_if_configured();

    // EngineListener - all of these run on the worker thread and only post a
    // message; every control is touched on the UI thread.
    void on_task_started(int task_id) override;
    void on_task_finished(int task_id, core::TaskStatus status,
                          const std::string& error) override;
    void on_new_listings(int task_id, const std::vector<core::Listing>& items) override;
    void on_notify_errors(const std::vector<std::string>& errors) override;
    void on_message(const std::string& text) override;
    void on_running_changed(bool running) override;

private:
    struct ToolButton {
        int id = 0;
        std::wstring text;
        RECT rect = {};
        bool enabled = true;
        bool wide = false;
    };

    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);

    void build_children(HINSTANCE instance);
    void layout();
    void paint();
    void paint_toolbar(HDC dc, const RECT& client);
    void paint_status(HDC dc, const RECT& client);

    void reload_tasks();
    void reload_feed();
    void rebuild_filter();
    void update_counters();
    void update_task_hint();
    void update_buttons();

    int selected_task_id() const;
    bool selected_task(core::Task& out) const;
    void select_task(int task_id);

    void add_task();
    void edit_task();
    void delete_task();
    void check_now();
    void open_task_url();
    void open_settings();
    void toggle_watching();
    void clear_feed();

    void set_task_row_status(int task_id, core::TaskStatus status, const std::string& error);
    void handle_toolbar_click(int index);
    int button_at(int x, int y) const;

    void add_tray_icon();
    void remove_tray_icon();
    void update_tray(bool alert);
    void show_balloon(const std::wstring& title, const std::wstring& text);
    void handle_tray(LPARAM lparam);
    void show_tray_menu();

    void shutdown();

    HWND window_ = nullptr;
    HWND task_list_ = nullptr;
    HWND filter_combo_ = nullptr;
    HWND clear_button_ = nullptr;
    HWND journal_ = nullptr;
    HINSTANCE instance_ = nullptr;

    std::unique_ptr<ImageLoader> images_;
    std::unique_ptr<FeedView> feed_;
    std::unique_ptr<avito::Engine> engine_;

    std::vector<ToolButton> buttons_;
    int hovered_button_ = -1;
    int pressed_button_ = -1;

    std::wstring status_text_ = L"Слежение остановлено";
    std::wstring counter_text_;
    std::wstring task_hint_;

    std::vector<int> filter_ids_;   // combo index -> task id (0 = all)
    core::Listing last_listing_;
    bool has_last_listing_ = false;
    int unseen_ = 0;
    bool quitting_ = false;
    bool tray_added_ = false;
    bool running_ = false;
};

}  // namespace ui
