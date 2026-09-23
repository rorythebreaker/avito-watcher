// A small JSON value used for settings, the Telegram API and the DevTools
// protocol. Only what this app needs: parsing, building and serialising.
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace util {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(bool value) : type_(Type::Bool), bool_(value) {}
    Json(double value) : type_(Type::Number), number_(value) {}
    Json(long long value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(int value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(std::string value) : type_(Type::String), string_(std::move(value)) {}
    Json(const char* value) : type_(Type::String), string_(value ? value : "") {}

    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    // Returns a Null value when the text is not valid JSON.
    static Json parse(std::string_view text);

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_string() const { return type_ == Type::String; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_bool() const { return type_ == Type::Bool; }

    // Readers that fall back to the given default when the value is missing or
    // has another type. Callers never have to check the type first.
    bool as_bool(bool fallback = false) const;
    double as_double(double fallback = 0.0) const;
    long long as_int(long long fallback = 0) const;
    std::string as_string(std::string_view fallback = "") const;

    // Object access. Reading an absent key yields a Null value.
    const Json& operator[](std::string_view key) const;
    Json& operator[](const std::string& key);
    bool has(std::string_view key) const;
    void erase(const std::string& key);
    const std::map<std::string, Json>& items() const { return object_; }

    // Array access.
    const Json& at(size_t index) const;
    void push_back(Json value);
    size_t size() const;

    std::string dump(int indent = -1) const;

private:
    void dump_to(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Json> array_;
    std::map<std::string, Json> object_;
};

}  // namespace util
