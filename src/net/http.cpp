#include "net/http.h"

#include <windows.h>
#include <winhttp.h>

#include "util/json.h"
#include "util/log.h"
#include "util/strings.h"

namespace net {
namespace {

constexpr const wchar_t* kUserAgent =
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    L"(KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36";

std::string last_error_text(const char* stage) {
    DWORD code = GetLastError();
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s (код %lu)", stage, static_cast<unsigned long>(code));
    return buffer;
}

}  // namespace

bool split_url(const std::string& url, std::wstring& host, unsigned short& port,
               std::wstring& path, bool& secure) {
    std::wstring wide = util::widen(url);
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    parts.dwSchemeLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts)) return false;

    host.assign(parts.lpszHostName, parts.dwHostNameLength);
    path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    port = parts.nPort;
    return !host.empty();
}

HttpClient::HttpClient() {
    reopen_session();
}

HttpClient::~HttpClient() {
    if (session_) WinHttpCloseHandle(static_cast<HINTERNET>(session_));
}

void HttpClient::reopen_session() {
    if (session_) {
        WinHttpCloseHandle(static_cast<HINTERNET>(session_));
        session_ = nullptr;
    }

    HINTERNET session = nullptr;
    if (!proxy_.empty()) {
        std::wstring proxy = util::widen(proxy_);
        session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_NAMED_PROXY,
                              proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, 0);
    } else {
        session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) {
            // Older systems do not know the automatic proxy type.
            session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        }
    }
    if (!session) return;

    // Let WinHTTP negotiate the newest TLS the system supports.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

    // Avito always answers compressed; WinHTTP handles gzip and deflate for us.
    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(session, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));

    int ms = timeout_seconds_ * 1000;
    WinHttpSetTimeouts(session, ms, ms, ms, ms);
    session_ = session;
}

void HttpClient::set_proxy(const std::string& proxy) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (proxy_ == proxy) return;
    proxy_ = proxy;
    reopen_session();
}

void HttpClient::set_timeout_seconds(int seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    timeout_seconds_ = seconds > 0 ? seconds : 30;
    if (session_) {
        int ms = timeout_seconds_ * 1000;
        WinHttpSetTimeouts(static_cast<HINTERNET>(session_), ms, ms, ms, ms);
    }
}

void HttpClient::set_cookies_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    cookies_enabled_ = enabled;
}

std::string HttpClient::cookie_header_locked() const {
    std::string header;
    for (const auto& [name, value] : cookies_) {
        if (!header.empty()) header += "; ";
        header += name;
        header += '=';
        header += value;
    }
    return header;
}

void HttpClient::absorb_cookies_locked(const std::wstring& raw_headers) {
    // Raw headers arrive as CRLF separated lines; pick out every Set-Cookie.
    std::string headers = util::narrow(raw_headers);
    size_t pos = 0;
    while (pos < headers.size()) {
        size_t end = headers.find("\r\n", pos);
        if (end == std::string::npos) end = headers.size();
        std::string line = headers.substr(pos, end - pos);
        pos = end + 2;
        if (line.size() < 12) continue;
        if (util::to_lower(line.substr(0, 11)) != "set-cookie:") continue;

        std::string body = util::trim(line.substr(11));
        size_t semicolon = body.find(';');
        std::string pair = semicolon == std::string::npos ? body : body.substr(0, semicolon);
        size_t equals = pair.find('=');
        if (equals == std::string::npos) continue;
        std::string name = util::trim(pair.substr(0, equals));
        std::string value = util::trim(pair.substr(equals + 1));
        if (name.empty()) continue;
        if (value.empty() || value == "deleted") {
            cookies_.erase(name);
        } else {
            cookies_[name] = value;
        }
    }
}

Response HttpClient::get(const std::string& url, const Headers& headers) {
    return request(L"GET", url, {}, {}, headers);
}

