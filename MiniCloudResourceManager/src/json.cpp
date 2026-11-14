#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

Json& Json::set(const std::string& key, Json value) {
    type_ = Type::Object;
    for (auto& member : members_) {
        if (member.first == key) {
            member.second = std::move(value);
            return member.second;
        }
    }
    members_.emplace_back(key, std::move(value));
    return members_.back().second;
}

const Json* Json::find(const std::string& key) const {
    for (const auto& member : members_) {
        if (member.first == key) return &member.second;
    }
    return nullptr;
}

namespace {

void appendEscaped(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

std::string formatDouble(double v) {
    if (!std::isfinite(v)) return "null";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();  // trim trailing zeros
    if (!s.empty() && s.back() == '.') s += '0';           // keep "2.0", never "2."
    return s;
}

}  // namespace

void Json::dumpTo(std::string& out) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Int: out += std::to_string(int_); break;
        case Type::Double: out += formatDouble(double_); break;
        case Type::String: appendEscaped(out, string_); break;
        case Type::Array:
            out += '[';
            for (std::size_t i = 0; i < items_.size(); ++i) {
                if (i) out += ',';
                items_[i].dumpTo(out);
            }
            out += ']';
            break;
        case Type::Object:
            out += '{';
            for (std::size_t i = 0; i < members_.size(); ++i) {
                if (i) out += ',';
                appendEscaped(out, members_[i].first);
                out += ':';
                members_[i].second.dumpTo(out);
            }
            out += '}';
            break;
    }
}

std::string Json::dump() const {
    std::string out;
    dumpTo(out);
    return out;
}

namespace {

class Parser {
public:
    Parser(const std::string& text, std::string& error) : s_(text), error_(error) {}

    bool parseDocument(Json& out) {
        skipSpace();
        if (!parseValue(out, 0)) return false;
        skipSpace();
        if (pos_ != s_.size()) return fail("unexpected trailing characters");
        return true;
    }

private:
    bool fail(const std::string& message) {
        if (error_.empty()) error_ = message + " at offset " + std::to_string(pos_);
        return false;
    }

    void skipSpace() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\t' || s_[pos_] == '\r')) ++pos_;
    }

    bool literal(const char* word, Json value, Json& out) {
        const std::string w = word;
        if (s_.compare(pos_, w.size(), w) != 0) return fail("invalid literal");
        pos_ += w.size();
        out = std::move(value);
        return true;
    }

    bool parseValue(Json& out, int depth) {
        if (depth > 64) return fail("nesting too deep");
        if (pos_ >= s_.size()) return fail("unexpected end of input");
        const char c = s_[pos_];
        if (c == '{') return parseObject(out, depth);
        if (c == '[') return parseArray(out, depth);
        if (c == '"') {
            std::string str;
            if (!parseString(str)) return false;
            out = Json(std::move(str));
            return true;
        }
        if (c == 't') return literal("true", Json(true), out);
        if (c == 'f') return literal("false", Json(false), out);
        if (c == 'n') return literal("null", Json(), out);
        return parseNumber(out);
    }

    bool parseNumber(Json& out) {
        const std::size_t start = pos_;
        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        if (pos_ >= s_.size() || !(s_[pos_] >= '0' && s_[pos_] <= '9')) return fail("invalid number");
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        bool isDouble = false;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            isDouble = true;
            ++pos_;
            if (pos_ >= s_.size() || !(s_[pos_] >= '0' && s_[pos_] <= '9')) return fail("invalid number");
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            isDouble = true;
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (pos_ >= s_.size() || !(s_[pos_] >= '0' && s_[pos_] <= '9')) return fail("invalid number");
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        const std::string token = s_.substr(start, pos_ - start);
        if (isDouble) {
            out = Json(std::strtod(token.c_str(), nullptr));
        } else {
            if (token.size() > 18) return fail("integer too large");
            out = Json(static_cast<long long>(std::strtoll(token.c_str(), nullptr, 10)));
        }
        return true;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& value) {
        if (pos_ + 4 > s_.size()) return fail("bad \\u escape");
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = s_[pos_++];
            value <<= 4;
            if (h >= '0' && h <= '9') value |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') value |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') value |= static_cast<unsigned>(h - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }

    bool parseString(std::string& out) {
        ++pos_;  // opening quote
        while (true) {
            if (pos_ >= s_.size()) return fail("unterminated string");
            const unsigned char c = static_cast<unsigned char>(s_[pos_++]);
            if (c == '"') return true;
            if (c < 0x20) return fail("control character in string");
            if (c != '\\') {
                out += static_cast<char>(c);
                continue;
            }
            if (pos_ >= s_.size()) return fail("unterminated escape");
            const char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {  // surrogate pair
                        if (pos_ + 1 < s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            unsigned low = 0;
                            if (!hex4(low)) return false;
                            if (low < 0xDC00 || low > 0xDFFF) return fail("bad surrogate pair");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            return fail("lone surrogate");
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("invalid escape");
            }
        }
    }

    bool parseArray(Json& out, int depth) {
        ++pos_;
        out = Json::array();
        skipSpace();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
        while (true) {
            skipSpace();
            Json item;
            if (!parseValue(item, depth + 1)) return false;
            out.push(std::move(item));
            skipSpace();
            if (pos_ >= s_.size()) return fail("unterminated array");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == ']') { ++pos_; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool parseObject(Json& out, int depth) {
        ++pos_;
        out = Json::object();
        skipSpace();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
        while (true) {
            skipSpace();
            if (pos_ >= s_.size() || s_[pos_] != '"') return fail("expected string key");
            std::string key;
            if (!parseString(key)) return false;
            skipSpace();
            if (pos_ >= s_.size() || s_[pos_] != ':') return fail("expected ':'");
            ++pos_;
            skipSpace();
            Json value;
            if (!parseValue(value, depth + 1)) return false;
            out.set(key, std::move(value));
            skipSpace();
            if (pos_ >= s_.size()) return fail("unterminated object");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == '}') { ++pos_; return true; }
            return fail("expected ',' or '}'");
        }
    }

    const std::string& s_;
    std::string& error_;
    std::size_t pos_ = 0;
};

}  // namespace

bool Json::parse(const std::string& text, Json& out, std::string& error) {
    error.clear();
    Parser parser(text, error);
    Json result;
    if (!parser.parseDocument(result)) return false;
    out = std::move(result);
    return true;
}
