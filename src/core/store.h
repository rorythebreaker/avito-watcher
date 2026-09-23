// Persistence for tasks, the feed and the set of already seen listings.
//
// The data is small and only ever appended to or rewritten whole, so three
// plain files in %APPDATA% do the job without pulling a database engine into
// the build:
//   tasks.json     - the watch list
//   listings.json  - the feed shown in the window, capped in size
//   seen.txt       - "task id <tab> listing id <tab> timestamp" per line
#pragma once

#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/models.h"

namespace core {

class Store {
public:
    Store();

    // --- tasks ---
    std::vector<Task> tasks() const;
    bool task(int id, Task& out) const;
    Task add_task(Task value);
    void update_task(const Task& value);
    void delete_task(int id);
    void set_task_status(int id, TaskStatus status, const std::string& error);

    // --- seen listings ---
    // Returns the subset of ids never reported for this task before.
    std::vector<std::string> filter_new(int task_id, const std::vector<std::string>& ids) const;
    void mark_seen(int task_id, const std::vector<std::string>& ids);
    size_t seen_count(int task_id) const;

    // --- feed ---
    void add_listings(const std::vector<Listing>& items);
    // task_id 0 means "every task".
    std::vector<Listing> recent_listings(size_t limit, int task_id = 0) const;
    void clear_listings(int task_id = 0);

    // Drops old seen entries and trims the feed. Called once an hour.
    void prune(int keep_days, size_t max_items);

private:
    static std::string seen_key(int task_id, const std::string& item_id);
    void load_tasks();
    void load_seen();
    void load_listings();
    bool save_tasks_locked() const;
    bool save_listings_locked() const;
    bool rewrite_seen_locked() const;

    mutable std::mutex mutex_;
    std::vector<Task> tasks_;
    std::vector<Listing> listings_;
    std::unordered_set<std::string> seen_;
    // Kept alongside seen_ so pruning can drop entries by age.
    std::vector<std::pair<std::string, double>> seen_log_;
    int next_task_id_ = 1;
};

Store& store();

}  // namespace core