Response HttpClient::post(const std::string& url, const std::string& body,
                          const std::string& content_type, const Headers& headers) {
    return request(L"POST", url, body, content_type, headers);
}

Response HttpClient::request(const std::wstring& method, const std::string& url,
                             const std::string& body, const std::string& content_type,
                             const Headers& headers) {
    Response response;

    std::wstring host, path;
    unsigned short port = 0;
    bool secure = false;
    if (!split_url(url, host, port, path, secure)) {
        response.error = "Некорректный адрес: " + url;
        return response;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!session_) reopen_session();
    if (!session_) {
        response.error = "Не удалось открыть сетевую сессию";
        return response;
    }

    HINTERNET connection = WinHttpConnect(static_cast<HINTERNET>(session_), host.c_str(), port, 0);
    if (!connection) {
        response.error = last_error_text("Не удалось подключиться");
        return response;
    }

    DWORD flags = WINHTTP_FLAG_REFRESH | (secure ? WINHTTP_FLAG_SECURE : 0);
    HINTERNET request_handle = WinHttpOpenRequest(connection, method.c_str(), path.c_str(),
                                                  nullptr, WINHTTP_NO_REFERER,
                                                  WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request_handle) {
        WinHttpCloseHandle(connection);
        response.error = last_error_text("Не удалось создать запрос");
        return response;
    }

    // We manage cookies by hand so they survive for the whole session and can
    // be inspected; WinHTTP's own jar is disabled to avoid duplicates.
    DWORD disable = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(request_handle, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));

    std::wstring header_block;
    for (const auto& [name, value] : headers) {
        header_block += util::widen(name + ": " + value + "\r\n");
    }
    if (!content_type.empty()) {
        header_block += util::widen("Content-Type: " + content_type + "\r\n");
    }
    if (cookies_enabled_) {
        std::string cookie = cookie_header_locked();
        if (!cookie.empty()) header_block += util::widen("Cookie: " + cookie + "\r\n");
    }

    BOOL sent = WinHttpSendRequest(
        request_handle,
        header_block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header_block.c_str(),
        header_block.empty() ? 0 : static_cast<DWORD>(header_block.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);

    if (sent) sent = WinHttpReceiveResponse(request_handle, nullptr);

    if (!sent) {
        response.error = last_error_text("Запрос не прошёл");
        WinHttpCloseHandle(request_handle);
        WinHttpCloseHandle(connection);
        return response;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request_handle,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<int>(status);

    if (cookies_enabled_) {
        DWORD header_size = 0;
        WinHttpQueryHeaders(request_handle, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                            WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &header_size,
                            WINHTTP_NO_HEADER_INDEX);
        if (header_size > 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            std::wstring raw(header_size / sizeof(wchar_t) + 1, L'\0');
            if (WinHttpQueryHeaders(request_handle, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                    WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &header_size,
                                    WINHTTP_NO_HEADER_INDEX)) {
                absorb_cookies_locked(raw);
            }
        }
    }

    std::string data;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request_handle, &available) && available > 0) {
        size_t offset = data.size();
        data.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request_handle, data.data() + offset, available, &read)) {
            data.resize(offset);
            break;
        }
        data.resize(offset + read);
        if (read == 0) break;
        // A runaway response must not eat all the memory.
        if (data.size() > 64u * 1024 * 1024) break;
    }

    response.body = std::move(data);
    response.ok = true;

    WinHttpCloseHandle(request_handle);
    WinHttpCloseHandle(connection);
    return response;
}

void HttpClient::load_cookies(const std::string& json_text) {
    util::Json root = util::Json::parse(json_text);
    if (!root.is_object()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [name, value] : root.items()) {
        if (value.is_string()) cookies_[name] = value.as_string();
    }
}

std::string HttpClient::dump_cookies() const {
    std::lock_guard<std::mutex> lock(mutex_);
    util::Json root = util::Json::object();
    for (const auto& [name, value] : cookies_) root[name] = util::Json(value);
    return root.dump();
}

}  // namespace net
