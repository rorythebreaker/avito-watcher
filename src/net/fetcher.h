// Loading Avito pages: the fast HTTP path with a browser fallback.
//
// A plain request is tried first because it is quick and light on the site. If
// the answer is one of Avito's anti-bot stubs, the same URL is fetched once more
// through a real browser. Requests across the whole app are serialised with a
// randomised pause so the site never sees a burst.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "core/config.h"
#include "net/http.h"

namespace net {

enum class FetchStatus { Ok, Blocked, Failed };

struct FetchResult {
    FetchStatus status = FetchStatus::Failed;
    std::string html;
    std::string error;
    std::string transport;  // "http" or "browser", shown in the log

    bool ok() const { return status == FetchStatus::Ok; }
};

// Text of the page title, lowercased. Empty when there is no title.
std::string page_title(const std::string& html);

// True when Avito answered "nothing found" rather than blocking us.
bool is_empty_result(const std::string& html);

// Tells an anti-bot stub from a real page. The decision rests on the status code
// and the page title rather than on stray words in the markup: strings like
// "captcha" appear in ordinary Avito scripts and used to cause false alarms.
// `require` is a snippet of markup the page must contain to be useful.
bool looks_blocked(const std::string& html, int status, const std::string& require);

class Fetcher {
public:
    Fetcher();

    void apply_settings(const core::Settings& settings);

    // `stop` lets a long pause be interrupted when the user stops watching.
    FetchResult get(const std::string& url, const std::string& require,
                    const std::atomic<bool>* stop = nullptr);

    const std::string& last_transport() const { return last_transport_; }

private:
    void wait_turn(const std::atomic<bool>* stop);

    std::mutex mutex_;
    HttpClient client_;
    core::Settings settings_;
    double last_request_ = 0.0;   // monotonic seconds
    std::string last_transport_;
};

}  // namespace net
