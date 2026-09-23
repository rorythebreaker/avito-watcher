// Console self-test for everything below the user interface.
//
//   AvitoWatcherSelfTest                 run the offline checks
//   AvitoWatcherSelfTest <fixtures dir>  also parse saved Avito pages
//   AvitoWatcherSelfTest --live          also make real requests to Avito
//
// Saved pages are not kept in the repository; point the test at a directory
// holding real.html, http_block.html, live_item.html and avito.html to run the
// parsing checks against genuine markup.
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "avito/parser.h"
#include "avito/similarity.h"
#include "core/config.h"
#include "net/fetcher.h"
#include "util/json.h"
#include "util/paths.h"
#include "util/strings.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void report(bool ok, const std::string& what, const std::string& detail = {}) {
    ++g_checks;
    if (!ok) ++g_failures;
    std::printf("%s %s%s%s\n", ok ? "[ ok ]" : "[FAIL]", what.c_str(),
                detail.empty() ? "" : " -> ", detail.c_str());
}

std::string quoted(const std::string& text, size_t limit = 46) {
    if (text.size() <= limit) return "\"" + text + "\"";
    // Do not cut a UTF-8 character in half.
    size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return "\"" + text.substr(0, end) + "…\"";
}

bool load(const std::string& directory, const char* name, std::string& out) {
    return util::read_file(util::widen(directory + "\\" + name), out);
}

// --- strings and json ----------------------------------------------------
void test_utilities() {
    std::printf("\n--- строки и JSON ---\n");

    report(util::format_thousands(45000) == "45 000", "разделение разрядов",
           util::format_thousands(45000));

    long long value = 0;
    report(util::parse_number("45\xC2\xA0""000 \xE2\x82\xBD", value) && value == 45000,
           "цена с неразрывным пробелом", std::to_string(value));

    report(util::url_encode("велосипед stels") == "%D0%B2%D0%B5%D0%BB%D0%BE%D1%81%D0%B8%D0%BF"
                                                  "%D0%B5%D0%B4+stels",
           "кодирование запроса");

    std::string round_trip = util::narrow(util::widen("Тест UTF-8 ↔ UTF-16"));
    report(round_trip == "Тест UTF-8 ↔ UTF-16", "перекодировка", round_trip);

    util::Json root = util::Json::parse(
        "{\"ok\":true,\"result\":{\"id\":42,\"name\":\"\\u0411\\u043e\\u0442\"},\"list\":[1,2]}");
    report(root["ok"].as_bool(), "разбор JSON: логическое");
    report(root["result"]["id"].as_int() == 42, "разбор JSON: число");
    report(root["result"]["name"].as_string() == "Бот", "разбор JSON: \\u-последовательность",
           root["result"]["name"].as_string());
    report(root["list"].size() == 2, "разбор JSON: массив");

    util::Json built = util::Json::object();
    built["text"] = util::Json("кавычка \" и перенос\n");
    report(util::Json::parse(built.dump())["text"].as_string() == "кавычка \" и перенос\n",
           "JSON туда и обратно");
}

// --- similarity ----------------------------------------------------------
void test_similarity() {
    std::printf("\n--- похожесть ---\n");

    std::string query = avito::build_query("Велосипед гревел Cube Nuroad C62 Carbon");
    report(query == "велосипед гревел cube nuroad", "короткий запрос из заголовка", query);

    report(avito::build_query("iPhone 15, 128 ГБ, SIM + eSIM") == "iphone 15 128 гб",
           "запрос: телефон", avito::build_query("iPhone 15, 128 ГБ, SIM + eSIM"));

    const std::string reference = "Велосипед Stels Navigator 500";
    report(avito::score(reference, 18000, reference, 18000, 40) == 100, "точное совпадение");
    report(avito::score(reference, 18000, "Велосипед Stels Navigator 500 МД", 17000, 40) >= 85,
           "почти то же самое");
    report(avito::score(reference, 18000, "Детская коляска", 18000, 40) == 0, "чужой товар");
    // The price band is a hard condition, matching the pmin/pmax sent to Avito.
    report(avito::score(reference, 18000, reference, 90000, 40) == 0, "цена вне разброса");
    report(avito::price_matches(18000, 25000, 50), "цена внутри разброса");

    avito::ItemInfo info;
    info.title = "Велосипед Stels Navigator 500";
    info.price = 18000;
    info.region = "kazan";
    info.category = "velosipedy";
    std::string url = avito::build_search_url(info, 40);
    report(util::contains(url, "/kazan/velosipedy?") && util::contains(url, "pmin=10800") &&
               util::contains(url, "pmax=25200") && util::contains(url, "s=104"),
           "ссылка поиска похожих", url);
}

