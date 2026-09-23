#include "avito/parser.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include "util/strings.h"

namespace avito {

const char* const kSearchMarker = "data-marker=\"item\"";
const char* const kItemMarker = "data-marker=\"item-view/title-info\"";

namespace {

const std::unordered_set<std::string>& void_tags() {
    static const std::unordered_set<std::string> tags = {
        "img", "meta", "br", "hr", "input", "link", "source", "area", "base",
        "col", "embed", "param", "track", "wbr",
    };
    return tags;
}

bool name_char(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '-' || c == '_' || c == ':';
}

// Walks back from an attribute to the '<' that opens its tag.
size_t tag_open_before(const std::string& html, size_t attr_pos) {
    size_t limit = attr_pos > 4096 ? attr_pos - 4096 : 0;
    for (size_t i = attr_pos; i > limit; --i) {
        if (html[i - 1] == '<') return i - 1;
        if (html[i - 1] == '>') return std::string::npos;  // left the tag
    }
    return std::string::npos;
}

std::string tag_name_at(const std::string& html, size_t tag_start) {
    size_t i = tag_start + 1;
    if (i < html.size() && html[i] == '/') ++i;
    size_t begin = i;
    while (i < html.size() && name_char(html[i])) ++i;
    return util::to_lower(html.substr(begin, i - begin));
}

// End of the opening tag, i.e. the position just past its '>'.
size_t open_tag_end(const std::string& html, size_t tag_start) {
    bool in_quote = false;
    char quote = '\0';
    for (size_t i = tag_start; i < html.size(); ++i) {
        char c = html[i];
        if (in_quote) {
            if (c == quote) in_quote = false;
        } else if (c == '"' || c == '\'') {
            in_quote = true;
            quote = c;
        } else if (c == '>') {
            return i + 1;
        }
    }
    return std::string::npos;
}

// Position just past the element's closing tag, counting nested elements of the
// same name so a card containing inner divs is sliced whole.
size_t element_end(const std::string& html, size_t tag_start) {
    std::string name = tag_name_at(html, tag_start);
    size_t after_open = open_tag_end(html, tag_start);
    if (after_open == std::string::npos) return std::string::npos;
    if (name.empty() || void_tags().count(name)) return after_open;
    if (after_open >= 2 && html[after_open - 2] == '/') return after_open;  // self closing

    const std::string open_needle = "<" + name;
    const std::string close_needle = "</" + name;
    int depth = 1;
    size_t cursor = after_open;

    while (cursor < html.size() && depth > 0) {
        size_t next_open = html.find(open_needle, cursor);
        size_t next_close = html.find(close_needle, cursor);
        if (next_close == std::string::npos) return html.size();

        if (next_open != std::string::npos && next_open < next_close) {
            // Make sure "<div" did not match the start of "<divider".
            size_t after = next_open + open_needle.size();
            if (after < html.size() && name_char(html[after])) {
                cursor = next_open + 1;
                continue;
            }
            ++depth;
            cursor = next_open + open_needle.size();
            continue;
        }

        --depth;
        cursor = next_close + close_needle.size();
        if (depth == 0) {
            size_t end = html.find('>', cursor);
            return end == std::string::npos ? html.size() : end + 1;
        }
    }
    return cursor;
}

// Reads an attribute of the tag that starts at tag_start.
std::string attribute_at(const std::string& html, size_t tag_start, const std::string& name) {
    size_t tag_end = open_tag_end(html, tag_start);
    if (tag_end == std::string::npos) return {};

    size_t cursor = tag_start;
    while (true) {
        size_t found = html.find(name, cursor);
        if (found == std::string::npos || found >= tag_end) return {};
        // The name must stand alone and be followed by '='.
        bool left_ok = found > 0 && !name_char(html[found - 1]);
        size_t after = found + name.size();
        while (after < tag_end && html[after] == ' ') ++after;
        if (!left_ok || after >= tag_end || html[after] != '=') {
            cursor = found + 1;
            continue;
        }
        ++after;
        while (after < tag_end && html[after] == ' ') ++after;
        if (after >= tag_end) return {};

        char quote = html[after];
        if (quote == '"' || quote == '\'') {
            size_t value_start = after + 1;
            size_t value_end = html.find(quote, value_start);
            if (value_end == std::string::npos || value_end > tag_end) return {};
            return html.substr(value_start, value_end - value_start);
        }
        size_t value_end = after;
        while (value_end < tag_end && html[value_end] != ' ' && html[value_end] != '>') ++value_end;
        return html.substr(after, value_end - after);
    }
}

struct Element {
    size_t start = std::string::npos;
    size_t end = std::string::npos;
    bool valid() const { return start != std::string::npos && end != std::string::npos; }
};

// Locates the element carrying data-marker="<marker>" inside the slice.
Element find_marker(const std::string& html, const std::string& marker, size_t from = 0) {
    const std::string needle = "data-marker=\"" + marker + "\"";
    size_t found = html.find(needle, from);
    if (found == std::string::npos) return {};
    size_t start = tag_open_before(html, found);
    if (start == std::string::npos) return {};
    Element element;
    element.start = start;
    element.end = element_end(html, start);
    return element;
}

// Same, but the marker value only has to start with the prefix. Avito encodes
// slider image addresses right inside the marker value.
Element find_marker_prefix(const std::string& html, const std::string& prefix,
                           std::string& value) {
    const std::string needle = "data-marker=\"" + prefix;
    size_t found = html.find(needle);
    if (found == std::string::npos) return {};
    size_t value_start = found + needle.size();
    size_t value_end = html.find('"', value_start);
    if (value_end == std::string::npos) return {};
    value = html.substr(value_start, value_end - value_start);

    size_t start = tag_open_before(html, found);
    if (start == std::string::npos) return {};
    Element element;
    element.start = start;
    element.end = element_end(html, start);
    return element;
}

std::string decode_entities(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out.push_back(text[i]);
            continue;
        }
        size_t semicolon = text.find(';', i + 1);
        if (semicolon == std::string::npos || semicolon - i > 10) {
            out.push_back('&');
            continue;
        }
        std::string name = text.substr(i + 1, semicolon - i - 1);
        i = semicolon;

