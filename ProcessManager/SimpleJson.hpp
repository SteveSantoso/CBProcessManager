// SimpleJson.hpp - Lightweight header-only JSON parser/writer for ProcessManager
// Supports: objects, arrays, strings, booleans, numbers, null
#pragma once
#include <string>
#include <vector>
#include <map>
#include <variant>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <functional>

namespace sj {

struct Value;
using Object = std::map<std::string, Value>;
using Array  = std::vector<Value>;
using Null   = std::monostate;

struct Value {
    std::variant<Null, bool, double, std::string, Object, Array> data;

    Value()                        : data(Null{}) {}
    Value(std::nullptr_t)          : data(Null{}) {}
    Value(bool v)                  : data(v) {}
    Value(int v)                   : data((double)v) {}
    Value(long long v)             : data((double)v) {}
    Value(double v)                : data(v) {}
    Value(const char* v)           : data(std::string(v)) {}
    Value(const std::string& v)    : data(v) {}
    Value(std::string&& v)         : data(std::move(v)) {}
    Value(const Object& v)         : data(v) {}
    Value(Object&& v)              : data(std::move(v)) {}
    Value(const Array& v)          : data(v) {}
    Value(Array&& v)               : data(std::move(v)) {}

    bool is_null()   const { return std::holds_alternative<Null>(data); }
    bool is_bool()   const { return std::holds_alternative<bool>(data); }
    bool is_number() const { return std::holds_alternative<double>(data); }
    bool is_string() const { return std::holds_alternative<std::string>(data); }
    bool is_object() const { return std::holds_alternative<Object>(data); }
    bool is_array()  const { return std::holds_alternative<Array>(data); }

    bool        get_bool()   const { return std::get<bool>(data); }
    double      get_number() const { return std::get<double>(data); }
    int         get_int()    const { return (int)std::get<double>(data); }
    const std::string& get_string() const { return std::get<std::string>(data); }
    std::string& get_string()       { return std::get<std::string>(data); }
    const Object& get_object() const { return std::get<Object>(data); }
    Object&       get_object()       { return std::get<Object>(data); }
    const Array&  get_array()  const { return std::get<Array>(data); }
    Array&        get_array()        { return std::get<Array>(data); }

    // operator[] for object
    Value& operator[](const std::string& key) {
        if (is_null()) data = Object{};
        return std::get<Object>(data)[key];
    }
    const Value& operator[](const std::string& key) const {
        return std::get<Object>(data).at(key);
    }
    bool contains(const std::string& key) const {
        if (!is_object()) return false;
        return get_object().count(key) > 0;
    }
    // operator[] for array
    Value& operator[](size_t idx)       { return std::get<Array>(data)[idx]; }
    const Value& operator[](size_t idx) const { return std::get<Array>(data)[idx]; }
    size_t size() const {
        if (is_array())  return get_array().size();
        if (is_object()) return get_object().size();
        return 0;
    }
    void push_back(Value v) { std::get<Array>(data).push_back(std::move(v)); }

    // Convenience: get with default
    std::string get_string_or(const std::string& def) const {
        if (is_string()) return get_string();
        return def;
    }
    bool get_bool_or(bool def) const {
        if (is_bool()) return get_bool();
        return def;
    }
    int get_int_or(int def) const {
        if (is_number()) return get_int();
        return def;
    }
    double get_number_or(double def) const {
        if (is_number()) return get_number();
        return def;
    }
};

// ─── Serializer ─────────────────────────────────────────────────────────────
static inline std::string escapeString(const std::string& s) {
    std::ostringstream oss;
    oss << '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':  oss << "\\\""; break;
        case '\\': oss << "\\\\"; break;
        case '\n': oss << "\\n";  break;
        case '\r': oss << "\\r";  break;
        case '\t': oss << "\\t";  break;
        default:
            if (c < 0x20) oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c;
            else oss << c;
        }
    }
    oss << '"';
    return oss.str();
}

