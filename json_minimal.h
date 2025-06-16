#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <cctype>

// Minimal JSON parser for flat/nested objects and arrays (string/number/bool/null support)
class JsonValue {
public:
    enum Type { Null, Bool, Number, String, Object, Array } type = Null;
    std::string str;
    std::vector<JsonValue> arr;
    std::unordered_map<std::string, JsonValue> obj;
    bool boolean = false;
    double number = 0.0;

    JsonValue() : type(Null) {}
    JsonValue(bool b) : type(Bool), boolean(b) {}
    JsonValue(double n) : type(Number), number(n) {}
    JsonValue(const std::string& s) : type(String), str(s) {}
    static JsonValue parse(const char* &p);
    const JsonValue& operator[](const std::string& k) const { static JsonValue none; auto it = obj.find(k); return it != obj.end() ? it->second : none; }
    const JsonValue& operator[](size_t i) const { static JsonValue none; return i < arr.size() ? arr[i] : none; }
    bool is_null() const { return type == Null; }
    std::string as_str() const { return type == String ? str : ""; }
    double as_num() const { return type == Number ? number : 0; }
    bool as_bool() const { return type == Bool ? boolean : false; }
    size_t size() const { return type == Array ? arr.size() : type == Object ? obj.size() : 0; }
};

// Skip whitespace
inline void skip_ws(const char* &p) { while (std::isspace(*p)) ++p; }

// Parse a JSON string, handling escapes
inline std::string parse_json_string(const char* &p) {
    std::string s;
    if (*p++ != '"') return s;
    while (*p && *p != '"') {
        if (*p == '\\') {
            ++p;
            switch (*p) {
                case '"': s += '"'; break;
                case '\\': s += '\\'; break;
                case '/': s += '/'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'n': s += '\n'; break;
                case 'r': s += '\r'; break;
                case 't': s += '\t'; break;
                case 'u': /* ignore unicode for simplicity */ p += 4; break;
                default: break;
            }
            ++p;
        } else {
            s += *p++;
        }
    }
    ++p;
    return s;
}

inline JsonValue parse_json_number(const char* &p) {
    const char* start = p;
    if (*p == '-') ++p;
    while (std::isdigit(*p)) ++p;
    if (*p == '.') { ++p; while (std::isdigit(*p)) ++p; }
    if (*p == 'e' || *p == 'E') { ++p; if (*p == '+' || *p == '-') ++p; while (std::isdigit(*p)) ++p; }
    return JsonValue(strtod(start, nullptr));
}

inline JsonValue JsonValue::parse(const char* &p) {
    skip_ws(p);
    if (*p == '{') {
        ++p;
        JsonValue v; v.type = Object;
        skip_ws(p);
        while (*p && *p != '}') {
            skip_ws(p);
            std::string key = parse_json_string(p);
            skip_ws(p);
            if (*p == ':') ++p;
            skip_ws(p);
            v.obj[key] = parse(p);
            skip_ws(p);
            if (*p == ',') ++p;
            skip_ws(p);
        }
        if (*p == '}') ++p;
        return v;
    } else if (*p == '[') {
        ++p;
        JsonValue v; v.type = Array;
        skip_ws(p);
        while (*p && *p != ']') {
            v.arr.push_back(parse(p));
            skip_ws(p);
            if (*p == ',') ++p;
            skip_ws(p);
        }
        if (*p == ']') ++p;
        return v;
    } else if (*p == '"') {
        return JsonValue(parse_json_string(p));
    } else if (std::isdigit(*p) || *p == '-') {
        return parse_json_number(p);
    } else if (strncmp(p, "true", 4) == 0) {
        p += 4; return JsonValue(true);
    } else if (strncmp(p, "false", 5) == 0) {
        p += 5; return JsonValue(false);
    } else if (strncmp(p, "null", 4) == 0) {
        p += 4; return JsonValue();
    }
    return JsonValue();
}

// Convenience function to parse from std::string
inline JsonValue parse_json(const std::string& s) {
    const char* p = s.c_str();
    return JsonValue::parse(p);
}