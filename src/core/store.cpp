#include "core/store.h"

#include <windows.h>

#include <algorithm>
#include <unordered_map>

#include "util/json.h"
#include "util/log.h"
#include "util/paths.h"
#include "util/strings.h"

namespace core {
namespace {

using util::Json;

Json task_to_json(const Task& t) {
    Json j = Json::object();
    j["id"] = Json(t.id);
    j["name"] = Json(t.name);
    j["kind"] = Json(kind_to_string(t.kind));
    j["enabled"] = Json(t.enabled);
    j["interval"] = Json(t.interval);
    j["url"] = Json(t.url);
    j["query"] = Json(t.query);
    j["region"] = Json(t.region);
    j["price_min"] = Json(t.price_min);
    j["price_max"] = Json(t.price_max);
    j["exclude"] = Json(t.exclude);
    j["manual_url"] = Json(t.manual_url);
    j["similarity"] = Json(t.similarity);
    j["price_tolerance"] = Json(t.price_tolerance);
    j["source_title"] = Json(t.source_title);
    j["source_price"] = Json(t.source_price);
    j["source_id"] = Json(t.source_id);
    j["derived_url"] = Json(t.derived_url);
    j["derived_query"] = Json(t.derived_query);
    j["notify_existing"] = Json(t.notify_existing);
    j["baseline_done"] = Json(t.baseline_done);
    j["created_at"] = Json(t.created_at);
    j["last_check"] = Json(t.last_check);
    j["found_total"] = Json(t.found_total);
    return j;
}

Task task_from_json(const Json& j) {
    Task t;
    t.id = static_cast<int>(j["id"].as_int(0));
    t.name = j["name"].as_string();
    t.kind = kind_from_string(j["kind"].as_string("search"));
    t.enabled = j["enabled"].as_bool(true);
    t.interval = static_cast<int>(j["interval"].as_int(300));
    t.url = j["url"].as_string();
    t.query = j["query"].as_string();
    t.region = j["region"].as_string("rossiya");
    t.price_min = j["price_min"].as_int(0);
    t.price_max = j["price_max"].as_int(0);
    t.exclude = j["exclude"].as_string();
    t.manual_url = j["manual_url"].as_bool(false);
    t.similarity = static_cast<int>(j["similarity"].as_int(45));
    t.price_tolerance = static_cast<int>(j["price_tolerance"].as_int(40));
    t.source_title = j["source_title"].as_string();
    t.source_price = j["source_price"].as_int(0);
    t.source_id = j["source_id"].as_string();
    t.derived_url = j["derived_url"].as_string();
    t.derived_query = j["derived_query"].as_string();
    t.notify_existing = j["notify_existing"].as_bool(false);
    t.baseline_done = j["baseline_done"].as_bool(false);
    t.created_at = j["created_at"].as_double(now_seconds());
    t.last_check = j["last_check"].as_double(0.0);
    t.found_total = static_cast<int>(j["found_total"].as_int(0));
    t.status = TaskStatus::Idle;  // runtime state is never restored from disk
    return t;
}

Json listing_to_json(const Listing& l) {
    Json j = Json::object();
    j["item_id"] = Json(l.item_id);
    j["task_id"] = Json(l.task_id);
    j["title"] = Json(l.title);
    j["url"] = Json(l.url);
    j["price"] = Json(l.price);
    j["price_text"] = Json(l.price_text);
    j["location"] = Json(l.location);
    j["date_text"] = Json(l.date_text);
    j["seller"] = Json(l.seller);
    j["description"] = Json(l.description);
    j["image_url"] = Json(l.image_url);
    j["score"] = Json(l.score);
    j["first_seen"] = Json(l.first_seen);
    return j;
}

Listing listing_from_json(const Json& j) {
    Listing l;
    l.item_id = j["item_id"].as_string();
    l.task_id = static_cast<int>(j["task_id"].as_int(0));
    l.title = j["title"].as_string();
    l.url = j["url"].as_string();
    l.price = j["price"].as_int(0);
    l.price_text = j["price_text"].as_string();
    l.location = j["location"].as_string();
    l.date_text = j["date_text"].as_string();
    l.seller = j["seller"].as_string();
    l.description = j["description"].as_string();
    l.image_url = j["image_url"].as_string();
    l.score = static_cast<int>(j["score"].as_int(100));
    l.first_seen = j["first_seen"].as_double(0.0);
    return l;
}

}  // namespace

Store::Store() {
    load_tasks();
    load_seen();
    load_listings();
}

std::string Store::seen_key(int task_id, const std::string& item_id) {
    return std::to_string(task_id) + ":" + item_id;
}

void Store::load_tasks() {
    std::string text;
    if (!util::read_file(util::tasks_path(), text)) return;
    Json root = Json::parse(text);
    if (!root.is_array()) return;
    for (size_t i = 0; i < root.size(); ++i) {
        Task t = task_from_json(root.at(i));
        if (t.id <= 0) continue;
        next_task_id_ = std::max(next_task_id_, t.id + 1);
        tasks_.push_back(std::move(t));
    }
}

void Store::load_seen() {
    std::string text;
    if (!util::read_file(util::seen_path(), text)) return;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = util::trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty()) continue;