// --- url handling --------------------------------------------------------
void test_urls() {
    std::printf("\n--- адреса объявлений ---\n");

    // Avito separates the id with an underscore or a hyphen.
    report(avito::item_id_from_url(
               "https://www.avito.ru/moskva/telefony/iphone_15_128_gb_8377047842") ==
               "8377047842",
           "номер после подчёркивания");
    report(avito::item_id_from_url("https://www.avito.ru/kazan/velosipedy/stels-1122334455") ==
               "1122334455",
           "номер после дефиса");
    report(avito::item_id_from_url(
               "https://www.avito.ru/kazan/velosipedy/stels_500-1122334455?context=x") ==
               "1122334455",
           "номер при хвосте запроса");
    report(avito::item_id_from_url("https://www.avito.ru/kazan/velosipedy").empty(),
           "ссылка на категорию — не объявление");
}

// --- parsing saved pages -------------------------------------------------
void test_search_page(const std::string& directory, const char* file, size_t expected_cards,
                      const std::string& expected_first_title, long long expected_first_price) {
    std::string html;
    if (!load(directory, file, html)) {
        std::printf("[skip] %s не найден\n", file);
        return;
    }

    std::vector<core::Listing> items = avito::parse_search(html);
    report(items.size() == expected_cards,
           std::string(file) + ": число карточек",
           std::to_string(items.size()) + " из " + std::to_string(expected_cards));
    if (items.empty()) return;

    const core::Listing& first = items.front();
    report(first.title == expected_first_title, std::string(file) + ": заголовок",
           quoted(first.title));
    report(first.price == expected_first_price, std::string(file) + ": цена",
           std::to_string(first.price));
    report(!first.item_id.empty() && !first.url.empty(), std::string(file) + ": номер и ссылка",
           first.item_id);

    size_t with_photo = 0;
    size_t with_price = 0;
    for (const core::Listing& item : items) {
        if (!item.image_url.empty()) ++with_photo;
        if (item.price > 0) ++with_price;
    }
    // Cards below the fold have no <img>; the address still lives in the
    // slider's data-marker, so every card must resolve a photo.
    report(with_photo == items.size(), std::string(file) + ": фото у всех карточек",
           std::to_string(with_photo) + "/" + std::to_string(items.size()));
    report(with_price == items.size(), std::string(file) + ": цена у всех карточек",
           std::to_string(with_price) + "/" + std::to_string(items.size()));
}

