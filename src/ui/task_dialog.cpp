#include "ui/task_dialog.h"

#include <commctrl.h>

#include <memory>
#include <thread>

#include "avito/similarity.h"
#include "ui/theme.h"
#include "util/strings.h"

namespace ui {
namespace {

enum Ids {
    kRadioSearch = 1000,
    kRadioSimilar,
    kQueryEdit,
    kRegionCombo,
    kPriceMin,
    kPriceMax,
    kUrlEdit,
    kItemUrlEdit,
    kProbeButton,
    kSimilaritySlider,
    kToleranceSlider,
    kNameEdit,
    kIntervalCombo,
    kExcludeEdit,
    kNotifyExisting,
    kEnabledCheck,
};

constexpr UINT WM_PROBE_DONE = WM_APP + 11;

struct Region {
    const wchar_t* title;
    const char* slug;
};

// The cities people ask for most; anything else is covered by pasting a link.
const Region kRegions[] = {
    {L"Вся Россия", "rossiya"},
    {L"Москва", "moskva"},
    {L"Санкт-Петербург", "sankt-peterburg"},
    {L"Новосибирск", "novosibirsk"},
    {L"Екатеринбург", "ekaterinburg"},
    {L"Казань", "kazan"},
    {L"Нижний Новгород", "nizhniy_novgorod"},
    {L"Челябинск", "chelyabinsk"},
    {L"Самара", "samara"},
    {L"Омск", "omsk"},
    {L"Ростов-на-Дону", "rostov-na-donu"},
    {L"Уфа", "ufa"},
    {L"Красноярск", "krasnoyarsk"},
    {L"Воронеж", "voronezh"},
    {L"Пермь", "perm"},
    {L"Волгоград", "volgograd"},
    {L"Краснодар", "krasnodar"},
    {L"Саратов", "saratov"},
    {L"Тюмень", "tyumen"},
    {L"Сочи", "sochi"},
    {L"Владивосток", "vladivostok"},
    {L"Калининград", "kaliningrad"},
};

struct Interval {
    const wchar_t* title;
    int seconds;
};

const Interval kIntervals[] = {
    {L"1 минута", 60},   {L"2 минуты", 120},  {L"5 минут", 300},
    {L"10 минут", 600},  {L"15 минут", 900},  {L"30 минут", 1800},
    {L"1 час", 3600},    {L"3 часа", 10800},  {L"6 часов", 21600},
};

// Carries the probe outcome from the worker thread back to the dialog.
struct ProbePayload {
    avito::ItemInfo info;
    std::string error;
    bool ok = false;
};

bool looks_like_item_url(const std::string& url) {
    return !avito::item_id_from_url(url).empty();
}

}  // namespace

TaskDialog::TaskDialog(avito::Engine* engine, const core::Task* task, int default_interval)
    : engine_(engine), editing_(task != nullptr) {
    if (task) {
        task_ = *task;
    } else {
        task_.interval = default_interval;
        task_.created_at = core::now_seconds();
    }
    if (!task_.source_title.empty()) info_ready_ = true;
}

TaskDialog::~TaskDialog() = default;

void TaskDialog::build() {
    const int left = 16;
    const int label_width = 130;
    const int field_left = left + label_width + 8;
    const int field_width = 440;

    // --- what to watch ---
    label(L"Что отслеживаем", left, 12, 200, 18);
    radio_search_ = radio(L"Объявления по запросу", left, 34, 220, 22, kRadioSearch,
                          task_.kind == core::TaskKind::Search, true);
    radio_similar_ = radio(L"Похожие на конкретное объявление", left + 240, 34, 300, 22,
                           kRadioSimilar, task_.kind == core::TaskKind::Similar, false);

    // --- search page ---
    search_hint_ = label(
        L"Задайте запрос и город — или настройте фильтры прямо на Авито и вставьте "
        L"сюда ссылку из адресной строки: так доступны все фильтры сайта.",
        left, 68, 570, 34);
    mark_hint(search_hint_);

    query_label_ = label(L"Запрос:", left, 112, label_width, 20);
    query_edit_ = edit(util::widen(task_.query), field_left, 108, field_width, 24, kQueryEdit);

    region_label_ = label(L"Город:", left, 144, label_width, 20);
    std::vector<std::wstring> region_titles;
    int region_index = 0;
    for (size_t i = 0; i < std::size(kRegions); ++i) {
        region_titles.emplace_back(kRegions[i].title);
        if (task_.region == kRegions[i].slug) region_index = static_cast<int>(i);
    }
    region_combo_ = combo(field_left, 140, 240, 26, kRegionCombo, region_titles, region_index);

    price_label_ = label(L"Цена, ₽:", left, 176, label_width, 20);
    price_min_ = number(static_cast<int>(task_.price_min), field_left, 172, 110, 24, kPriceMin);
    price_to_ = label(L"до", field_left + 118, 176, 20, 20);
    price_max_ = number(static_cast<int>(task_.price_max), field_left + 142, 172, 110, 24,
                        kPriceMax);

    url_label_ = label(L"Готовая ссылка:", left, 208, label_width, 20);
    url_edit_ = edit(task_.manual_url ? util::widen(task_.url) : L"", field_left, 204,
                     field_width, 24, kUrlEdit);

    search_preview_ = label(L"", left, 236, 570, 32);
    mark_hint(search_preview_);

    // --- similar page ---
    similar_hint_ = label(
        L"Вставьте ссылку на объявление-образец. Приложение разберёт его и будет искать "
        L"новые объявления того же рода — в той же категории и городе, с похожим "
        L"названием и близкой ценой.",
        left, 68, 570, 48);
    mark_hint(similar_hint_);

    item_url_edit_ = edit(util::widen(task_.kind == core::TaskKind::Similar ? task_.url : ""),
                          left, 122, 460, 24, kItemUrlEdit);
    probe_button_ = button(L"Проверить", left + 470, 121, 100, 26, kProbeButton);

    item_preview_ = label(L"Объявление ещё не проверено.", left, 156, 570, 56);
    mark_hint(item_preview_);

    similarity_label_ = label(L"Порог похожести:", left, 226, label_width, 20);
    similarity_slider_ = slider(field_left, 224, 330, 26, kSimilaritySlider, 10, 95,
                                task_.similarity);
    similarity_value_ = label(L"", field_left + 340, 226, 60, 20);

    tolerance_label_ = label(L"Разброс цены:", left, 258, label_width, 20);
    tolerance_slider_ = slider(field_left, 256, 330, 26, kToleranceSlider, 0, 100,
                               task_.price_tolerance);
    tolerance_value_ = label(L"", field_left + 340, 258, 60, 20);

    similar_note_ = label(
        L"Чем выше порог, тем строже отбор: 30–40% ловит широкий круг похожих товаров, "
        L"60% и выше — почти те же модели.",
        left, 288, 570, 32);
    mark_hint(similar_note_);

    // --- common ---
    label(L"Общее", left, 336, 200, 18);

    label(L"Название:", left, 366, label_width, 20);
    name_edit_ = edit(util::widen(task_.name), field_left, 362, field_width, 24, kNameEdit);

    label(L"Проверять раз в:", left, 398, label_width, 20);
    std::vector<std::wstring> interval_titles;
    int interval_index = 2;
    for (size_t i = 0; i < std::size(kIntervals); ++i) {
        interval_titles.emplace_back(kIntervals[i].title);
        if (task_.interval == kIntervals[i].seconds) interval_index = static_cast<int>(i);
    }
    interval_combo_ = combo(field_left, 394, 240, 26, kIntervalCombo, interval_titles,
                            interval_index);

    label(L"Исключать слова:", left, 430, label_width, 20);
    exclude_edit_ = edit(util::widen(task_.exclude), field_left, 426, field_width, 24,
                         kExcludeEdit);

    notify_existing_ = check(L"Уведомить обо всех объявлениях уже при первой проверке",
                             field_left, 456, field_width, 22, kNotifyExisting,
                             task_.notify_existing);
    enabled_check_ = check(L"Задача включена", field_left, 482, field_width, 22, kEnabledCheck,
                           task_.enabled);

    button(L"Сохранить", 380, 520, 110, 30, IDOK, true);
    button(L"Отмена", 500, 520, 100, 30, IDCANCEL);

    switch_page(task_.kind == core::TaskKind::Similar);
    update_sliders();
    update_search_preview();
    if (info_ready_) {
        show_sample(task_.source_title, task_.source_price, task_.derived_url);
    }
}

void TaskDialog::switch_page(bool similar) {
    for (HWND control : {search_hint_, query_label_, query_edit_, region_label_, region_combo_,
                         price_label_, price_min_, price_to_, price_max_, url_label_,
                         url_edit_, search_preview_}) {
        show(control, !similar);
    }
    for (HWND control : {similar_hint_, item_url_edit_, probe_button_, item_preview_,
                         similarity_label_, similarity_slider_, similarity_value_,
                         tolerance_label_, tolerance_slider_, tolerance_value_,
                         similar_note_}) {
        show(control, similar);
    }
}

void TaskDialog::update_sliders() {
    set_text(similarity_value_, std::to_wstring(slider_value(similarity_slider_)) + L"%");
    int tolerance = slider_value(tolerance_slider_);
    set_text(tolerance_value_, tolerance > 0 ? L"±" + std::to_wstring(tolerance) + L"%"
                                             : std::wstring(L"любая"));
}

std::string TaskDialog::built_search_url() const {
    std::string manual = util::trim(util::narrow(text_of(url_edit_)));
    if (!manual.empty()) {
        if (!util::starts_with(manual, "http")) manual = "https://" + manual;
        return manual;
    }

    std::string query = util::trim(util::narrow(text_of(query_edit_)));
    if (query.empty()) return {};

    int region_index = static_cast<int>(SendMessageW(region_combo_, CB_GETCURSEL, 0, 0));
    if (region_index < 0 || region_index >= static_cast<int>(std::size(kRegions))) {
        region_index = 0;
    }

    std::string params = "q=" + util::url_encode(query);
    long long low = 0;
    long long high = 0;
    util::parse_number(util::narrow(text_of(price_min_)), low);
    util::parse_number(util::narrow(text_of(price_max_)), high);
    if (low > 0) params += "&pmin=" + std::to_string(low);
    if (high > 0) params += "&pmax=" + std::to_string(high);
    params += "&s=104";

    return std::string("https://www.avito.ru/") + kRegions[region_index].slug + "?" + params;
}

void TaskDialog::update_search_preview() {
    std::string url = built_search_url();
    set_text(search_preview_,
             url.empty() ? L"" : L"Ссылка для проверки: " + util::widen(url));
}

void TaskDialog::show_sample(const std::string& title, long long price,
                             const std::string& url) {
    std::wstring text = util::widen(title);
    text += L"\nЦена: ";
    text += price > 0 ? util::widen(util::format_thousands(price)) + L" ₽" : L"не указана";
    std::string query = avito::build_query(title);
    text += L"\nПоисковый запрос: " + util::widen(query.empty() ? "—" : query);
    if (!url.empty()) text += L"\n" + util::widen(url);
    set_text(item_preview_, text);
}

void TaskDialog::start_probe() {
    std::string url = util::trim(util::narrow(text_of(item_url_edit_)));
    if (!looks_like_item_url(url)) {
        show_warning(window(), L"Проверьте ссылку",
                     L"Нужна ссылка на страницу конкретного объявления — она "
                     L"заканчивается числовым номером.");
        return;
    }
    if (probing_) return;

    probing_ = true;
    enable(probe_button_, false);
    set_text(probe_button_, L"Загружаю…");
    set_text(item_preview_, L"Загружаю объявление с Авито…");

    HWND target = window();
    avito::Engine* engine = engine_;
    std::thread([target, engine, url] {
        auto* payload = new ProbePayload();
        payload->ok = engine->load_item_info(url, payload->info, payload->error);
        PostMessageW(target, WM_PROBE_DONE, 0, reinterpret_cast<LPARAM>(payload));
    }).detach();
}

void TaskDialog::finish_probe(bool ok) {
    probing_ = false;
    enable(probe_button_, true);
    set_text(probe_button_, L"Проверить");
    if (ok && util::trim(util::narrow(text_of(name_edit_))).empty()) {
        std::string name = "Похожие: " + info_.title;
        if (name.size() > 60) name.resize(60);
        set_text(name_edit_, util::widen(name));
    }
}

void TaskDialog::on_command(int control_id, int notification) {
    switch (control_id) {
        case kRadioSearch:
            switch_page(false);
            break;
        case kRadioSimilar:
            switch_page(true);
            break;
        case kProbeButton:
            start_probe();
            break;
        case kQueryEdit:
        case kUrlEdit:
        case kPriceMin:
        case kPriceMax:
            if (notification == EN_CHANGE) update_search_preview();
            break;
        case kRegionCombo:
            if (notification == CBN_SELCHANGE) update_search_preview();
            break;
        default:
            break;
    }
}

void TaskDialog::on_scroll(HWND control) {
    if (control == similarity_slider_ || control == tolerance_slider_) update_sliders();
}

bool TaskDialog::on_accept() {
    const bool similar = checked(radio_similar_);

    if (similar) {
        std::string url = util::trim(util::narrow(text_of(item_url_edit_)));
        if (!looks_like_item_url(url)) {
            show_warning(window(), L"Нужна ссылка на объявление",
                         L"Вставьте ссылку на конкретное объявление Авито.");
            return false;
        }

        const bool changed = task_.kind != core::TaskKind::Similar || task_.url != url;
        task_.kind = core::TaskKind::Similar;
        task_.url = url;
        task_.similarity = slider_value(similarity_slider_);
        task_.price_tolerance = slider_value(tolerance_slider_);

        if (info_ready_ && info_.item_id == avito::item_id_from_url(url)) {
            task_.source_title = info_.title;
            task_.source_price = info_.price;
            task_.source_id = info_.item_id;
            task_.derived_url = avito::build_search_url(info_, task_.price_tolerance);
            task_.derived_query = avito::build_query(info_.title);
        } else if (changed) {
            // The engine works the search link out on the first check.
            task_.derived_url.clear();
            task_.derived_query.clear();
            task_.baseline_done = false;
        }
    } else {
        std::string url = built_search_url();
        if (url.empty()) {
            show_warning(window(), L"Нечего отслеживать",
                         L"Введите поисковый запрос или вставьте готовую ссылку с Авито.");
            return false;
        }
        task_.kind = core::TaskKind::Search;
        task_.url = url;
        task_.manual_url = !util::trim(util::narrow(text_of(url_edit_))).empty();
        task_.query = util::trim(util::narrow(text_of(query_edit_)));

        int region_index = static_cast<int>(SendMessageW(region_combo_, CB_GETCURSEL, 0, 0));
        if (region_index >= 0 && region_index < static_cast<int>(std::size(kRegions))) {
            task_.region = kRegions[region_index].slug;
        }
        task_.price_min = 0;
        task_.price_max = 0;
        util::parse_number(util::narrow(text_of(price_min_)), task_.price_min);
        util::parse_number(util::narrow(text_of(price_max_)), task_.price_max);
    }

    task_.exclude = util::trim(util::narrow(text_of(exclude_edit_)));
    task_.notify_existing = checked(notify_existing_);
    task_.enabled = checked(enabled_check_);

    int interval_index = static_cast<int>(SendMessageW(interval_combo_, CB_GETCURSEL, 0, 0));
    if (interval_index >= 0 && interval_index < static_cast<int>(std::size(kIntervals))) {
        task_.interval = kIntervals[interval_index].seconds;
    }

    task_.name = util::trim(util::narrow(text_of(name_edit_)));
    if (task_.name.empty()) {
        if (similar) {
            task_.name = task_.source_title.empty() ? "Похожие объявления" : task_.source_title;
        } else {
            task_.name = task_.query.empty() ? "Поиск на Авито" : task_.query;
        }
        if (task_.name.size() > 60) task_.name.resize(60);
    }
    return true;
}

// The probe finishes on a worker thread; the result comes back as a message so
// every control touch stays on the UI thread.
bool TaskDialog::on_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result) {
    if (message != WM_PROBE_DONE) return false;
    (void)wparam;
    result = 0;

    std::unique_ptr<ProbePayload> payload(reinterpret_cast<ProbePayload*>(lparam));
    if (!payload) return true;

    if (payload->ok) {
        info_ = payload->info;
        info_ready_ = true;
        show_sample(info_.title, info_.price,
                    avito::build_search_url(info_, slider_value(tolerance_slider_)));
    } else {
        info_ready_ = false;
        set_text(item_preview_, util::widen(payload->error));
        mark_error(item_preview_);
    }
    finish_probe(payload->ok);
    return true;
}

}  // namespace ui
