#ifndef JSON_H
#define JSON_H

#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <iostream>
#include <stdexcept>

namespace Util {

enum JsonType { JNULL, JSTRING, JNUMBER, JOBJECT, JARRAY, JBOOL };

struct JsonValue {
    JsonType type = JNULL;
    std::string s;
    double n = 0;
    bool b = false;
    std::map<std::string, JsonValue> obj;
    std::vector<JsonValue> arr;

    const JsonValue& operator[](const std::string& key) const {
        auto it = obj.find(key);
        if (it != obj.end()) return it->second;
        static JsonValue nullVal;
        return nullVal;
    }

    const JsonValue& operator[](size_t index) const {
        if (index < arr.size()) return arr[index];
        static JsonValue nullVal;
        return nullVal;
    }
};

class JsonParser {
public:
    static JsonValue parse(const std::string& json) {
        size_t pos = 0;
        return parseValue(json, pos);
    }

private:
    static void skipWhitespace(const std::string& json, size_t& pos) {
        while (pos < json.length() && isspace(json[pos])) pos++;
    }

    static JsonValue parseValue(const std::string& json, size_t& pos) {
        skipWhitespace(json, pos);
        if (pos >= json.length()) return { JNULL };

        char c = json[pos];
        if (c == '{') return parseObject(json, pos);
        if (c == '[') return parseArray(json, pos);
        if (c == '"') return parseString(json, pos);
        if (isdigit(c) || c == '-') return parseNumber(json, pos);
        if (json.compare(pos, 4, "true") == 0) { pos += 4; return { JBOOL, "", 0, true }; }
        if (json.compare(pos, 5, "false") == 0) { pos += 5; return { JBOOL, "", 0, false }; }
        if (json.compare(pos, 4, "null") == 0) { pos += 4; return { JNULL }; }

        return { JNULL };
    }

    static JsonValue parseString(const std::string& json, size_t& pos) {
        pos++; // skip "
        std::string s;
        while (pos < json.length() && json[pos] != '"') {
            if (json[pos] == '\\') pos++; // naive escape
            s += json[pos++];
        }
        if (pos < json.length()) pos++; // skip "
        return { JSTRING, s };
    }

    static JsonValue parseNumber(const std::string& json, size_t& pos) {
        size_t start = pos;
        while (pos < json.length() && (isdigit(json[pos]) || json[pos] == '.' || json[pos] == '-' || json[pos] == 'e' || json[pos] == 'E' || json[pos] == '+')) pos++;
        try {
            return { JNUMBER, "", std::stod(json.substr(start, pos - start)) };
        } catch (...) {
            return { JNULL };
        }
    }

    static JsonValue parseObject(const std::string& json, size_t& pos) {
        pos++; // skip {
        JsonValue val;
        val.type = JOBJECT;
        skipWhitespace(json, pos);
        while (pos < json.length() && json[pos] != '}') {
            JsonValue keyVal = parseString(json, pos);
            skipWhitespace(json, pos);
            if (pos < json.length() && json[pos] == ':') pos++;
            val.obj[keyVal.s] = parseValue(json, pos);
            skipWhitespace(json, pos);
            if (pos < json.length() && json[pos] == ',') pos++;
            skipWhitespace(json, pos);
        }
        if (pos < json.length()) pos++; // skip }
        return val;
    }

    static JsonValue parseArray(const std::string& json, size_t& pos) {
        pos++; // skip [
        JsonValue val;
        val.type = JARRAY;
        skipWhitespace(json, pos);
        while (pos < json.length() && json[pos] != ']') {
            val.arr.push_back(parseValue(json, pos));
            skipWhitespace(json, pos);
            if (pos < json.length() && json[pos] == ',') pos++;
            skipWhitespace(json, pos);
        }
        if (pos < json.length()) pos++; // skip ]
        return val;
    }
};

}

#endif
