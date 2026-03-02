#ifndef BEDROCK_PACKETS_H
#define BEDROCK_PACKETS_H

namespace Bedrock {
    enum PacketID {
        LOGIN = 0x01,
        PLAY_STATUS = 0x02,
        SERVER_TO_CLIENT_HANDSHAKE = 0x03,
        CLIENT_TO_SERVER_HANDSHAKE = 0x04,
        DISCONNECT = 0x05,
        RESOURCE_PACKS_INFO = 0x06,
        RESOURCE_PACK_STACK = 0x07,
        RESOURCE_PACK_CLIENT_RESPONSE = 0x08,
        TEXT = 0x09,
        SET_TIME = 0x0a,
        START_GAME = 0x0b,
        ADD_PLAYER = 0x0c,
        ADD_ENTITY = 0x0d,
        REMOVE_ENTITY = 0x0e,
        CHUNK_RADIUS_UPDATED = 0x46,
        NETWORK_CHUNK_PUBLISHER_UPDATE = 0x79,
        REQUEST_NETWORK_SETTINGS = 0xc1,
        NETWORK_SETTINGS = 0x8f
    };
}

#endif
