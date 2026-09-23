#include "net/browser.h"

#include <windows.h>

#include <chrono>
#include <thread>

#include "net/http.h"
#include "net/websocket.h"
#include "util/json.h"
#include "util/log.h"
#include "util/paths.h"
#include "util/strings.h"

namespace net {
namespace {

using util::Json;

// Edge first: it is always present on Windows 11.
const wchar_t* kCandidates[] = {
    L"%ProgramFiles(x86)%\\Microsoft\\Edge\\Application\\msedge.exe",
    L"%ProgramFiles%\\Microsoft\\Edge\\Application\\msedge.exe",
    L"%ProgramFiles%\\Google\\Chrome\\Application\\chrome.exe",
    L"%ProgramFiles(x86)%\\Google\\Chrome\\Application\\chrome.exe",
    L"%LocalAppData%\\Google\\Chrome\\Application\\chrome.exe",
    L"%ProgramFiles%\\BraveSoftware\\Brave-Browser\\Application\\brave.exe",
};

constexpr const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36";

std::wstring expand(const wchar_t* pattern) {
    wchar_t buffer[MAX_PATH * 2] = {};
    DWORD length = ExpandEnvironmentStringsW(pattern, buffer, MAX_PATH * 2);
    if (length == 0 || length > MAX_PATH * 2) return {};
    return std::wstring(buffer, length - 1);
}

void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

double monotonic_seconds() {
    return static_cast<double>(GetTickCount64()) / 1000.0;
}

// The browser relaunches itself and the process we spawned exits right away, so
// the only reliable way to stop our instance is to ask it over the protocol.
class BrowserProcess {
public:
    ~BrowserProcess() { stop(); }

    bool start(const std::wstring& exe, const std::wstring& profile,
               const std::string& proxy, double deadline, std::string& error) {
        profile_ = profile;
        util::ensure_dir(profile);
        // A stale port file would point at a browser that is already gone.
        util::delete_file(profile + L"\\DevToolsActivePort");

        std::wstring command =
            L"\"" + exe + L"\""
            L" --headless=new"
            L" --disable-gpu"
            L" --disable-extensions"
            L" --disable-background-networking"
            L" --disable-sync"
            L" --disable-blink-features=AutomationControlled"
            L" --no-first-run"
            L" --no-default-browser-check"
            L" --mute-audio"
            L" --window-size=1440,900"
            L" --lang=ru-RU"
            L" --user-agent=\"" + util::widen(kUserAgent) + L"\""
            L" --user-data-dir=\"" + profile + L"\""
            L" --remote-debugging-port=0";
        if (!proxy.empty()) command += L" --proxy-server=\"" + util::widen(proxy) + L"\"";
        command += L" about:blank";

        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION info = {};

        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');

        if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) {
            error = "Не удалось запустить браузер";
            return false;
        }
        CloseHandle(info.hThread);
        process_ = info.hProcess;

        port_ = wait_for_port(deadline);
        if (port_ == 0) {
            error = "Браузер не открыл отладочный порт за отведённое время";
            return false;
        }
        return true;
    }

    int port() const { return port_; }

    void stop() {
        if (port_ > 0) {
            // Ask the browser to close itself; our own user-data-dir means this
            // never touches windows the user opened.
            HttpClient client;
            client.set_timeout_seconds(5);
            Response version = client.get(endpoint("/json/version"));
            if (version.ok) {
                Json root = Json::parse(version.body);
                std::string socket_url = root["webSocketDebuggerUrl"].as_string();
                if (!socket_url.empty()) {
                    WebSocket socket;
                    if (socket.connect(socket_url, 5)) {
                        Json command = Json::object();
                        command["id"] = Json(1);
                        command["method"] = Json("Browser.close");
                        socket.send_text(command.dump());
                        sleep_ms(300);
                        socket.close();
                    }
                }
            }
            port_ = 0;
        }
        if (process_) {
            if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) TerminateProcess(process_, 0);
            CloseHandle(process_);
            process_ = nullptr;
        }
    }

    std::string endpoint(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

private:
    // The browser writes the port it picked into DevToolsActivePort. Watching
    // the spawned process is pointless: it exits with code 0 almost at once
    // while the real browser keeps starting up.
    int wait_for_port(double deadline) {
        const std::wstring marker = profile_ + L"\\DevToolsActivePort";
        while (monotonic_seconds() < deadline) {
            std::string text;
            if (util::read_file(marker, text)) {
                size_t newline = text.find('\n');
                std::string first = util::trim(newline == std::string::npos
                                                   ? text
                                                   : text.substr(0, newline));
                int value = std::atoi(first.c_str());
                if (value > 0) return value;
            }
            sleep_ms(200);
        }
        return 0;
    }

    std::wstring profile_;
    HANDLE process_ = nullptr;
    int port_ = 0;
};

}  // namespace