        if (name == "amp") { out.push_back('&'); continue; }
        if (name == "lt") { out.push_back('<'); continue; }
        if (name == "gt") { out.push_back('>'); continue; }
        if (name == "quot") { out.push_back('"'); continue; }
        if (name == "apos" || name == "#39") { out.push_back('\''); continue; }
        if (name == "nbsp" || name == "#160") { out += "\xC2\xA0"; continue; }

        if (!name.empty() && name[0] == '#') {
            unsigned int code = 0;
            if (name.size() > 2 && (name[1] == 'x' || name[1] == 'X')) {
                code = static_cast<unsigned int>(std::strtoul(name.c_str() + 2, nullptr, 16));
            } else {
                code = static_cast<unsigned int>(std::strtoul(name.c_str() + 1, nullptr, 10));
            }
            if (code == 0) continue;
            if (code < 0x80) {
                out.push_back(static_cast<char>(code));
            } else if (code < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            continue;
        }
        // Unknown entity, keep it as written.
        out.push_back('&');
        out.append(name);
        out.push_back(';');
    }
    return out;
}

std::string text_of(const std::string& html, const Element& element) {
    if (!element.valid()) return {};
    return strip_tags(html.substr(element.start, element.end - element.start));
}

std::string marker_text(const std::string& slice, std::initializer_list<const char*> markers) {
    for (const char* marker : markers) {
        Element element = find_marker(slice, marker);
        if (element.valid()) {
            std::string text = text_of(slice, element);
            if (!text.empty()) return text;
        }
    }
    return {};
}

std::string absolute_url(const std::string& href) {
    if (href.empty()) return {};
    if (util::starts_with(href, "http://") || util::starts_with(href, "https://")) return href;
    if (util::starts_with(href, "//")) return "https:" + href;
    if (href[0] == '/') return "https://www.avito.ru" + href;
    return "https://www.avito.ru/" + href;
}

// "url 208w, url 416w" -> the widest address.
std::string widest_from_srcset(const std::string& srcset) {
    std::string best;
    long long best_width = -1;
    for (const std::string& part : util::split(srcset, ',')) {
        std::string entry = util::trim(part);
        if (entry.empty()) continue;
        size_t space = entry.find(' ');
        std::string url = space == std::string::npos ? entry : entry.substr(0, space);
        long long width = 0;
        if (space != std::string::npos) util::parse_number(entry.substr(space), width);
        if (width > best_width) {
            best_width = width;
            best = url;
        }
    }
    return best;
}

std::string card_image(const std::string& slice) {
    // Prefer a real <img>: the first cards on the page carry one.
    size_t cursor = 0;
    while (true) {
        size_t img = slice.find("<img", cursor);
        if (img == std::string::npos) break;
        cursor = img + 4;

        std::string srcset = attribute_at(slice, img, "srcset");
        std::string url = widest_from_srcset(decode_entities(srcset));
        if (url.empty()) url = decode_entities(attribute_at(slice, img, "src"));
        if (url.empty()) url = decode_entities(attribute_at(slice, img, "data-src"));
        if (!url.empty() && !util::starts_with(url, "data:")) return absolute_url(url);
    }

    // Cards below the fold arrive without an <img> tag at all, but the photo
    // address is still there inside the slider's data-marker value.
    std::string value;
    Element slider = find_marker_prefix(slice, "slider-image/image-", value);
    if (slider.valid() && !value.empty()) return absolute_url(decode_entities(value));
    return {};
}

long long price_from_slice(const std::string& slice, std::string& price_text) {
    // The machine readable value wins over the formatted one.
    size_t cursor = 0;
    while (true) {
        size_t found = slice.find("itemprop=\"price\"", cursor);
        if (found == std::string::npos) break;
        cursor = found + 1;
        size_t tag = tag_open_before(slice, found);
        if (tag == std::string::npos) continue;
        std::string content = attribute_at(slice, tag, "content");
        long long value = 0;
        if (!content.empty() && util::parse_number(content, value) && value > 0) return value;
    }

    price_text = marker_text(slice, {"item-price", "item-price-value"});
    long long value = 0;
    if (!price_text.empty() && util::parse_number(price_text, value)) return value;
    return 0;
}

}  // namespace

