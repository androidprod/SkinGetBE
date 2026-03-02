#ifndef RAKNET_H
#define RAKNET_H

#include <vector>
#include <cstdint>
#include <string>
#include "../Util/Buffer.h"

namespace RakNet {
    const uint8_t MAGIC[] = { 0x00, 0xff, 0xff, 0x00, 0xfe, 0xfe, 0xfe, 0xfe, 0xfd, 0xfd, 0xfd, 0xfd, 0x12, 0x34, 0x56, 0x78 };

    enum PacketID {
        UNCONNECTED_PING = 0x01,
        UNCONNECTED_PONG = 0x1c,
        OPEN_CONNECTION_REQUEST_1 = 0x05,
        OPEN_CONNECTION_REPLY_1 = 0x06,
        OPEN_CONNECTION_REQUEST_2 = 0x07,
        OPEN_CONNECTION_REPLY_2 = 0x08,
        CONNECTION_REQUEST = 0x09,
        CONNECTION_REQUEST_ACCEPTED = 0x10,
        NEW_INCOMING_CONNECTION = 0x13,
        CONNECTED_PING = 0x00,
        CONNECTED_PONG = 0x03,
        FRAME_SET_PACKET_BEGIN = 0x80,
        FRAME_SET_PACKET_END = 0x8d,
        ACK = 0xc0,
        NACK = 0xa0
    };

    struct ServerInfo {
        std::string motd = "SkinGetBE Research Server";
        std::string subMotd = "C++ Implementation";
        int protocol = 662; // 1.20.70+
        std::string version = "1.20.70";
        int players = 0;
        int maxPlayers = 100;
        uint64_t guid = 1234567890;
    };
}

#endif
