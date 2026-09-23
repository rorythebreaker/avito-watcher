// Finding listings similar to a given one.
//
// The sample listing gives a title, a price, a category and a city. The title
// becomes a search query, the price becomes a band around it, and every result
// is then scored on how much of the sample's wording it repeats.
#pragma once

#include <string>
#include <vector>

#include "avito/parser.h"

namespace avito {

// Significant words of a title: lowercased, punctuation dropped, "ё" folded to
// "е", filler words removed.
std::vector<std::string> normalize_words(const std::string& title);

// Shortens a title into a search query. The leading words are kept because
// sellers start with the kind of thing and its make, while the tail holds
// colours and sizes. A full title as a query tends to match exactly one
// listing - the sample itself.
std::string build_query(const std::string& title, int max_words = 4);

// Search address for listings of the same kind. An empty query means "derive
// one from the sample title".
std::string build_search_url(const ItemInfo& info, int price_tolerance,
                             const std::string& query = {});

// How much of the sample's wording the candidate repeats, 0..100.
int title_score(const std::string& reference, const std::string& candidate);

// Whether the candidate price sits inside the band. The same band is sent to
// Avito as pmin/pmax, so it is a hard condition here too: a slider that says
// "±40%" has to mean exactly that.
bool price_matches(long long reference, long long candidate, int tolerance);

// Final similarity, 0..100. A price outside the band scores zero.
int score(const std::string& reference_title, long long reference_price,
          const std::string& candidate_title, long long candidate_price,
          int price_tolerance);

}  // namespace avito