std::string strip_tags(const std::string& markup) {
    std::string out;
    out.reserve(markup.size() / 2);

    for (size_t i = 0; i < markup.size();) {
        if (markup[i] != '<') {
            out.push_back(markup[i]);
            ++i;
            continue;
        }

        std::string name = tag_name_at(markup, i);
        if (name == "script" || name == "style" || name == "noscript") {
            // Skip past the closing tag, not just up to it: tag_name_at reads
            // "</script" as "script" too, so stopping on it would find the very
            // same tag again on the next turn and never move forward.
            size_t close = markup.find("</" + name, i);
            if (close == std::string::npos) {
                i = markup.size();
            } else {
                size_t after = open_tag_end(markup, close);
                i = after == std::string::npos ? markup.size() : after;
            }
            continue;
        }
        size_t after = open_tag_end(markup, i);
        i = after == std::string::npos ? markup.size() : after;
        out.push_back(' ');  // tags act as word separators
    }
    return util::squeeze_spaces(decode_entities(out));
}

std::string item_id_from_url(const std::string& url) {
    std::string path = url;
    size_t query = path.find_first_of("?#");
    if (query != std::string::npos) path = path.substr(0, query);
    while (!path.empty() && path.back() == '/') path.pop_back();

    size_t end = path.size();
    size_t begin = end;
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(path[begin - 1]))) --begin;
    if (begin == end || end - begin < 6) return {};
    if (begin == 0) return {};
    char separator = path[begin - 1];
    if (separator != '-' && separator != '_') return {};
    return path.substr(begin, end - begin);
}

std::vector<core::Listing> parse_search(const std::string& html) {
    std::vector<core::Listing> listings;
    std::unordered_set<std::string> seen;

    const std::string needle = kSearchMarker;
    size_t cursor = 0;
    while (true) {
        size_t found = html.find(needle, cursor);
        if (found == std::string::npos) break;

        size_t card_start = tag_open_before(html, found);
        if (card_start == std::string::npos) { cursor = found + needle.size(); continue; }
        size_t card_end = element_end(html, card_start);
        if (card_end == std::string::npos || card_end <= card_start) {
            cursor = found + needle.size();
            continue;
        }
        cursor = card_end;

        const std::string slice = html.substr(card_start, card_end - card_start);

        core::Listing listing;
        listing.item_id = attribute_at(slice, 0, "data-item-id");

        Element title_element = find_marker(slice, "item-title");
        std::string href;
        if (title_element.valid()) {
            href = decode_entities(attribute_at(slice, title_element.start, "href"));
            listing.title = text_of(slice, title_element);
            if (listing.title.empty()) {
                listing.title = decode_entities(attribute_at(slice, title_element.start, "title"));
            }
        }
        if (href.empty()) {
            // Fall back to the first anchor that looks like a listing address.
            size_t anchor = slice.find("<a ");
            while (anchor != std::string::npos) {
                std::string candidate = decode_entities(attribute_at(slice, anchor, "href"));
                if (!item_id_from_url(candidate).empty()) { href = candidate; break; }
                anchor = slice.find("<a ", anchor + 3);
            }
        }
        if (href.empty()) continue;

        listing.url = absolute_url(href);
        size_t query = listing.url.find('?');
        if (query != std::string::npos) listing.url = listing.url.substr(0, query);

        if (listing.item_id.empty()) listing.item_id = item_id_from_url(listing.url);
        if (listing.item_id.empty()) continue;
        if (!seen.insert(listing.item_id).second) continue;  // promoted duplicate

        listing.price = price_from_slice(slice, listing.price_text);
        listing.location = marker_text(slice, {"item-location", "item-address"});
        listing.date_text = marker_text(slice, {"item-date"});
        listing.seller = marker_text(slice, {"seller-info/name", "seller-link/link"});
        listing.description = marker_text(slice, {"item-description", "item-specific-params"});
        if (listing.description.size() > 400) listing.description.resize(400);
        listing.image_url = card_image(slice);

        listings.push_back(std::move(listing));
    }
    return listings;
}