static inline std::string stringify(const Value& v, int indent = 0, int step = 2) {
    std::ostringstream oss;
    std::string pad(indent, ' ');
    std::string inner(indent + step, ' ');

    if (v.is_null())   return "null";
    if (v.is_bool())   return v.get_bool() ? "true" : "false";
    if (v.is_number()) {
        double d = v.get_number();
        if (d == (long long)d) {
            oss << (long long)d;
        } else {
            oss << std::setprecision(15) << d;
        }
        return oss.str();
    }
    if (v.is_string()) return escapeString(v.get_string());
    if (v.is_array()) {
        const auto& arr = v.get_array();
        if (arr.empty()) return "[]";
        oss << "[\n";
        for (size_t i = 0; i < arr.size(); ++i) {
            oss << inner << stringify(arr[i], indent + step, step);
            if (i + 1 < arr.size()) oss << ",";
            oss << "\n";
        }
        oss << pad << "]";
        return oss.str();
    }
    if (v.is_object()) {
        const auto& obj = v.get_object();
        if (obj.empty()) return "{}";
        oss << "{\n";
        size_t i = 0;
        for (const auto& [key, val] : obj) {
            oss << inner << escapeString(key) << ": " << stringify(val, indent + step, step);
            if (++i < obj.size()) oss << ",";
            oss << "\n";
        }
        oss << pad << "}";
        return oss.str();
    }
    return "null";
}

// ─── Parser ──────────────────────────────────────────────────────────────────
struct Parser {
    const char* p;
    const char* end;

    void skip() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    char peek() { skip(); return p < end ? *p : '\0'; }
    char next() { skip(); return p < end ? *p++ : '\0'; }
    void expect(char c) {
        if (next() != c) throw std::runtime_error(std::string("Expected '") + c + "'");
    }

    Value parseValue() {
        char c = peek();
        if (c == '"') return parseString();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == 't') { p += 4; return Value(true); }
        if (c == 'f') { p += 5; return Value(false); }
        if (c == 'n') { p += 4; return Value(nullptr); }
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
        throw std::runtime_error(std::string("Unexpected char: ") + c);
    }

    std::string parseRawString() {
        expect('"');
        std::string s;
        while (p < end && *p != '"') {
            if (*p == '\\') {
                ++p;
                switch (*p++) {
                case '"':  s += '"';  break;
                case '\\': s += '\\'; break;
                case '/':  s += '/';  break;
                case 'n':  s += '\n'; break;
                case 'r':  s += '\r'; break;
                case 't':  s += '\t'; break;
                case 'b':  s += '\b'; break;
                case 'f':  s += '\f'; break;
                case 'u': {
                    // simple 4-hex BMP decode
                    char hex[5] = {};
                    for (int i = 0; i < 4; i++) hex[i] = *p++;
                    unsigned int cp = (unsigned int)std::stoul(hex, nullptr, 16);
                    if (cp < 0x80) s += (char)cp;
                    else if (cp < 0x800) {
                        s += (char)(0xC0 | (cp >> 6));
                        s += (char)(0x80 | (cp & 0x3F));
                    } else {
                        s += (char)(0xE0 | (cp >> 12));
                        s += (char)(0x80 | ((cp >> 6) & 0x3F));
                        s += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: s += *(p-1); break;
                }
            } else {
                s += *p++;
            }
        }
        if (*p == '"') ++p;
        return s;
    }

    Value parseString() { return Value(parseRawString()); }

    Value parseNumber() {
        const char* start = p;
        if (*p == '-') ++p;
        while (p < end && *p >= '0' && *p <= '9') ++p;
        if (p < end && *p == '.') { ++p; while (p < end && *p >= '0' && *p <= '9') ++p; }
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p;
            if (*p == '+' || *p == '-') ++p;
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        return Value(std::stod(std::string(start, p)));
    }

    Value parseObject() {
        expect('{');
        Object obj;
        if (peek() == '}') { ++p; return Value(std::move(obj)); }
        while (true) {
            std::string key = parseRawString();
            expect(':');
            obj[key] = parseValue();
            char c2 = next();
            if (c2 == '}') break;
            if (c2 != ',') throw std::runtime_error("Expected ',' or '}'");
        }
        return Value(std::move(obj));
    }

    Value parseArray() {
        expect('[');
        Array arr;
        if (peek() == ']') { ++p; return Value(std::move(arr)); }
        while (true) {
            arr.push_back(parseValue());
            char c2 = next();
            if (c2 == ']') break;
            if (c2 != ',') throw std::runtime_error("Expected ',' or ']'");
        }
        return Value(std::move(arr));
    }
};

static inline Value parse(const std::string& s) {
    Parser parser{ s.c_str(), s.c_str() + s.size() };
    return parser.parseValue();
}

} // namespace sj
