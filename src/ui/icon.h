// Tray icons drawn at run time.
//
// The window and the taskbar use the icon compiled into the executable; the
// tray needs a second version with an alert dot, so both are painted here from
// the same shape - a magnifier on a blue rounded square.
#pragma once

#include <windows.h>

namespace ui {

// Cached; never destroy the returned handles.
HICON app_icon(bool alert = false);
HICON app_icon_small(bool alert = false);

}  // namespace ui