void test_fixtures(const std::string& directory) {
    std::printf("\n--- разбор сохранённых страниц Авито ---\n");

    test_search_page(directory, "real.html", 49, "iPhone 15, 128 ГБ, SIM + eSIM", 32000);
    test_search_page(directory, "http_block.html", 50, "Велосипед Intense 27.5''", 149000);

    std::string html;
    if (load(directory, "live_item.html", html)) {
        avito::ItemInfo info = avito::parse_item(
            html, "https://www.avito.ru/moskva/velosipedy/detskoe_velokreslo_polisport_8393884149");
        report(info.title == "Детское велокресло Polisport", "объявление: заголовок",
               quoted(info.title));
        // The page carries a strip of suggested listings built from the same
        // markup; the price must come from the listing's own block.
        report(info.price == 3000, "объявление: цена своя, не из карусели",
               std::to_string(info.price));
        report(info.region == "moskva" && info.category == "velosipedy",
               "объявление: регион и категория", info.region + "/" + info.category);
        report(info.item_id == "8393884149", "объявление: номер", info.item_id);
        report(!avito::is_removed_item(html), "объявление: не снято с продажи");
    } else {
        std::printf("[skip] live_item.html не найден\n");
    }

    if (load(directory, "item.html", html)) {
        report(avito::is_removed_item(html), "снятое объявление распознано");
    }

    if (load(directory, "avito.html", html)) {
        report(net::looks_blocked(html, 200, avito::kSearchMarker),
               "страница-заглушка распознана как блокировка");
        report(util::contains(net::page_title(html), "доступ ограничен"),
               "заголовок заглушки", net::page_title(html));
    }
}

// --- live request --------------------------------------------------------
void test_live() {
    std::printf("\n--- живой запрос к Авито ---\n");

    core::Settings settings;
    settings.request_delay_min = 1.0;
    settings.request_delay_max = 2.0;

    net::Fetcher fetcher;
    fetcher.apply_settings(settings);

    net::FetchResult page = fetcher.get("https://www.avito.ru/moskva/velosipedy?s=104",
                                        avito::kSearchMarker);
    if (!page.ok()) {
        report(false, "загрузка выдачи", page.error);
        return;
    }
    report(true, "загрузка выдачи", page.transport + ", " +
                                        std::to_string(page.html.size()) + " байт");

    std::vector<core::Listing> items = avito::parse_search(page.html);
    report(!items.empty(), "разбор живой выдачи",
           std::to_string(items.size()) + " карточек");

    for (size_t i = 0; i < items.size() && i < 3; ++i) {
        const core::Listing& item = items[i];
        std::printf("       %s | %s | %s | фото:%s\n", quoted(item.title, 38).c_str(),
                    item.price_label().c_str(), item.date_text.c_str(),
                    item.image_url.empty() ? "нет" : "есть");
    }

    if (items.empty()) return;

    size_t photos = 0;
    for (const core::Listing& item : items) {
        if (!item.image_url.empty()) ++photos;
    }
    report(photos == items.size(), "фото у всех живых карточек",
           std::to_string(photos) + "/" + std::to_string(items.size()));

    // Now a real listing page, the basis of the "similar" feature.
    for (size_t i = 0; i < items.size() && i < 4; ++i) {
        net::FetchResult item_page = fetcher.get(items[i].url, {});
        if (!item_page.ok()) continue;
        if (avito::is_removed_item(item_page.html)) {
            std::printf("       (объявление снято, пробую следующее)\n");
            continue;
        }
        avito::ItemInfo info = avito::parse_item(item_page.html, items[i].url);
        report(!info.title.empty(), "разбор живого объявления", quoted(info.title));
        report(info.price > 0, "цена живого объявления", std::to_string(info.price));
        report(!info.region.empty() && !info.category.empty(), "регион и категория",
               info.region + "/" + info.category);
        std::printf("       запрос похожих: %s\n", avito::build_query(info.title).c_str());
        std::printf("       ссылка:         %s\n",
                    avito::build_search_url(info, 40).c_str());
        return;
    }
    std::printf("[skip] живое объявление найти не удалось\n");
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    std::string fixtures;
    bool live = false;
    for (int i = 1; i < argc; ++i) {
        std::string argument = argv[i];
        if (argument == "--live") live = true;
        else fixtures = argument;
    }

    test_utilities();
    test_similarity();
    test_urls();
    if (!fixtures.empty()) test_fixtures(fixtures);
    if (live) test_live();

    std::printf("\n=== проверок: %d, провалено: %d ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
