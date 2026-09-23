// Reading Avito pages: the search results and a single listing.
//
// There is no DOM here. The pages are two megabytes of generated markup and all
// the app needs is a handful of fields per card, so the parser scans for the
// data-marker attributes Avito puts on every element and slices out the element
// that carries them. Each field is looked up through several markers in turn: if
// the layout changes again, one field stops resolving instead of the whole page.
#pragma once

#include <string>
#include <vector>

#include "core/models.h"

namespace avito {

// Markup that must be present for a page to be worth parsing. Passed to the
// fetcher so a stub page triggers the browser fallback.
extern const char* const kSearchMarker;  // data-marker="item"
extern const char* const kItemMarker;    // data-marker="item-view/title-info"

struct ItemInfo {
    std::string item_id;
    std::string title;
    long long price = 0;
    std::string price_text;
    std::string region;
    std::string category;
    std::string location;
    std::string image_url;
    std::string url;
};

std::vector<core::Listing> parse_search(const std::string& html);

ItemInfo parse_item(const std::string& html, const std::string& url);

// A withdrawn listing is replaced by a page of suggestions, which must not be
// mistaken for a parse failure.
bool is_removed_item(const std::string& html);

// How many listings Avito reports for the query, or -1 when unknown.
long long total_found(const std::string& html);

// Extracts the numeric id from a listing address. Avito separates it with a
// hyphen or an underscore: /moskva/telefony/iphone_15_128_gb_8377047842
std::string item_id_from_url(const std::string& url);

// Turns markup into readable text: tags dropped, entities decoded, runs of
// whitespace collapsed.
std::string strip_tags(const std::string& markup);

}  // namespace avito
