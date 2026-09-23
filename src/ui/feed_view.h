// The feed of found listings: a custom drawn, scrollable list of cards.
//
// A standard list control cannot show a photo, a price and two lines of detail
// per row the way this needs, so the whole thing is painted by hand into an
// off-screen bitmap and blitted in one go.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "core/models.h"
#include "ui/images.h"

namespace ui {

class FeedView {
public:
    static void register_class(HINSTANCE instance);

    HWND create(HWND parent, int control_id, ImageLoader* loader, HINSTANCE instance);
    HWND handle() const { return window_; }

    void set_items(std::vector<core::Listing> items);
    void prepend(const std::vector<core::Listing>& items);
    void set_limit(size_t limit);
    void clear();
    size_t count() const { return items_.size(); }

    void refresh();  // repaint, e.g. after a thumbnail arrived

private:
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

    void paint();
    void render(HDC target, const RECT& client);
    void update_scrollbar();
    void scroll_to(int offset);
    int card_height() const;
    int index_at(int y) const;
    void open_selected();
    void copy_link(bool all);
    void show_menu(int x, int y);

    HWND window_ = nullptr;
    ImageLoader* loader_ = nullptr;
    std::vector<core::Listing> items_;
    size_t limit_ = 500;
    int scroll_ = 0;
    int hovered_ = -1;
    int selected_ = -1;
};

}  // namespace ui
