// Creating and editing a watch task.
#pragma once

#include <string>

#include "avito/engine.h"
#include "core/models.h"
#include "ui/dialog.h"

namespace ui {

class TaskDialog : public ModalDialog {
public:
    // `task` is null when creating a new one.
    TaskDialog(avito::Engine* engine, const core::Task* task, int default_interval);
    ~TaskDialog() override;

    // Valid after run() returned true.
    const core::Task& result() const { return task_; }

protected:
    void build() override;
    void on_command(int control_id, int notification) override;
    void on_scroll(HWND control) override;
    bool on_accept() override;
    bool on_message(UINT message, WPARAM wparam, LPARAM lparam,
                    LRESULT& result) override;

private:
    void switch_page(bool similar);
    void update_search_preview();
    void update_sliders();
    void start_probe();
    void finish_probe(bool ok);
    std::string built_search_url() const;
    void show_sample(const std::string& title, long long price, const std::string& url);

    avito::Engine* engine_ = nullptr;
    core::Task task_;
    bool editing_ = false;
    bool probing_ = false;

    avito::ItemInfo info_;
    bool info_ready_ = false;

    // Controls
    HWND radio_search_ = nullptr;
    HWND radio_similar_ = nullptr;

    HWND search_hint_ = nullptr;
    HWND query_label_ = nullptr, query_edit_ = nullptr;
    HWND region_label_ = nullptr, region_combo_ = nullptr;
    HWND price_label_ = nullptr, price_min_ = nullptr, price_to_ = nullptr, price_max_ = nullptr;
    HWND url_label_ = nullptr, url_edit_ = nullptr;
    HWND search_preview_ = nullptr;

    HWND similar_hint_ = nullptr;
    HWND item_url_edit_ = nullptr, probe_button_ = nullptr;
    HWND item_preview_ = nullptr;
    HWND similarity_label_ = nullptr, similarity_slider_ = nullptr, similarity_value_ = nullptr;
    HWND tolerance_label_ = nullptr, tolerance_slider_ = nullptr, tolerance_value_ = nullptr;
    HWND similar_note_ = nullptr;

    HWND name_edit_ = nullptr;
    HWND interval_combo_ = nullptr;
    HWND exclude_edit_ = nullptr;
    HWND notify_existing_ = nullptr;
    HWND enabled_check_ = nullptr;
};

}  // namespace ui
