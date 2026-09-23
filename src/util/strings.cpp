#include "util/strings.h"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace util {
namespace {

bool is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

// Avito writes prices with a non-breaking space (U+00A0, "\xC2\xA0" in UTF-8).
bool is_nbsp_at(std::string_view text, size_t i) {
    return i + 1 < text.size() &&
           static_cast<unsigned char>(text[i]) == 0xC2 &&
           static_cast<unsigned char>(text[i + 1]) == 0xA0;
}

}  // namespace

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), size);
    return result;
}

std::string narrow(std::wstring_view utf16) {
    if (utf16.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::string trim(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && is_space(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && is_space(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

std::string to_lower(std::string_view text) {
    // ASCII-only fast path plus the Cyrillic block, which is all this app needs
    // for case-insensitive matching of Avito markup and titles.
    std::wstring wide = widen(text);
    if (!wide.empty()) {
        CharLowerBuffW(wide.data(), static_cast<DWORD>(wide.size()));
        return narrow(wide);
    }
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

bool icontains(std::string_view text, std::string_view needle) {
    return contains(to_lower(text), to_lower(needle));
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t pos = text.find(separator, start);
        if (pos == std::string_view::npos) {
            parts.emplace_back(text.substr(start));
            break;
        }
        parts.emplace_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

std::string join(const std::vector<std::string>& parts, std::string_view glue) {
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) result.append(glue);
        result.append(parts[i]);
    }
    return result;
}

std::string replace_all(std::string_view text, std::string_view from, std::string_view to) {
    if (from.empty()) return std::string(text);
    std::string result;
    result.reserve(text.size());
    size_t start = 0;
    while (true) {
        size_t pos = text.find(from, start);
        if (pos == std::string_view::npos) {
            result.append(text.substr(start));
            return result;
        }
        result.append(text.substr(start, pos - start));
        result.append(to);
        start = pos + from.size();
    }
}

std::string squeeze_spaces(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    bool pending_space = false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (is_nbsp_at(text, i)) {
            pending_space = true;
            ++i;
            continue;
        }
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (is_space(c)) {
            pending_space = true;
            continue;
        }
        if (pending_space && !result.empty()) result.push_back(' ');
        pending_space = false;
        result.push_back(text[i]);
    }
    return result;
}

std::string url_encode(std::string_view text) {
    static const char* kHex = "0123456789ABCDEF";
    std::string result;
    result.reserve(text.size() * 3);
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            result.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            result.push_back('+');
        } else {
            result.push_back('%');
            result.push_back(kHex[c >> 4]);
            result.push_back(kHex[c & 0x0F]);
        }
    }
    return result;
}

std::string url_decode(std::string_view text) {
    auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            int high = hex_value(text[i + 1]);
            int low = hex_value(text[i + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        result.push_back(text[i] == '+' ? ' ' : text[i]);
    }
    return result;
}

std::string html_escape(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&#39;"; break;
            default: result.push_back(c);
        }
    }
    return result;
}

std::string format_thousands(long long value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", value < 0 ? -value : value);
    std::string digits(buffer);
    std::string grouped;
    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (count && count % 3 == 0) grouped.push_back(' ');
        grouped.push_back(*it);
        ++count;
    }
    if (value < 0) grouped.push_back('-');
    std::reverse(grouped.begin(), grouped.end());
    return grouped;
}

bool parse_number(std::string_view text, long long& out) {
    std::string digits;
    for (size_t i = 0; i < text.size(); ++i) {
        if (is_nbsp_at(text, i)) { ++i; continue; }
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (std::isdigit(c)) {
            digits.push_back(static_cast<char>(c));
        } else if (!digits.empty() && !is_space(c) && c != ' ') {
            break;  // digits ended at a real character, stop at the first group
        }
    }
    if (digits.empty()) return false;
    if (digits.size() > 18) digits.resize(18);
    out = std::strtoll(digits.c_str(), nullptr, 10);
    return true;
}

std::string base64_encode(const unsigned char* data, size_t size) {
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((size + 2) / 3) * 4);
    for (size_t i = 0; i < size; i += 3) {
        unsigned value = data[i] << 16;
        if (i + 1 < size) value |= data[i + 1] << 8;
        if (i + 2 < size) value |= data[i + 2];
        result.push_back(kTable[(value >> 18) & 0x3F]);
        result.push_back(kTable[(value >> 12) & 0x3F]);
        result.push_back(i + 1 < size ? kTable[(value >> 6) & 0x3F] : '=');
        result.push_back(i + 2 < size ? kTable[value & 0x3F] : '=');
    }
    return result;
}

std::vector<unsigned char> base64_decode(std::string_view text) {
    auto value_of = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<unsigned char> result;
    int buffer = 0;
    int bits = 0;
    for (char c : text) {
        int value = value_of(c);
        if (value < 0) continue;
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<unsigned char>((buffer >> bits) & 0xFF));
        }
    }
    return result;
}

std::string sha1_hex(std::string_view data) {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    std::string result;
    if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL,
                             CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        if (CryptCreateHash(provider, CALG_SHA1, 0, 0, &hash)) {
            if (CryptHashData(hash, reinterpret_cast<const BYTE*>(data.data()),
                              static_cast<DWORD>(data.size()), 0)) {
                BYTE digest[20] = {};
                DWORD length = sizeof(digest);
                if (CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0)) {
                    static const char* kHex = "0123456789abcdef";
                    for (DWORD i = 0; i < length; ++i) {
                        result.push_back(kHex[digest[i] >> 4]);
                        result.push_back(kHex[digest[i] & 0x0F]);
                    }
                }
            }
            CryptDestroyHash(hash);
        }
        CryptReleaseContext(provider, 0);
    }
    return result;
}

}  // namespace util
