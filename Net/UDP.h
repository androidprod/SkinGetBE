#ifndef UDP_H
#define UDP_H

#ifdef _WIN32
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <cstring>
    #define SOCKET int
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
    #define closesocket close
#endif

#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>
#include "../Util/Logger.h"

class UDP {
public:
    SOCKET sock;
    
    UDP() : sock(INVALID_SOCKET) {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            throw std::runtime_error("Winsock init failed");
        }
#endif
    }

    ~UDP() {
        if (sock != INVALID_SOCKET) closesocket(sock);
#ifdef _WIN32
        WSACleanup();
#endif
    }

    void listen(int port) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) throw std::runtime_error("Socket creation failed");

        sockaddr_in server;
        memset(&server, 0, sizeof(server));
        server.sin_family = AF_INET;
        server.sin_addr.s_addr = INADDR_ANY;
        server.sin_port = htons((uint16_t)port);

        if (bind(sock, (sockaddr*)&server, sizeof(server)) == SOCKET_ERROR) {
            throw std::runtime_error("Bind failed");
        }
        Logger::info("UDP Server listening on port " + std::to_string(port));
    }

    int recv(uint8_t* buf, int len, sockaddr_in& from) {
        socklen_t fromLen = sizeof(from);
        int res = recvfrom(sock, (char*)buf, len, 0, (sockaddr*)&from, &fromLen);
        if (res > 0 && Logger::debugEnabled) {
             // Logger::debug("Recv raw " + std::to_string(res) + " bytes from " + inet_ntoa(from.sin_addr));
        }
        return res;
    }

    void send(const uint8_t* buf, int len, const sockaddr_in& to) {
        sendto(sock, (const char*)buf, len, 0, (const sockaddr*)&to, sizeof(to));
    }
};

#endif
