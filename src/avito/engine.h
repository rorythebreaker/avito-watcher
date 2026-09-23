// The watching loop.
//
// One worker thread serves every task, so requests to Avito go one at a time
// with a pause between them. Results reach the window through the listener,
// whose methods are called on the worker thread and are expected to marshal
// the data to the UI thread themselves.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "avito/parser.h"
#include "core/models.h"
#include "net/fetcher.h"

namespace avito {

// A task is never polled more often than this, whatever the interval says.
constexpr int kMinInterval = 60;

class EngineListener {
public:
    virtual ~EngineListener() = default;
    virtual void on_task_started(int task_id) = 0;
    virtual void on_task_finished(int task_id, core::TaskStatus status,
                                  const std::string& error) = 0;
    virtual void on_new_listings(int task_id, const std::vector<core::Listing>& items) = 0;
    virtual void on_notify_errors(const std::vector<std::string>& errors) = 0;
    virtual void on_message(const std::string& text) = 0;
    virtual void on_running_changed(bool running) = 0;
};

class Engine {
public:
    explicit Engine(EngineListener* listener);
    ~Engine();

    void start();
    void stop();
    bool running() const { return running_.load(); }

    void check_now(int task_id);
    void schedule_soon(int task_id, double delay_seconds = 2.0);
    void forget_task(int task_id);
    void settings_changed();

    net::Fetcher& fetcher() { return fetcher_; }

    // Loads a sample listing and reads its title, price, category and city.
    // Used by the engine itself and by the task dialog's "Проверить" button.
    bool load_item_info(const std::string& url, ItemInfo& out, std::string& error);

private:
    void loop();
    void tick();
    void run_task(core::Task task);
    bool check_task(core::Task& task, std::string& error, core::TaskStatus& status);
    bool ensure_similar_ready(core::Task& task, std::string& error);
    std::string pick_search_url(core::Task& task, const ItemInfo& info);
    bool passes_filters(const core::Task& task, core::Listing& listing) const;

    EngineListener* listener_ = nullptr;
    net::Fetcher fetcher_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<int> manual_;
    std::map<int, double> next_run_;   // task id -> unix time of the next check
    double last_prune_ = 0.0;
};

}  // namespace avito