bool is_removed_item(const std::string& html) {
    if (html.find(kItemMarker) != std::string::npos) return false;

    size_t h1 = html.find("<h1");
    if (h1 == std::string::npos) return false;
    size_t end = element_end(html, h1);
    if (end == std::string::npos) return false;
    std::string heading = util::to_lower(strip_tags(html.substr(h1, end - h1)));

    static const char* kSigns[] = {"снят с", "снято с продажи", "подобрали похожие",
                                   "объявление удалено"};
    for (const char* sign : kSigns) {
        if (heading.find(sign) != std::string::npos) return true;
    }
    return false;
}

long long total_found(const std::string& html) {
    Element element = find_marker(html, "page-title/count");
    if (!element.valid()) return -1;
    long long value = 0;
    if (util::parse_number(text_of(html, element), value)) return value;
    return -1;
}

ItemInfo parse_item(const std::string& html, const std::string& url) {
    ItemInfo info;
    info.url = url;
    size_t query = info.url.find('?');
    if (query != std::string::npos) info.url = info.url.substr(0, query);
    info.item_id = item_id_from_url(info.url);

    Element title = find_marker(html, "item-view/title-info");
    info.title = text_of(html, title);
    if (info.title.empty()) {
        size_t og = html.find("property=\"og:title\"");
        if (og != std::string::npos) {
            size_t tag = tag_open_before(html, og);
            if (tag != std::string::npos) {
                info.title = util::trim(decode_entities(attribute_at(html, tag, "content")));
            }
        }
    }
    if (info.title.empty()) {
        size_t h1 = html.find("<h1");
        if (h1 != std::string::npos) {
            size_t end = element_end(html, h1);
            if (end != std::string::npos) info.title = strip_tags(html.substr(h1, end - h1));
        }
    }

    // The price must come from the listing's own block. Avito appends a strip of
    // suggested listings built from the very same markup, and without this
    // scoping the sample would pick up a neighbour's price.
    Element price_scope = find_marker(html, "item-view/item-price-container");
    if (!price_scope.valid()) price_scope = find_marker(html, "item-view/item-price");
    if (price_scope.valid()) {
        std::string slice = html.substr(price_scope.start, price_scope.end - price_scope.start);
        std::string text;
        info.price = price_from_slice(slice, text);
        info.price_text = strip_tags(slice);
        if (info.price == 0) util::parse_number(info.price_text, info.price);
    } else {
        size_t found = html.find("itemprop=\"price\"");
        if (found != std::string::npos) {
            size_t tag = tag_open_before(html, found);
            if (tag != std::string::npos) {
                util::parse_number(attribute_at(html, tag, "content"), info.price);
            }
        }
    }

    info.location = marker_text(html, {"item-view/item-address", "item-address"});

    size_t og_image = html.find("property=\"og:image\"");
    if (og_image != std::string::npos) {
        size_t tag = tag_open_before(html, og_image);
        if (tag != std::string::npos) {
            info.image_url = decode_entities(attribute_at(html, tag, "content"));
        }
    }

    // Region and category come straight from the address:
    // /moskva/telefony/iphone_15_128_gb_8377047842
    std::string path = info.url;
    size_t scheme = path.find("://");
    if (scheme != std::string::npos) {
        size_t slash = path.find('/', scheme + 3);
        path = slash == std::string::npos ? "" : path.substr(slash);
    }
    std::vector<std::string> parts;
    for (const std::string& part : util::split(path, '/')) {
        if (!part.empty()) parts.push_back(part);
    }
    if (parts.size() >= 2) {
        info.region = parts[0];
        info.category = parts[1];
    }
    return info;
}

}  // namespace avito
