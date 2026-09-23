// A small modal dialog framework.
//
// Dialog templates in the resource script cannot be themed dark, so each dialog
// is an ordinary popup window with child controls placed by hand. Buttons,
// checkboxes, radio buttons and combo boxes are owner drawn here so the whole
// app looks the same; edits and lists get their colours from WM_CTLCOLOR*.
#pragma once

#include <windows.h>

#include <map>
#include <string>
#include <vector>

namespace ui {

class ModalDialog {
public:
    virtual ~ModalDialog() = default;

    // Shows the dialog and returns true when the user accepted it.
    bool run(HWND parent, const std::wstring& title, int width, int height);

protected:
    // Called once after the window exists; create the controls here.
    virtual void build() = 0;
    virtual void on_command(int control_id, int notification) { (void)control_id; (void)notification; }
    virtual void on_scroll(HWND control) { (void)control; }
    // Return false to keep the dialog open (validation failed).
    virtual bool on_accept() { return true; }
    // Handle a message the framework does not know about, such as a result
    // posted from a worker thread. Return true when it was consumed.
    virtual bool on_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result) {
        (void)message; (void)wparam; (void)lparam; (void)result;
        return false;
    }

    HWND label(const std::wstring& text, int x, int y, int w, int h, int id = -1);
    HWND edit(const std::wstring& text, int x, int y, int w, int h, int id,
              bool password = false, bool multiline = false);
    HWND number(int value, int x, int y, int w, int h, int id);
    HWND button(const std::wstring& text, int x, int y, int w, int h, int id,
                bool primary = false);
    HWND check(const std::wstring& text, int x, int y, int w, int h, int id, bool checked);
    HWND radio(const std::wstring& text, int x, int y, int w, int h, int id, bool checked,
               bool group_start);
    HWND combo(int x, int y, int w, int h, int id, const std::vector<std::wstring>& items,
               int selected);
    HWND slider(int x, int y, int w, int h, int id, int low, int high, int value);
    HWND group(const std::wstring& text, int x, int y, int w, int h);

    std::wstring text_of(HWND control) const;
    void set_text(HWND control, const std::wstring& text);
    bool checked(HWND control) const;
    void set_checked(HWND control, bool value);
    int slider_value(HWND control) const;
    void enable(HWND control, bool value);
    void show(HWND control, bool value);

    HWND window() const { return window_; }
    HWND control(int id) const { return GetDlgItem(window_, id); }

    // Marks a label as a hint so it is painted in the muted colour.
    void mark_hint(HWND control);
    void mark_error(HWND control);

private:
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
    void draw_item(DRAWITEMSTRUCT* item);
    void toggle_clicked(HWND control);
    HWND add(const wchar_t* class_name, const std::wstring& text, DWORD style, DWORD ex_style,
             int x, int y, int w, int h, int id);

    HWND window_ = nullptr;
    HWND parent_ = nullptr;
    bool accepted_ = false;
    bool finished_ = false;
    std::vector<HWND> hints_;
    // Text fields carry no border of their own, so the dialog draws one
    // around each of them; the sunken 3D edge Windows offers looks wrong
    // on a dark background.
    std::vector<HWND> fields_;
    std::vector<HWND> errors_;
    std::vector<HWND> primary_buttons_;
    // BS_OWNERDRAW lives in the same bits as BS_AUTOCHECKBOX, so a button
    // cannot be both owner drawn and an automatic toggle. Owner-drawn
    // toggles therefore keep their state here; radio buttons additionally
    // remember which group they belong to.
    std::map<HWND, bool> toggles_;
    std::map<HWND, int> radio_group_;
    int current_radio_group_ = 0;
};

// Message box styled like the rest of the app is not worth the code; these wrap
// the system one with the right icon and caption.
void show_info(HWND parent, const std::wstring& title, const std::wstring& text);
void show_warning(HWND parent, const std::wstring& title, const std::wstring& text);
bool ask_yes_no(HWND parent, const std::wstring& title, const std::wstring& text);

}  // namespace ui
