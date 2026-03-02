#ifndef BUFFER_H
#define BUFFER_H

#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <algorithm>

class Buffer {
public:
    std::vector<uint8_t> data;
    size_t offset = 0;

    Buffer() {}
    Buffer(const uint8_t* buf, size_t len) : data(buf, buf + len) {}
    Buffer(const std::vector<uint8_t>& d) : data(d) {}

    // Readers
    uint8_t readByte() {
        if (offset + 1 > data.size()) throw std::runtime_error("Buffer overflow");
        return data[offset++];
    }

    uint16_t readShort() {
        if (offset + 2 > data.size()) throw std::runtime_error("Buffer overflow");
        uint16_t val = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        return val;
    }

    uint16_t readLShort() {
        if (offset + 2 > data.size()) throw std::runtime_error("Buffer overflow");
        uint16_t val = data[offset] | (data[offset + 1] << 8);
        offset += 2;
        return val;
    }

    uint32_t readLInt() {
        if (offset + 4 > data.size()) throw std::runtime_error("Buffer overflow");
        uint32_t val = data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16) | (data[offset + 3] << 24);
        offset += 4;
        return val;
    }

    uint32_t readLTriad() {
        if (offset + 3 > data.size()) throw std::runtime_error("Buffer overflow");
        uint32_t val = data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16);
        offset += 3;
        return val;
    }

    int64_t readLong() {
        if (offset + 8 > data.size()) throw std::runtime_error("Buffer overflow");
        int64_t val = 0;
        for (int i = 0; i < 8; ++i) val = (val << 8) | data[offset++];
        return val;
    }

    int64_t readLLong() {
        if (offset + 8 > data.size()) throw std::runtime_error("Buffer overflow");
        int64_t val = 0;
        for (int i = 0; i < 8; ++i) val |= (static_cast<int64_t>(data[offset++]) << (i * 8));
        return val;
    }

    std::vector<uint8_t> readRemaining() {
        std::vector<uint8_t> res(data.begin() + offset, data.end());
        offset = data.size();
        return res;
    }

    // Writers
    void writeByte(uint8_t v) { data.push_back(v); }
    
    void writeShort(uint16_t v) {
        data.push_back((v >> 8) & 0xFF);
        data.push_back(v & 0xFF);
    }

    void writeLShort(uint16_t v) {
        data.push_back(v & 0xFF);
        data.push_back((v >> 8) & 0xFF);
    }

    uint32_t readInt() {
        if (offset + 4 > data.size()) throw std::runtime_error("Buffer overflow");
        uint32_t val = (data[offset] << 24) | (data[offset + 1] << 16) | (data[offset + 2] << 8) | data[offset + 3];
        offset += 4;
        return val;
    }

    float readFloat() {
        uint32_t i = readInt();
        float f;
        std::memcpy(&f, &i, 4);
        return f;
    }

    void writeInt(uint32_t v) {
        data.push_back((v >> 24) & 0xFF);
        data.push_back((v >> 16) & 0xFF);
        data.push_back((v >> 8) & 0xFF);
        data.push_back(v & 0xFF);
    }

    void writeFloat(float v) {
        uint32_t i;
        std::memcpy(&i, &v, 4);
        writeInt(i);
    }

    void writeLFloat(float v) {
        uint32_t i;
        std::memcpy(&i, &v, 4);
        writeLInt(i);
    }

    void writeLInt(uint32_t v) {
        data.push_back(v & 0xFF);
        data.push_back((v >> 8) & 0xFF);
        data.push_back((v >> 16) & 0xFF);
        data.push_back((v >> 24) & 0xFF);
    }

    void writeLTriad(uint32_t v) {
        data.push_back(v & 0xFF);
        data.push_back((v >> 8) & 0xFF);
        data.push_back((v >> 16) & 0xFF);
    }

    void writeLong(int64_t v) {
        for (int i = 7; i >= 0; --i) data.push_back((v >> (i * 8)) & 0xFF);
    }
    
    void writeBool(bool v) {
        data.push_back(v ? 1 : 0);
    }

    void writeLLong(int64_t v) {
        for (int i = 0; i < 8; ++i) data.push_back((v >> (i * 8)) & 0xFF);
    }

    void writeBuffer(const std::vector<uint8_t>& v) {
        data.insert(data.end(), v.begin(), v.end());
    }

    // VarInt helpers
    uint32_t readVarInt() {
        uint32_t val = 0;
        int shift = 0;
        while (true) {
            uint8_t b = readByte();
            val |= (b & 0x7f) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
        }
        return val;
    }

    int32_t readSignedVarInt() {
        uint32_t u = readVarInt();
        return (int32_t)(u >> 1) ^ -(int32_t)(u & 1);
    }

    void writeSignedVarInt(int32_t v) {
        writeVarInt((v << 1) ^ (v >> 31));
    }

    void writeVarInt(uint32_t v) {
        while (v >= 0x80) {
            writeByte((v & 0x7F) | 0x80);
            v >>= 7;
        }
        writeByte(v & 0x7F);
    }

    // String helpers (Length-prefixed for RakNet/Bedrock)
    std::string readString() {
        uint16_t len = readShort();
        if (offset + len > data.size()) throw std::runtime_error("Buffer overflow");
        std::string s((const char*)&data[offset], len);
        offset += len;
        return s;
    }

    void writeString(const std::string& s) {
        writeShort((uint16_t)s.length());
        data.insert(data.end(), s.begin(), s.end());
    }

    std::string readVarString() {
        uint32_t len = readVarInt();
        if (offset + len > data.size()) throw std::runtime_error("Buffer overflow");
        std::string s((const char*)&data[offset], len);
        offset += len;
        return s;
    }

    void writeVarString(const std::string& s) {
        writeVarInt((uint32_t)s.length());
        data.insert(data.end(), s.begin(), s.end());
    }
};

#endif
