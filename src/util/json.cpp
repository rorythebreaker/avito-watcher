#include "util/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace util {
namespace {

const Json& null_value() {
    static const Json kNull;
    return kNull;
}

// Appends a Unicode code point to an UTF-8 string.
void append_utf8(std::string& out, unsigned int code_point) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    bool parse_value(Json& out) {
        skip_space();
        if (pos_ >= text_.size()) return false;
        switch (text_[pos_]) {
            case '{': return parse_object(out);
            case '[': return parse_array(out);
            case '"': {
                std::string value;
                if (!parse_string(value)) return false;
                out = Json(std::move(value));
                return true;
            }
            case 't':
                if (text_.compare(pos_, 4, "true") != 0) return false;
                pos_ += 4;
                out = Json(true);
                return true;
            case 'f':
                if (text_.compare(pos_, 5, "false") != 0) return false;
                pos_ += 5;
                out = Json(false);
                return true;
            case 'n':
                if (text_.compare(pos_, 4, "null") != 0) return false;
                pos_ += 4;
                out = Json();
                return true;
            default: return parse_number(out);
        }
    }

    void skip_space() {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    bool at_end() {
        skip_space();
        return pos_ >= text_.size();
    }

private:
    bool parse_object(Json& out) {
        out = Json::object();
        ++pos_;  // '{'
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == '}') { ++pos_; return true; }
        while (pos_ < text_.size()) {
            skip_space();
            std::string key;
            if (!parse_string(key)) return false;
            skip_space();
            if (pos_ >= text_.size() || text_[pos_] != ':') return false;
            ++pos_;
            Json value;
            if (!parse_value(value)) return false;
            out[key] = std::move(value);
            skip_space();
            if (pos_ >= text_.size()) return false;
            if (text_[pos_] == ',') { ++pos_; continue; }
            if (text_[pos_] == '}') { ++pos_; return true; }
            return false;
        }
        return false;
    }

    bool parse_array(Json& out) {
        out = Json::array();
        ++pos_;  // '['
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == ']') { ++pos_; return true; }
        while (pos_ < text_.size()) {
            Json value;
            if (!parse_value(value)) return false;
            out.push_back(std::move(value));
            skip_space();
            if (pos_ >= text_.size()) return false;
            if (text_[pos_] == ',') { ++pos_; continue; }
            if (text_[pos_] == ']') { ++pos_; return true; }
            return false;
        }
        return false;
    }

    bool parse_string(std::string& out) {
        skip_space();
        if (pos_ >= text_.size() || text_[pos_] != '"') return false;
        ++pos_;
        out.clear();
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (pos_ >= text_.size()) return false;
            char escape = text_[pos_++];
            switch (escape) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned int code = 0;
                    if (!read_hex4(code)) return false;
                    // Surrogate pair: the second half follows as another \u escape.
                    if (code >= 0xD800 && code <= 0xDBFF && pos_ + 1 < text_.size() &&
                        text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                        size_t saved = pos_;
                        pos_ += 2;
                        unsigned int low = 0;
                        if (read_hex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            pos_ = saved;
                        }
                    }
                    append_utf8(out, code);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool read_hex4(unsigned int& out) {
        if (pos_ + 4 > text_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = text_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool parse_number(Json& out) {
        size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
        bool any = false;
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                ((c == '-' || c == '+') && (text_[pos_ - 1] == 'e' || text_[pos_ - 1] == 'E'))) {
                any = true;
                ++pos_;
            } else {
                break;
            }
        }
        if (!any) return false;
        out = Json(std::strtod(std::string(text_.substr(start, pos_ - start)).c_str(), nullptr));
        return true;
    }

    std::string_view text_;
    size_t pos_ = 0;
};

void dump_string(std::string& out, const std::string& value) {
    out.push_back('"');
    for (unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(c));  // UTF-8 passes through
                }
        }
    }
    out.push_back('"');
}

}  // namespace

Json Json::parse(std::string_view text) {
    Parser parser(text);
    Json result;
    if (!parser.parse_value(result)) return Json();
    return result;
}

bool Json::as_bool(bool fallback) const {
    if (type_ == Type::Bool) return bool_;
    if (type_ == Type::Number) return number_ != 0.0;
    return fallback;
}

double Json::as_double(double fallback) const {
    if (type_ == Type::Number) return number_;
    if (type_ == Type::Bool) return bool_ ? 1.0 : 0.0;
    return fallback;
}

long long Json::as_int(long long fallback) const {
    if (type_ == Type::Number) return static_cast<long long>(std::llround(number_));
    if (type_ == Type::Bool) return bool_ ? 1 : 0;
    return fallback;
}

std::string Json::as_string(std::string_view fallback) const {
    if (type_ == Type::String) return string_;
    return std::string(fallback);
}

const Json& Json::operator[](std::string_view key) const {
    if (type_ != Type::Object) return null_value();
    auto it = object_.find(std::string(key));
    return it == object_.end() ? null_value() : it->second;
}

Json& Json::operator[](const std::string& key) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        object_.clear();
    }
    return object_[key];
}

bool Json::has(std::string_view key) const {
    return type_ == Type::Object && object_.count(std::string(key)) > 0;
}

void Json::erase(const std::string& key) {
    if (type_ == Type::Object) object_.erase(key);
}

const Json& Json::at(size_t index) const {
    if (type_ != Type::Array || index >= array_.size()) return null_value();
    return array_[index];
}

void Json::push_back(Json value) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        array_.clear();
    }
    array_.push_back(std::move(value));
}

size_t Json::size() const {
    if (type_ == Type::Array) return array_.size();
    if (type_ == Type::Object) return object_.size();
    return 0;
}

std::string Json::dump(int indent) const {
    std::string out;
    dump_to(out, indent, 0);
    return out;
}

void Json::dump_to(std::string& out, int indent, int depth) const {
    auto newline = [&](int level) {
        if (indent < 0) return;
        out.push_back('\n');
        out.append(static_cast<size_t>(indent * level), ' ');
    };

    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            if (number_ == std::floor(number_) && std::fabs(number_) < 1e15) {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "%lld",
                              static_cast<long long>(std::llround(number_)));
                out += buffer;
            } else {
                char buffer[40];
                std::snprintf(buffer, sizeof(buffer), "%.10g", number_);
                out += buffer;
            }
            break;
        }
        case Type::String: dump_string(out, string_); break;
        case Type::Array: {
            if (array_.empty()) { out += "[]"; break; }
            out.push_back('[');
            for (size_t i = 0; i < array_.size(); ++i) {
                if (i) out.push_back(',');
                newline(depth + 1);
                array_[i].dump_to(out, indent, depth + 1);
            }
            newline(depth);
            out.push_back(']');
            break;
        }
        case Type::Object: {
            if (object_.empty()) { out += "{}"; break; }
            out.push_back('{');
            bool first = true;
            for (const auto& [key, value] : object_) {
                if (!first) out.push_back(',');
                first = false;
                newline(depth + 1);
                dump_string(out, key);
                out.push_back(':');
                if (indent >= 0) out.push_back(' ');
                value.dump_to(out, indent, depth + 1);
            }
            newline(depth);
            out.push_back('}');
            break;
        }
    }
}

}  // namespace util