        size_t first = line.find('\t');
        if (first == std::string::npos) continue;
        size_t second = line.find('\t', first + 1);
        std::string task_part = line.substr(0, first);
        std::string item_part = second == std::string::npos
                                    ? line.substr(first + 1)
                                    : line.substr(first + 1, second - first - 1);
        double stamp = second == std::string::npos
                           ? 0.0
                           : std::strtod(line.c_str() + second + 1, nullptr);
        if (item_part.empty()) continue;

        std::string key = task_part + ":" + item_part;
        if (seen_.insert(key).second) seen_log_.emplace_back(key, stamp);
    }
}

void Store::load_listings() {
    std::string text;
    if (!util::read_file(util::listings_path(), text)) return;
    Json root = Json::parse(text);
    if (!root.is_array()) return;
    for (size_t i = 0; i < root.size(); ++i) {
        Listing l = listing_from_json(root.at(i));
        if (!l.item_id.empty()) listings_.push_back(std::move(l));
    }
    std::stable_sort(listings_.begin(), listings_.end(),
                     [](const Listing& a, const Listing& b) { return a.first_seen > b.first_seen; });
}

bool Store::save_tasks_locked() const {
    Json root = Json::array();
    for (const Task& t : tasks_) root.push_back(task_to_json(t));
    return util::write_file_atomic(util::tasks_path(), root.dump(2));
}

bool Store::save_listings_locked() const {
    Json root = Json::array();
    for (const Listing& l : listings_) root.push_back(listing_to_json(l));
    return util::write_file_atomic(util::listings_path(), root.dump(2));
}

bool Store::rewrite_seen_locked() const {
    std::string text;
    text.reserve(seen_log_.size() * 32);
    for (const auto& [key, stamp] : seen_log_) {
        size_t colon = key.find(':');
        if (colon == std::string::npos) continue;
        text.append(key, 0, colon);
        text.push_back('\t');
        text.append(key, colon + 1, std::string::npos);
        text.push_back('\t');
        text.append(std::to_string(static_cast<long long>(stamp)));
        text.push_back('\n');
    }
    return util::write_file_atomic(util::seen_path(), text);
}

std::vector<Task> Store::tasks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_;
}

bool Store::task(int id, Task& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Task& t : tasks_) {
        if (t.id == id) { out = t; return true; }
    }
    return false;
}

Task Store::add_task(Task value) {
    std::lock_guard<std::mutex> lock(mutex_);
    value.id = next_task_id_++;
    if (value.created_at <= 0.0) value.created_at = now_seconds();
    tasks_.push_back(value);
    save_tasks_locked();
    return value;
}

void Store::update_task(const Task& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Task& t : tasks_) {
        if (t.id != value.id) continue;
        t = value;
        save_tasks_locked();
        return;
    }
}

