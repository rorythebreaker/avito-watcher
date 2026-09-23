// Domain types shared by the engine, the store and the UI.
#pragma once

#include <string>
#include <vector>

namespace core {

enum class TaskKind { Search, Similar };

enum class TaskStatus { Idle, Running, Ok, Blocked, Error };

const char* kind_title(TaskKind kind);
const char* status_title(TaskStatus status);
std::string kind_to_string(TaskKind kind);
TaskKind kind_from_string(const std::string& text);

struct Task {
    int id = 0;
    std::string name;
    TaskKind kind = TaskKind::Search;
    bool enabled = true;
    int interval = 300;          // seconds between checks
    std::string url;             // search page, or the sample listing for Similar

    // Search parameters
    std::string query;
    std::string region = "rossiya";
    long long price_min = 0;     // 0 means "no limit"
    long long price_max = 0;
    std::string exclude;         // comma separated stop words
    bool manual_url = false;     // the user pasted a ready Avito link

    // Similar-listing parameters
    int similarity = 45;         // percent threshold
    int price_tolerance = 40;    // percent band around the sample price
    std::string source_title;
    long long source_price = 0;
    std::string source_id;
    std::string derived_url;     // search link computed from the sample
    std::string derived_query;

    bool notify_existing = false;  // announce the very first page as well
    bool baseline_done = false;

    // Runtime state
    double created_at = 0.0;
    double last_check = 0.0;
    TaskStatus status = TaskStatus::Idle;
    std::string last_error;
    int found_total = 0;

    // The link the engine actually polls.
    const std::string& search_url() const {
        return kind == TaskKind::Similar ? derived_url : url;
    }

    std::vector<std::string> exclude_words() const;
};

struct Listing {
    std::string item_id;
    int task_id = 0;
    std::string title;
    std::string url;
    long long price = 0;         // 0 means "price not stated"
    std::string price_text;
    std::string location;
    std::string date_text;
    std::string seller;
    std::string description;
    std::string image_url;
    int score = 100;             // similarity percent for Similar tasks
    double first_seen = 0.0;
    std::string task_name;

    std::string price_label() const;
};

// Seconds since the Unix epoch. Stored as a double so timestamps survive a
// round trip through JSON without a separate integer type.
double now_seconds();

}  // namespace core
