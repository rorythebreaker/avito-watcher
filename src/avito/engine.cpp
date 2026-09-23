#include "avito/engine.h"

#include <algorithm>

#include "avito/similarity.h"
#include "core/config.h"
#include "core/store.h"
#include "notify/notify.h"
#include "util/log.h"
#include "util/strings.h"

namespace avito {
namespace {

// Query widths tried when setting up a "similar listings" task, from precise to
// broad.
constexpr int kQueryWidths[] = {4, 3, 2};
constexpr size_t kEnoughNeighbours = 3;

std::string normalize_search_url(const std::string& url) {
    std::string text = util::trim(url);
    if (text.empty()) return {};
    if (!util::starts_with(text, "http")) {
        while (!text.empty() && text.front() == '/') text.erase(0, 1);
        text = "https://" + text;
    }

    size_t question = text.find('?');
    std::string base = question == std::string::npos ? text : text.substr(0, question);
    std::string query = question == std::string::npos ? "" : text.substr(question + 1);

    std::vector<std::string> kept;
    bool has_sort = false;
    for (const std::string& part : util::split(query, '&')) {
        if (part.empty()) continue;
        std::string name = part.substr(0, part.find('='));
        if (name == "p") continue;          // always the first page
        if (name == "s") has_sort = true;
        kept.push_back(part);
    }
    if (!has_sort) kept.push_back("s=104");  // newest first

    std::string result = base;
    if (!kept.empty()) result += "?" + util::join(kept, "&");
    return result;
}

}  // namespace

Engine::Engine(EngineListener* listener) : listener_(listener) {
    fetcher_.apply_settings(core::settings().get());
}

Engine::~Engine() {
    stop();
}

void Engine::start() {
    if (running_.load()) return;
    stop_.store(false);
    fetcher_.apply_settings(core::settings().get());
    running_.store(true);
    thread_ = std::thread(&Engine::loop, this);
    if (listener_) {
        listener_->on_running_changed(true);
        listener_->on_message("Слежение запущено");
    }
}

void Engine::stop() {
    if (!running_.load()) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    stop_.store(true);
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
    if (listener_) {
        listener_->on_running_changed(false);
        listener_->on_message("Слежение остановлено");
    }
}

void Engine::check_now(int task_id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        manual_.push_back(task_id);
    }
    wake_.notify_all();
}

void Engine::schedule_soon(int task_id, double delay_seconds) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        next_run_[task_id] = core::now_seconds() + delay_seconds;
    }
    wake_.notify_all();
}

void Engine::forget_task(int task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    next_run_.erase(task_id);
}

void Engine::settings_changed() {
    fetcher_.apply_settings(core::settings().get());
    wake_.notify_all();
}

void Engine::loop() {
    while (!stop_.load()) {
        try {
            tick();
        } catch (const std::exception& error) {
            util::log_error("engine", std::string("Сбой в цикле слежения: ") + error.what());
        } catch (...) {
            util::log_error("engine", "Неизвестный сбой в цикле слежения");
        }

        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, std::chrono::seconds(2), [this] {
            return stop_.load() || !manual_.empty();
        });
    }
}

void Engine::tick() {
    // Manual checks jump the queue.
    while (true) {
        int task_id = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (manual_.empty()) break;
            task_id = manual_.front();
            manual_.pop_front();
        }
        core::Task task;
        if (core::store().task(task_id, task)) run_task(task);
        if (stop_.load()) return;
    }

    const double now = core::now_seconds();
    for (const core::Task& task : core::store().tasks()) {
        if (stop_.load()) return;
        if (!task.enabled) continue;

        double due;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = next_run_.find(task.id);
            due = it != next_run_.end()
                      ? it->second
                      : task.last_check + std::max(kMinInterval, task.interval);
        }
        if (now >= due) run_task(task);
    }

    if (now - last_prune_ > 3600.0) {
        last_prune_ = now;
        core::Settings settings = core::settings().get();
        core::store().prune(settings.keep_history_days,
                            static_cast<size_t>(settings.max_feed_items) * 4);
    }
}

