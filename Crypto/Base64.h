#ifndef BASE64_H
#define BASE64_H

#include <string>
#include <cstdint>

class Base64 {
    // Standard base64 alphabet
    static int decodeChar(char c) {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1; // padding or invalid
    }

public:
    // Standard base64 decode (handles = padding and ignores invalid chars gracefully)
    static std::string decode(const std::string& in) {
        std::string out;
        out.reserve(in.size() * 3 / 4);
        uint32_t buf = 0;
        int bits = 0;
        for (unsigned char c : in) {
            if (c == '=') break; // end of data
            int val = decodeChar((char)c);
            if (val < 0) continue; // skip whitespace / unknown chars
            buf = (buf << 6) | (uint32_t)val;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out += (char)((buf >> bits) & 0xFF);
            }
        }
        return out;
    }

    // base64url decode (JWT format: '-' → '+', '_' → '/', no padding required)
    static std::string decodeURL(std::string in) {
        for (auto& c : in) {
            if (c == '-') c = '+';
            if (c == '_') c = '/';
        }
        // Padding is optional in base64url; our decode() handles missing padding fine
        // because it stops at '=' and the bit accumulator flushes naturally.
        return decode(in);
    }
};

#endif
