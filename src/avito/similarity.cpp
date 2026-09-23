#include "avito/similarity.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <set>
#include <unordered_set>

#include "util/strings.h"

namespace avito {
namespace {

// Words that say nothing about the goods and only narrow the search.
const std::unordered_set<std::string>& stopwords() {
    static const std::unordered_set<std::string> words = {
        "в", "во", "и", "на", "с", "со", "за", "от", "до", "из", "по", "для", "под",
        "или", "не", "но", "а", "к", "у", "о", "об", "при", "как", "the", "a", "an",
        "новый", "новая", "новое", "новые", "продам", "продаю", "продается",
        "продаётся", "срочно", "торг", "идеальном", "отличном", "хорошем",
        "состоянии", "состояние", "бу", "оригинал", "недорого", "дёшево",
        "дешево", "цена", "руб", "рублей", "шт", "комплект", "есть", "нет",
    };
    return words;
}

}  // namespace

std::vector<std::string> normalize_words(const std::string& title) {
    // Going through UTF-16 keeps Cyrillic letters intact for the character
    // classification calls, which is not the case byte by byte in UTF-8.
    std::wstring wide = util::widen(title);
    if (!wide.empty()) CharLowerBuffW(wide.data(), static_cast<DWORD>(wide.size()));

    std::vector<std::string> words;
    std::wstring current;
    auto flush = [&] {
        if (current.empty()) return;
        std::string word = util::narrow(current);
        current.clear();
        if (word.size() < 2) return;               // one letter carries no meaning
        if (stopwords().count(word)) return;
        words.push_back(std::move(word));
    };

    for (wchar_t c : wide) {
        if (c == L'ё') c = L'е';          // ё -> е
        if (iswalnum(static_cast<wint_t>(c))) {
            current.push_back(c);
        } else {
            flush();
        }
    }
    flush();
    return words;
}

std::string build_query(const std::string& title, int max_words) {
    std::vector<std::string> words = normalize_words(title);
    if (max_words > 0 && static_cast<int>(words.size()) > max_words) {
        words.resize(static_cast<size_t>(max_words));
    }
    return util::join(words, " ");
}

std::string build_search_url(const ItemInfo& info, int price_tolerance,
                             const std::string& query) {
    std::string region = info.region.empty() ? "rossiya" : info.region;
    std::string path = "/" + region;
    if (!info.category.empty()) path += "/" + info.category;

    std::string text = query.empty() ? build_query(info.title) : query;

    std::string params;
    auto add = [&](const std::string& key, const std::string& value) {
        if (!params.empty()) params += "&";
        params += key + "=" + value;
    };

    if (!text.empty()) add("q", util::url_encode(text));
    if (info.price > 0 && price_tolerance > 0) {
        double factor = static_cast<double>(price_tolerance) / 100.0;
        long long low = static_cast<long long>(info.price * (1.0 - factor));
        long long high = static_cast<long long>(info.price * (1.0 + factor));
        add("pmin", std::to_string(low < 0 ? 0 : low));
        add("pmax", std::to_string(high));
    }
    add("s", "104");  // sort by date, newest first

    return "https://www.avito.ru" + path + "?" + params;
}

int title_score(const std::string& reference, const std::string& candidate) {
    std::vector<std::string> reference_words = normalize_words(reference);
    std::vector<std::string> candidate_words = normalize_words(candidate);

    std::set<std::string> left(reference_words.begin(), reference_words.end());
    std::set<std::string> right(candidate_words.begin(), candidate_words.end());
    if (left.empty() || right.empty()) return 0;

    size_t common = 0;
    for (const std::string& word : left) {
        if (right.count(word)) ++common;
    }
    size_t united = left.size() + right.size() - common;

    double coverage = static_cast<double>(common) / static_cast<double>(left.size());
    double jaccard = united ? static_cast<double>(common) / static_cast<double>(united) : 0.0;
    return static_cast<int>(std::lround(100.0 * (0.7 * coverage + 0.3 * jaccard)));
}

bool price_matches(long long reference, long long candidate, int tolerance) {
    if (reference <= 0 || candidate <= 0 || tolerance <= 0) return true;
    double deviation = std::fabs(static_cast<double>(candidate - reference)) /
                       static_cast<double>(reference) * 100.0;
    return deviation <= static_cast<double>(tolerance);
}

int score(const std::string& reference_title, long long reference_price,
          const std::string& candidate_title, long long candidate_price,
          int price_tolerance) {
    if (!price_matches(reference_price, candidate_price, price_tolerance)) return 0;
    return title_score(reference_title, candidate_title);
}

}  // namespace avito