void Engine::run_task(core::Task task) {
    core::store().set_task_status(task.id, core::TaskStatus::Running, {});
    if (listener_) listener_->on_task_started(task.id);

    std::string error;
    core::TaskStatus status = core::TaskStatus::Ok;
    check_task(task, error, status);

    task.last_check = core::now_seconds();
    task.status = status;
    task.last_error = error;
    core::store().update_task(task);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        next_run_[task.id] = task.last_check + std::max(kMinInterval, task.interval);
    }

    if (listener_) {
        listener_->on_task_finished(task.id, status, error);
        if (!error.empty()) listener_->on_message("«" + task.name + "»: " + error);
    }
}

bool Engine::check_task(core::Task& task, std::string& error, core::TaskStatus& status) {
    if (task.kind == core::TaskKind::Similar && task.derived_url.empty()) {
        if (!ensure_similar_ready(task, error)) {
            status = core::TaskStatus::Error;
            return false;
        }
    }

    const std::string url = normalize_search_url(task.search_url());
    if (url.empty()) {
        error = "У задачи не задана ссылка для поиска";
        status = core::TaskStatus::Error;
        return false;
    }

    net::FetchResult page = fetcher_.get(url, kSearchMarker, &stop_);
    if (!page.ok()) {
        error = page.error;
        status = page.status == net::FetchStatus::Blocked ? core::TaskStatus::Blocked
                                                          : core::TaskStatus::Error;
        return false;
    }
    util::log_info("engine", "Задача " + task.name + ": страница получена (" +
                                 page.transport + ")");

    std::vector<core::Listing> found = parse_search(page.html);
    if (found.empty()) {
        if (net::is_empty_result(page.html)) {
            // Nothing matches the query yet; that is a normal state.
            task.baseline_done = true;
            status = core::TaskStatus::Ok;
            return true;
        }
        error = "Авито вернул страницу без объявлений. Проверьте ссылку или попробуйте позже.";
        status = core::TaskStatus::Error;
        return false;
    }

    std::vector<core::Listing> candidates;
    std::vector<std::string> all_ids;
    all_ids.reserve(found.size());
    for (core::Listing& listing : found) {
        all_ids.push_back(listing.item_id);
        core::Listing copy = listing;
        if (passes_filters(task, copy)) candidates.push_back(std::move(copy));
    }

    // The first pass memorises what is already on Avito. Without it the user
    // would be flooded with notices about listings posted long ago.
    if (!task.baseline_done && !task.notify_existing) {
        core::store().mark_seen(task.id, all_ids);
        task.baseline_done = true;
        status = core::TaskStatus::Ok;
        if (listener_) {
            listener_->on_message("«" + task.name + "»: запомнил " +
                                  std::to_string(candidates.size()) +
                                  " текущих объявлений, жду новые");
        }
        return true;
    }
    task.baseline_done = true;

    std::vector<std::string> candidate_ids;
    candidate_ids.reserve(candidates.size());
    for (const core::Listing& listing : candidates) candidate_ids.push_back(listing.item_id);

    std::vector<std::string> new_ids = core::store().filter_new(task.id, candidate_ids);
    core::store().mark_seen(task.id, all_ids);

    if (new_ids.empty()) {
        status = core::TaskStatus::Ok;
        return true;
    }

    std::vector<core::Listing> fresh;
    const double stamp = core::now_seconds();
    for (core::Listing& listing : candidates) {
        if (std::find(new_ids.begin(), new_ids.end(), listing.item_id) == new_ids.end()) continue;
        listing.task_id = task.id;
        listing.task_name = task.name;
        listing.first_seen = stamp;
        fresh.push_back(listing);
    }
    if (fresh.empty()) {
        status = core::TaskStatus::Ok;
        return true;
    }

    task.found_total += static_cast<int>(fresh.size());
    core::store().add_listings(fresh);

    if (listener_) {
        listener_->on_new_listings(task.id, fresh);
        listener_->on_message("«" + task.name + "»: новых объявлений — " +
                              std::to_string(fresh.size()));
    }
    util::log_info("engine", "Задача " + task.name + ": новых объявлений " +
                                 std::to_string(fresh.size()));

    std::vector<std::string> errors =
        notify::dispatch(core::settings().get(), fresh, task.name);
    if (!errors.empty() && listener_) listener_->on_notify_errors(errors);

    status = core::TaskStatus::Ok;
    return true;
}