std::wstring find_browser() {
    for (const wchar_t* pattern : kCandidates) {
        std::wstring path = expand(pattern);
        if (!path.empty() && path.find(L'%') == std::wstring::npos &&
            util::file_exists(path)) {
            return path;
        }
    }
    return {};
}

std::string browser_name() {
    std::wstring path = find_browser();
    if (path.empty()) return {};
    size_t slash = path.find_last_of(L"\\/");
    return util::narrow(slash == std::wstring::npos ? path : path.substr(slash + 1));
}

bool browser_available() {
    return !find_browser().empty();
}

BrowserResult browser_fetch(const std::string& url, int timeout_seconds,
                            const std::string& proxy) {
    BrowserResult result;

    std::wstring exe = find_browser();
    if (exe.empty()) {
        result.error =
            "Не найден Chromium-браузер (Edge/Chrome). "
            "Установите Microsoft Edge или Google Chrome.";
        return result;
    }

    const double deadline = monotonic_seconds() + timeout_seconds;
    BrowserProcess browser;
    if (!browser.start(exe, util::browser_profile_dir(), proxy, deadline, result.error)) {
        return result;
    }

    // Find a page target to attach to.
    HttpClient client;
    client.set_timeout_seconds(15);
    std::string socket_url;
    while (monotonic_seconds() < deadline && socket_url.empty()) {
        Response targets = client.get(browser.endpoint("/json/list"));
        if (targets.ok) {
            Json list = Json::parse(targets.body);
            for (size_t i = 0; i < list.size(); ++i) {
                const Json& target = list.at(i);
                if (target["type"].as_string() == "page" &&
                    !target["webSocketDebuggerUrl"].as_string().empty()) {
                    socket_url = target["webSocketDebuggerUrl"].as_string();
                    break;
                }
            }
        }
        if (socket_url.empty()) {
            Response created = client.get(browser.endpoint("/json/new?about:blank"));
            if (created.ok) {
                socket_url = Json::parse(created.body)["webSocketDebuggerUrl"].as_string();
            }
        }
        if (socket_url.empty()) sleep_ms(300);
    }

    if (socket_url.empty()) {
        result.error = "Не удалось открыть вкладку в браузере";
        return result;
    }

    WebSocket socket;
    if (!socket.connect(socket_url, timeout_seconds)) {
        result.error = socket.error();
        return result;
    }

    int next_id = 0;
    auto call = [&](const std::string& method, const Json& params, Json& reply) -> bool {
        int id = ++next_id;
        Json command = Json::object();
        command["id"] = Json(id);
        command["method"] = Json(method);
        command["params"] = params.is_null() ? Json::object() : params;
        if (!socket.send_text(command.dump())) return false;

        while (monotonic_seconds() < deadline) {
            std::string text;
            if (!socket.receive_text(text)) return false;
            Json message = Json::parse(text);
            if (message["id"].as_int(-1) == id) {
                if (message.has("error")) {
                    result.error = message["error"]["message"].as_string("ошибка браузера");
                    return false;
                }
                reply = message["result"];
                return true;
            }
        }
        return false;
    };

    auto wait_for_event = [&](const std::string& event, double until) {
        while (monotonic_seconds() < until) {
            std::string text;
            if (!socket.receive_text(text)) return;
            if (Json::parse(text)["method"].as_string() == event) return;
        }
    };

    Json reply;
    if (!call("Page.enable", Json::object(), reply)) {
        if (result.error.empty()) result.error = "Браузер не принял команду";
        return result;
    }

    Json navigate = Json::object();
    navigate["url"] = Json(url);
    if (!call("Page.navigate", navigate, reply)) {
        if (result.error.empty()) result.error = "Браузер не открыл страницу";
        return result;
    }

    wait_for_event("Page.loadEventFired", std::min(deadline, monotonic_seconds() + timeout_seconds));
    sleep_ms(1500);  // let scripts finish painting

    // Cards below the fold only load their photos once scrolled into view.
    for (int step = 1; step <= 4; ++step) {
        Json scroll = Json::object();
        scroll["expression"] = Json("window.scrollTo(0, document.body.scrollHeight*" +
                                    std::to_string(step) + "/4)");
        Json ignored;
        if (!call("Runtime.evaluate", scroll, ignored)) break;
        sleep_ms(600);
    }
    {
        Json scroll = Json::object();
        scroll["expression"] = Json("window.scrollTo(0, 0)");
        Json ignored;
        call("Runtime.evaluate", scroll, ignored);
        sleep_ms(400);
    }

    Json evaluate = Json::object();
    evaluate["expression"] = Json("document.documentElement.outerHTML");
    evaluate["returnByValue"] = Json(true);
    if (!call("Runtime.evaluate", evaluate, reply)) {
        if (result.error.empty()) result.error = "Не удалось получить содержимое страницы";
        return result;
    }

    result.html = reply["result"]["value"].as_string();
    socket.close();

    if (result.html.size() < 500) {
        result.error = "Браузер вернул пустую страницу";
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace net
