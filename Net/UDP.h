#ifndef UDP_H
#define UDP_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>
#include "../Util/Logger.h"

#pragma comment(lib, "ws2_32.lib")

class UDP {
public:
    SOCKET sock;
    
    UDP() : sock(INVALID_SOCKET) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            throw std::runtime_error("Winsock init failed");
        }
    }

    ~UDP() {
        if (sock != INVALID_SOCKET) closesocket(sock);
        WSACleanup();
    }

    void listen(int port) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) throw std::runtime_error("Socket creation failed");

        sockaddr_in server;
        server.sin_family = AF_INET;
        server.sin_addr.s_addr = INADDR_ANY;
        server.sin_port = htons((u_short)port);

        if (bind(sock, (sockaddr*)&server, sizeof(server)) == SOCKET_ERROR) {
            throw std::runtime_error("Bind failed");
        }
        Logger::info("UDP Server listening on port " + std::to_string(port));
    }

    int recv(uint8_t* buf, int len, sockaddr_in& from) {
        int fromLen = sizeof(from);
        int res = recvfrom(sock, (char*)buf, len, 0, (sockaddr*)&from, &fromLen);
        if (res > 0 && Logger::debugEnabled) {
             // Avoid spamming too much, but for initial debugging this is helpful
             // Logger::debug("Recv raw " + std::to_string(res) + " bytes from " + inet_ntoa(from.sin_addr));
        }
        return res;
    }

    void send(const uint8_t* buf, int len, const sockaddr_in& to) {
        sendto(sock, (const char*)buf, len, 0, (const sockaddr*)&to, sizeof(to));
    }
};

#endif
