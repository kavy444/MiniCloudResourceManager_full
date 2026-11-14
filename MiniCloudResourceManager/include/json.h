// json.h - a tiny JSON value, parser and writer (standard library only).
// Used by the mcrm_engine executable to talk to the Python backend.
#pragma once

#include <string>
#include <utility>
#include <vector>

class Json {
public:
    enum class Type { Null, Bool, Int, Double, String, Array, Object };

    Json() = default;
    Json(bool v) : type_(Type::Bool), bool_(v) {}
    Json(int v) : type_(Type::Int), int_(v) {}
    Json(long long v) : type_(Type::Int), int_(v) {}
    Json(double v) : type_(Type::Double), double_(v) {}
    Json(const char* v) : type_(Type::String), string_(v) {}
    Json(std::string v) : type_(Type::String), string_(std::move(v)) {}

    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isInt() const { return type_ == Type::Int; }
    bool isNumber() const { return type_ == Type::Int || type_ == Type::Double; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool() const { return bool_; }
    long long asInt() const { return type_ == Type::Double ? static_cast<long long>(double_) : int_; }
    double asDouble() const { return type_ == Type::Double ? double_ : static_cast<double>(int_); }
    const std::string& asString() const { return string_; }

    // Arrays
    void push(Json value) { items_.push_back(std::move(value)); }
    const std::vector<Json>& items() const { return items_; }
    std::size_t size() const { return type_ == Type::Object ? members_.size() : items_.size(); }

    // Objects (insertion order is preserved so output is deterministic)
    Json& set(const std::string& key, Json value);
    const Json* find(const std::string& key) const;
    const std::vector<std::pair<std::string, Json>>& members() const { return members_; }

    std::string dump() const;

    // Returns false and fills `error` on malformed input. Never throws.
    static bool parse(const std::string& text, Json& out, std::string& error);

private:
    void dumpTo(std::string& out) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    long long int_ = 0;
    double double_ = 0.0;
    std::string string_;
    std::vector<Json> items_;
    std::vector<std::pair<std::string, Json>> members_;
};