bool Engine::load_item_info(const std::string& url, ItemInfo& out, std::string& error) {
    net::FetchResult page = fetcher_.get(util::trim(url), {}, &stop_);
    if (!page.ok()) {
        error = page.error;
        return false;
    }
    if (is_removed_item(page.html)) {
        error =
            "Это объявление снято с публикации, взять его за образец нельзя. "
            "Выберите другое, похожее объявление.";
        return false;
    }
    out = parse_item(page.html, url);
    if (out.title.empty()) {
        error =
            "Не удалось прочитать объявление. Проверьте ссылку — нужна ссылка "
            "на страницу конкретного объявления.";
        return false;
    }
    return true;
}

bool Engine::ensure_similar_ready(core::Task& task, std::string& error) {
    ItemInfo info;
    if (!load_item_info(task.url, info, error)) return false;

    task.source_title = info.title;
    task.source_price = info.price;
    task.source_id = info.item_id;
    task.derived_url = pick_search_url(task, info);

    core::store().update_task(task);
    if (listener_) {
        listener_->on_message("«" + task.name + "»: образец — " + info.title +
                              " (запрос «" + task.derived_query + "»)");
    }
    return true;
}

std::string Engine::pick_search_url(core::Task& task, const ItemInfo& info) {
    // A query made of the whole title usually finds exactly one listing - the
    // sample. So, once, when the task is created, try shorter queries until
    // neighbours show up.
    std::string fallback_url;
    std::string fallback_query;

    for (int width : kQueryWidths) {
        std::string query = build_query(info.title, width);
        if (query.empty()) continue;

        std::string url = build_search_url(info, task.price_tolerance, query);
        if (fallback_url.empty()) {
            fallback_url = url;
            fallback_query = query;
        }

        net::FetchResult page = fetcher_.get(url, kSearchMarker, &stop_);
        if (!page.ok()) {
            util::log_info("engine", "Проба запроса «" + query + "» не удалась: " + page.error);
            continue;
        }

        size_t neighbours = 0;
        for (const core::Listing& listing : parse_search(page.html)) {
            if (listing.item_id != info.item_id) ++neighbours;
        }
        util::log_info("engine", "Запрос «" + query + "»: соседей " +
                                     std::to_string(neighbours));

        if (neighbours >= kEnoughNeighbours) {
            task.derived_query = query;
            return url;
        }
        fallback_url = url;
        fallback_query = query;
        if (stop_.load()) break;
    }

    task.derived_query = fallback_query;
    return fallback_url;
}

bool Engine::passes_filters(const core::Task& task, core::Listing& listing) const {
    const std::string title = util::to_lower(listing.title);
    for (const std::string& word : task.exclude_words()) {
        if (title.find(word) != std::string::npos) return false;
    }

    if (task.kind == core::TaskKind::Search) {
        if (task.price_min > 0 && listing.price > 0 && listing.price < task.price_min) return false;
        if (task.price_max > 0 && listing.price > 0 && listing.price > task.price_max) return false;
        return true;
    }

    // "Similar": never report the sample listing back to the user.
    if (!listing.item_id.empty() && listing.item_id == task.source_id) return false;

    listing.score = score(task.source_title, task.source_price, listing.title,
                          listing.price, task.price_tolerance);
    return listing.score >= task.similarity;
}

}  // namespace avito