void Store::delete_task(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(),
                                [id](const Task& t) { return t.id == id; }),
                 tasks_.end());
    listings_.erase(std::remove_if(listings_.begin(), listings_.end(),
                                   [id](const Listing& l) { return l.task_id == id; }),
                    listings_.end());

    const std::string prefix = std::to_string(id) + ":";
    for (auto it = seen_log_.begin(); it != seen_log_.end();) {
        if (util::starts_with(it->first, prefix)) {
            seen_.erase(it->first);
            it = seen_log_.erase(it);
        } else {
            ++it;
        }
    }
    save_tasks_locked();
    save_listings_locked();
    rewrite_seen_locked();
}

void Store::set_task_status(int id, TaskStatus status, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Task& t : tasks_) {
        if (t.id != id) continue;
        t.status = status;
        t.last_error = error;
        return;  // runtime state only, nothing to persist
    }
}

std::vector<std::string> Store::filter_new(int task_id,
                                           const std::vector<std::string>& ids) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> fresh;
    for (const std::string& id : ids) {
        if (id.empty()) continue;
        if (seen_.find(seen_key(task_id, id)) == seen_.end()) fresh.push_back(id);
    }
    return fresh;
}

void Store::mark_seen(int task_id, const std::vector<std::string>& ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    const double stamp = now_seconds();
    std::string appended;
    for (const std::string& id : ids) {
        if (id.empty()) continue;
        std::string key = seen_key(task_id, id);
        if (!seen_.insert(key).second) continue;
        seen_log_.emplace_back(key, stamp);
        appended += std::to_string(task_id);
        appended += '\t';
        appended += id;
        appended += '\t';
        appended += std::to_string(static_cast<long long>(stamp));
        appended += '\n';
    }
    if (appended.empty()) return;

    // Appending keeps this cheap even when the history grows large.
    HANDLE file = CreateFileW(util::seen_path().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        rewrite_seen_locked();
        return;
    }
    DWORD written = 0;
    WriteFile(file, appended.data(), static_cast<DWORD>(appended.size()), &written, nullptr);
    CloseHandle(file);
}

size_t Store::seen_count(int task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string prefix = std::to_string(task_id) + ":";
    size_t count = 0;
    for (const auto& [key, stamp] : seen_log_) {
        (void)stamp;
        if (util::starts_with(key, prefix)) ++count;
    }
    return count;
}

void Store::add_listings(const std::vector<Listing>& items) {
    if (items.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    listings_.insert(listings_.begin(), items.begin(), items.end());
    std::stable_sort(listings_.begin(), listings_.end(),
                     [](const Listing& a, const Listing& b) { return a.first_seen > b.first_seen; });
    save_listings_locked();
}

std::vector<Listing> Store::recent_listings(size_t limit, int task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<int, std::string> names;
    for (const Task& t : tasks_) names[t.id] = t.name;

    std::vector<Listing> result;
    for (const Listing& l : listings_) {
        if (task_id != 0 && l.task_id != task_id) continue;
        result.push_back(l);
        auto it = names.find(l.task_id);
        if (it != names.end()) result.back().task_name = it->second;
        if (result.size() >= limit) break;
    }
    return result;
}

void Store::clear_listings(int task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_id == 0) {
        listings_.clear();
    } else {
        listings_.erase(std::remove_if(listings_.begin(), listings_.end(),
                                       [task_id](const Listing& l) { return l.task_id == task_id; }),
                        listings_.end());
    }
    save_listings_locked();
}

void Store::prune(int keep_days, size_t max_items) {
    std::lock_guard<std::mutex> lock(mutex_);
    const double cutoff = now_seconds() - static_cast<double>(keep_days) * 86400.0;

    size_t before = seen_log_.size();
    std::vector<std::pair<std::string, double>> kept;
    kept.reserve(seen_log_.size());
    for (auto& entry : seen_log_) {
        if (entry.second >= cutoff || entry.second == 0.0) {
            kept.push_back(entry);
        } else {
            seen_.erase(entry.first);
        }
    }
    if (kept.size() != before) {
        seen_log_.swap(kept);
        rewrite_seen_locked();
        util::log_info("store", "pruned " + std::to_string(before - seen_log_.size()) +
                                    " seen entries");
    }

    if (listings_.size() > max_items) {
        listings_.resize(max_items);
        save_listings_locked();
    }
}

Store& store() {
    static Store instance;
    return instance;
}

}  // namespace core
