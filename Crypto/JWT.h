#ifndef JWT_H
#define JWT_H

#include <string>
#include <vector>
#include "Base64.h"

class JWT {
public:
    static std::string getPayload(const std::string& token) {
        size_t firstDot = token.find('.');
        size_t secondDot = token.find('.', firstDot + 1);
        if (firstDot == std::string::npos || secondDot == std::string::npos) return "";
        std::string payloadBase64 = token.substr(firstDot + 1, secondDot - firstDot - 1);
        return Base64::decodeURL(payloadBase64);
    }

    // JSON string value extractor that correctly handles large Base64 values.
    // For string values (surrounded by "), find the closing " while skipping \".
    // For numeric values, stop at the next , ] } character.
    static std::string getJsonValue(const std::string& json, const std::string& key) {
        std::string searchKey = "\"" + key + "\":";
        size_t pos = json.find(searchKey);
        if (pos == std::string::npos) return "";
        pos += searchKey.length();
        // skip whitespace
        while (pos < json.length() && json[pos] == ' ') pos++;
        if (pos >= json.length()) return "";

        if (json[pos] == '"') {
            // String value: find closing " (respecting \")
            pos++; // skip opening "
            size_t start = pos;
            while (pos < json.length()) {
                if (json[pos] == '"' && (pos == 0 || json[pos - 1] != '\\')) break;
                pos++;
            }
            return json.substr(start, pos - start);
        } else {
            // Numeric / boolean value: stop at , ] } or whitespace
            size_t start = pos;
            while (pos < json.length() && json[pos] != ',' && json[pos] != '}' && json[pos] != ']' && json[pos] != ' ') pos++;
            return json.substr(start, pos - start);
        }
    }
};

#endif
