// Fallback page loader driving the browser already installed on the machine.
//
// Avito answers plain HTTP requests with a "Доступ ограничен" stub often enough
// that a real browser is needed. Edge ships with Windows 11, so the app starts
// it headless with the DevTools port open and pulls the rendered DOM over the
// protocol. Nothing is bundled: no Chromium, no driver.
//
// The --dump-dom switch is useless here: msedge.exe is a GUI subsystem binary
// and writes nothing to a redirected stdout, which is why this goes through the
// debugging port instead.
#pragma once

#include <string>

namespace net {

// Full path of the first Chromium-based browser found, empty when none exists.
std::wstring find_browser();
std::string browser_name();
bool browser_available();

struct BrowserResult {
    bool ok = false;
    std::string html;
    std::string error;
};

// Loads the page, waits for scripts, scrolls to pull in lazily loaded images
// and returns the resulting DOM.
BrowserResult browser_fetch(const std::string& url, int timeout_seconds,
                            const std::string& proxy);

}  // namespace net
