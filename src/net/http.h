// HTTPS client built on WinHTTP.
//
// Used for the fast path to Avito, for the Telegram API and for downloading
// listing thumbnails. Bodies are returned as std::string and may hold binary
// data, so the same client serves images and JSON alike.
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace net {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct Response {
    bool ok = false;          // the request completed; check status for the result
    int status = 0;
    std::string body;
    std::string error;        // set when ok is false
};

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    void set_proxy(const std::string& proxy);
    void set_timeout_seconds(int seconds);
    // Sends and stores cookies between requests, which keeps Avito's
    // anti-bot cookies alive for the whole session.
    void set_cookies_enabled(bool enabled);

    Response get(const std::string& url, const Headers& headers = {});
    Response post(const std::string& url, const std::string& body,
                  const std::string& content_type, const Headers& headers = {});

    void load_cookies(const std::string& json_text);
    std::string dump_cookies() const;

private:
    Response request(const std::wstring& method, const std::string& url,
                     const std::string& body, const std::string& content_type,
                     const Headers& headers);
    void reopen_session();
    std::string cookie_header_locked() const;
    void absorb_cookies_locked(const std::wstring& raw_headers);

    mutable std::mutex mutex_;
    void* session_ = nullptr;       // HINTERNET
    std::string proxy_;
    int timeout_seconds_ = 30;
    bool cookies_enabled_ = true;
    std::map<std::string, std::string> cookies_;
};

// Splits "https://host:port/path?query" into pieces. Returns false when the URL
// is not something WinHTTP can open.
bool split_url(const std::string& url, std::wstring& host, unsigned short& port,
               std::wstring& path, bool& secure);

}  // namespace net
