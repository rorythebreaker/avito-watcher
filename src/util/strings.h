// Small string helpers shared across the app.
//
// The whole program keeps text as UTF-8 std::string internally and converts to
// UTF-16 only at the Win32 boundary. That keeps parsing, JSON and networking
// code free of wide-char noise.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace util {

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view utf16);

std::string trim(std::string_view text);
std::string to_lower(std::string_view text);

bool starts_with(std::string_view text, std::string_view prefix);
bool ends_with(std::string_view text, std::string_view suffix);
bool contains(std::string_view text, std::string_view needle);
bool icontains(std::string_view text, std::string_view needle);

std::vector<std::string> split(std::string_view text, char separator);
std::string join(const std::vector<std::string>& parts, std::string_view glue);
std::string replace_all(std::string_view text, std::string_view from, std::string_view to);

// Collapses runs of whitespace into single spaces and trims the result.
std::string squeeze_spaces(std::string_view text);

// Percent-encodes a value for use in a query string.
std::string url_encode(std::string_view text);
std::string url_decode(std::string_view text);

// Escapes the five characters that must not appear raw in HTML text.
std::string html_escape(std::string_view text);

// Formats an integer with thin spaces between thousands: 45000 -> "45 000".
std::string format_thousands(long long value);

// Parses the first run of digits found in the text, ignoring spaces used as
// thousand separators. Returns false when the text holds no digits.
bool parse_number(std::string_view text, long long& out);

std::string base64_encode(const unsigned char* data, size_t size);
std::vector<unsigned char> base64_decode(std::string_view text);

// Lowercase hexadecimal SHA-1 of the input, used for cache file names.
std::string sha1_hex(std::string_view data);

}  // namespace util
