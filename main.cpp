#include <iostream>
#include <vector>
#include <fstream>
#include <map>
#include <cstring>
#include <cstdio>
#ifdef _WIN32
    #include <windows.h>
    #include <direct.h>   // _mkdir on Windows
#else
    #include <sys/stat.h> // mkdir on Linux
    #include <sys/types.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <limits.h>
    #define _mkdir(name) mkdir(name, 0777)
#endif
#include <zlib.h>
#include "Net/UDP.h"
#include "RakNet/RakNet.h"
#include "Bedrock/Packets.h"
#include "Crypto/JWT.h"
#include "Util/Buffer.h"
#include "Util/Logger.h"
#include "Util/Json.h"
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <functional>
#include <chrono>
#include <memory>
#include <future>
#include <ctime>
#include <optional>

// zlib raw deflate decompression (BE 1.20.30+ batch format)
// algo byte 0x00 竊・raw deflate (no zlib 0x78 header)
static std::vector<uint8_t> zlibDecompress(const uint8_t* src, size_t srcLen) {
    std::vector<uint8_t> out;
    out.resize(srcLen * 4 + 1024);
    z_stream zs{};
    zs.next_in  = (Bytef*)src;
    zs.avail_in = (uInt)srcLen;
    if (inflateInit2(&zs, -15) != Z_OK)
        throw std::runtime_error("inflateInit2 failed");
    int ret;
    do {
        zs.next_out  = (Bytef*)out.data() + zs.total_out;
        zs.avail_out = (uInt)(out.size() - zs.total_out);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret == Z_DATA_ERROR || ret == Z_MEM_ERROR || ret == Z_STREAM_ERROR) {
            inflateEnd(&zs);
            throw std::runtime_error("inflate error: " + std::to_string(ret));
        }
        if (zs.avail_out == 0) out.resize(out.size() * 2);
    } while (ret != Z_STREAM_END && zs.avail_in > 0);
    size_t finalSize = zs.total_out;
    inflateEnd(&zs);
    out.resize(finalSize);
    return out;
}

// zlib raw deflate compression (for outgoing Bedrock batches)
// Produces raw deflate output (no zlib header/trailer)
static std::vector<uint8_t> zlibCompress(const uint8_t* src, size_t srcLen, int level = 7) {
    uLongf bound = compressBound((uLong)srcLen) + 64;
    std::vector<uint8_t> tmp(bound);
    z_stream zs{};
    zs.next_in   = (Bytef*)src;
    zs.avail_in  = (uInt)srcLen;
    zs.next_out  = (Bytef*)tmp.data();
    zs.avail_out = (uInt)bound;
    // windowBits = -15: raw deflate, no header
    if (deflateInit2(&zs, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw std::runtime_error("deflateInit2 failed");
    if (deflate(&zs, Z_FINISH) != Z_STREAM_END) {
        deflateEnd(&zs);
        throw std::runtime_error("deflate failed");
    }
    size_t compSize = zs.total_out;
    deflateEnd(&zs);
    tmp.resize(compSize);
    return tmp;
}

// 繝溘ル繝槭ΝPNG繧ｨ繝ｳ繧ｳ繝ｼ繝繝ｼ (zlib縺ｮcompress2繧貞茜逕ｨ)
static void writePNG(const std::string& path,
                     const uint8_t* rgba, int w, int h) {
    // 蜷・・ｽ・ｽ繧ｭ繝｣繝ｳ繝ｩ繧､繝ｳ縺ｫ filter byte 0 (繝輔ぅ繝ｫ繧ｿ縺ｪ縺・ 繧剃ｻ倅ｸ・
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(1 + w * 4) * h);
    for (int y = 0; y < h; y++) {
        raw.push_back(0); // filter type: None
        raw.insert(raw.end(), rgba + y * w * 4, rgba + y * w * 4 + w * 4);
    }
    // zlib compress (standard header, level 6)
    uLongf compLen = compressBound((uLong)raw.size());
    std::vector<uint8_t> comp(compLen);
    if (compress2(comp.data(), &compLen, raw.data(), (uLong)raw.size(), 6) != Z_OK)
        throw std::runtime_error("PNG compress failed");
    comp.resize(compLen);

    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open " + path);

    // BE uint32 writer
    auto w32 = [&](uint32_t v) {
        f.put((v>>24)&0xFF); f.put((v>>16)&0xFF);
        f.put((v>> 8)&0xFF); f.put( v     &0xFF);
    };
    // PNG chunk writer
    auto chunk = [&](const char* type, const uint8_t* data, size_t len) {
        w32((uint32_t)len);
        f.write(type, 4);
        if (len) f.write((char*)data, len);
        uLong crc = crc32(crc32(0L, (const Bytef*)type, 4),
                          data ? (const Bytef*)data : (const Bytef*)type,
                          (uInt)len);
        w32((uint32_t)crc);
    };

    // PNG signature
    const uint8_t sig[] = {137,80,78,71,13,10,26,10};
    f.write((char*)sig, 8);

    // IHDR
    uint8_t ihdr[13] = {
        (uint8_t)(w>>24),(uint8_t)(w>>16),(uint8_t)(w>>8),(uint8_t)w,
        (uint8_t)(h>>24),(uint8_t)(h>>16),(uint8_t)(h>>8),(uint8_t)h,
        8, 6, 0, 0, 0  // bit-depth=8, colorType=6(RGBA), compression/filter/interlace
    };
    chunk("IHDR", ihdr, 13);

    // IDAT
    chunk("IDAT", comp.data(), comp.size());

    // IEND
    chunk("IEND", nullptr, 0);
}

using namespace RakNet;

std::string fallbackVersion = "";
uint16_t fallbackProtocol = 0;
uint16_t serverPort = 19132;
std::string exeDir = ".";
bool useConfig = true; // Enabled by default now

static std::string getExecutableDir() {
    char buffer[1024];
#ifdef _WIN32
    GetModuleFileNameA(NULL, buffer, sizeof(buffer));
#else
    ssize_t count = readlink("/proc/self/exe", buffer, sizeof(buffer));
    if (count <= 0) return ".";
    buffer[count] = '\0';
#endif
    std::string path(buffer);
    size_t pos = path.find_last_of("\\/");
    return (pos != std::string::npos) ? path.substr(0, pos) : ".";
}

// PrismarineJS/minecraft-data の data/bedrock/version.json をもとに生成
// 同一プロトコル番号に複数バージョンが対応する場合は "/" で結合
static std::map<uint32_t, std::string> BEDROCK_PROTOCOL_MAP = {
    {70,  "0.14.3"},
    {82,  "0.15.6"},
    {100, "1.0.0"},
    {422, "1.16.201"},
    {428, "1.16.210"},
    {431, "1.16.220"},
    {440, "1.17.0"},
    {448, "1.17.10"},
    {465, "1.17.30"},
    {471, "1.17.40"},
    {475, "1.18.0"},
    {486, "1.18.11"},
    {503, "1.18.30"},
    {527, "1.19.1"},
    {534, "1.19.10"},
    {544, "1.19.20"},
    {545, "1.19.21"},
    {554, "1.19.30"},
    {557, "1.19.40"},
    {560, "1.19.50"},
    {567, "1.19.60"},
    {568, "1.19.63"},
    {575, "1.19.70"},
    {582, "1.19.80"},
    {589, "1.20.0"},
    {594, "1.20.10"},
    {618, "1.20.30"},
    {622, "1.20.40"},
    {630, "1.20.50"},
    {649, "1.20.61"},
    {662, "1.20.71"},
    {671, "1.20.80"},
    {685, "1.21.0"},
    {686, "1.21.2"},
    {712, "1.21.20"},
    {729, "1.21.30"},
    {748, "1.21.42"},
    {766, "1.21.50"},
    {776, "1.21.60"},
    {786, "1.21.70"},
    {800, "1.21.80"},
    {818, "1.21.90"},
    {819, "1.21.93"},
    {827, "1.21.100"},
    {844, "1.21.111"},
    {859, "1.21.120"},
    {860, "1.21.124"},
    {898, "1.21.130"},
    {924, "1.26.0"},
};

static std::string resolveVersion(uint32_t protocol) {
    auto it = BEDROCK_PROTOCOL_MAP.find(protocol);
    if (it != BEDROCK_PROTOCOL_MAP.end()) return it->second;
    Logger::warn("Unknown protocol: " + std::to_string(protocol) + ", using fallback");
    return fallbackVersion;
}

void loadConfig() {
    // 1. Set initial defaults from protocol map
    if (!BEDROCK_PROTOCOL_MAP.empty()) {
        auto newest = BEDROCK_PROTOCOL_MAP.rbegin();
        fallbackProtocol = (uint16_t)newest->first;
        fallbackVersion  = newest->second;
    }

    std::string configPath = exeDir + "/config.json";
    
    // 2. If config doesn't exist, create it with current defaults
    std::ifstream check(configPath);
    if (!check.good()) {
        Logger::info("Generating default config.json at: " + configPath);
        std::ofstream out(configPath);
        if (out) {
            out << "{\n";
            out << "  \"version\": \"" << fallbackVersion << "\",\n";
            out << "  \"protocol\": " << fallbackProtocol << ",\n";
            out << "  \"port\": " << serverPort << "\n";
            out << "}\n";
            out.close();
        } else {
            Logger::error("Failed to create default config.json!");
        }
    } else {
        check.close();
    }

    // 3. Load from config.json
    std::ifstream in(configPath);
    if (in.good()) {
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        
        try {
            Util::JsonValue json = Util::JsonParser::parse(content);
            if (json.type == Util::JOBJECT) {
                if (json.obj.count("version") && json.obj["version"].type == Util::JSTRING) {
                    fallbackVersion = json.obj["version"].s;
                }
                if (json.obj.count("protocol") && json.obj["protocol"].type == Util::JNUMBER) {
                    fallbackProtocol = (uint16_t)json.obj["protocol"].n;
                }
                if (json.obj.count("port") && json.obj["port"].type == Util::JNUMBER) {
                    serverPort = (uint16_t)json.obj["port"].n;
                }
            }
        } catch (const std::exception& e) {
            Logger::warn("Failed to parse config.json: " + std::string(e.what()));
        }
    }
    Logger::info("Configuration Loaded:");
    Logger::info(" - Version:  " + fallbackVersion);
    Logger::info(" - Protocol: " + std::to_string(fallbackProtocol));
    Logger::info(" - Port:     " + std::to_string(serverPort));
}

void showHelp() {
    Logger::info("--- SkinGetBE Help ---");
    Logger::info("Usage: SkinGetBE.exe [options]");
    Logger::info("Options:");
    Logger::info("  --debug             Enable verbose debug logging");
    Logger::info("  --config            Enable loading version/protocol from config.json");
    Logger::info("  --filter <ip>       Filter communications to only specified IP address");
    Logger::info("Example: SkinGetBE.exe --filter 127.0.0.1 --debug");
    Logger::info("----------------------");
}

uint32_t handleLogin(Buffer& buf) {
    Logger::info("Handling Bedrock Login Packet...");
    try {
        struct LoginFields {
            uint32_t protocol = 0;
            std::string chainJson;
            std::string skinJWT;
        };
        auto tryParseLogin = [](const Buffer& source, bool protocolVarInt, bool hasPayloadLength) -> std::optional<LoginFields> {
            Buffer tmp(source);
            LoginFields out;
            try {
                out.protocol = protocolVarInt ? tmp.readVarInt() : tmp.readInt();
                if (hasPayloadLength) (void)tmp.readVarInt();
                uint32_t chainLen = tmp.readLInt();
                if (chainLen == 0 || tmp.offset + chainLen > tmp.data.size()) return std::nullopt;
                out.chainJson.assign((const char*)tmp.data.data() + tmp.offset, chainLen);
                tmp.offset += chainLen;
                uint32_t skinLen = tmp.readLInt();
                if (skinLen == 0 || tmp.offset + skinLen > tmp.data.size()) return std::nullopt;
                out.skinJWT.assign((const char*)tmp.data.data() + tmp.offset, skinLen);
                if (out.chainJson.find('{') == std::string::npos) return std::nullopt;
                if (std::count(out.skinJWT.begin(), out.skinJWT.end(), '.') != 2) return std::nullopt;
                return out;
            } catch (...) {
                return std::nullopt;
            }
        };
        std::optional<LoginFields> parsed = tryParseLogin(buf, false, false);
        if (!parsed.has_value()) parsed = tryParseLogin(buf, true, false);
        if (!parsed.has_value()) parsed = tryParseLogin(buf, false, true);
        if (!parsed.has_value()) parsed = tryParseLogin(buf, true, true);
        if (!parsed.has_value()) return 0;
        Logger::info("Client Protocol: " + std::to_string(parsed->protocol));
        std::string playerName = "UnknownPlayer";
        size_t searchPos = 0;
        while ((searchPos = parsed->chainJson.find("\"", searchPos)) != std::string::npos) {
            size_t jwtStart = searchPos + 1;
            size_t jwtEnd = parsed->chainJson.find("\"", jwtStart);
            if (jwtEnd == std::string::npos) break;
            std::string token = parsed->chainJson.substr(jwtStart, jwtEnd - jwtStart);
            searchPos = jwtEnd + 1;
            if (token.length() > 50 && std::count(token.begin(), token.end(), '.') == 2) {
                std::string payload = JWT::getPayload(token);
                if (!payload.empty()) {
                    std::string name = JWT::getJsonValue(payload, "xname");
                    if (name.empty()) name = JWT::getJsonValue(payload, "ThirdPartyName");
                    if (name.empty()) name = JWT::getJsonValue(payload, "displayName");
                    if (!name.empty()) playerName = name;
                }
            }
        }
        Logger::info("Detected Player: " + playerName);
        std::string skinJson = JWT::getPayload(parsed->skinJWT);
        if (skinJson.empty()) return 0;
        std::string skinId = JWT::getJsonValue(skinJson, "SkinId");
        std::string skinImage = JWT::getJsonValue(skinJson, "SkinData");
        if (skinImage.empty()) return 0;
        std::string skinRaw = Base64::decode(skinImage);
        if (skinRaw.empty() || (skinRaw.size() % 4) != 0) return 0;
        std::string widthStr = JWT::getJsonValue(skinJson, "SkinImageWidth");
        std::string heightStr = JWT::getJsonValue(skinJson, "SkinImageHeight");
        size_t pixels = skinRaw.size() / 4;
        int imgW = 0;
        int imgH = 0;
        try {
            if (!widthStr.empty()) imgW = std::stoi(widthStr);
            if (!heightStr.empty()) imgH = std::stoi(heightStr);
        } catch (...) {
            imgW = 0;
            imgH = 0;
        }
        if (imgW <= 0 || imgH <= 0 || (size_t)imgW * (size_t)imgH != pixels) {
            if      (pixels == 64 * 32)   { imgW = 64;  imgH = 32;  }
            else if (pixels == 64 * 64)   { imgW = 64;  imgH = 64;  }
            else if (pixels == 128 * 64)  { imgW = 128; imgH = 64;  }
            else if (pixels == 128 * 128) { imgW = 128; imgH = 128; }
            else {
                imgW = 64;
                imgH = (int)(pixels / 64);
                if (imgH <= 0) imgH = 64;
            }
        }
        auto sanitize = [](std::string s) {
            for (auto& c : s) if (c=='/' || c=='\\' || c==':' || c=='*' || c=='?' || c=='\"' || c=='<' || c=='>' || c=='|') c='_';
            return s;
        };
        playerName = sanitize(playerName);
        skinId = sanitize(skinId);
        
        std::string skinsDir = exeDir + "/skins";
        _mkdir(skinsDir.c_str());
        
        std::string baseName = playerName + "_" + skinId;
        std::string targetPath = skinsDir + "/" + baseName + ".png";
        int count = 1;
        while (true) {
            std::ifstream f(targetPath);
            if (!f.good()) break;
            targetPath = skinsDir + "/" + baseName + "_" + std::to_string(count++) + ".png";
        }
        writePNG(targetPath, (const uint8_t*)skinRaw.data(), imgW, imgH);
        Logger::success(">>> Saved PNG: " + targetPath + " (" + std::to_string(imgW) + "x" + std::to_string(imgH) + ")");
        return parsed->protocol;
    } catch (const std::exception& e) {
        Logger::error("Failed to parse login: " + std::string(e.what()));
    }
    return 0;
}

struct SplitPacket {
    uint32_t count        = 0;
    uint32_t receivedCount = 0;
    std::vector<std::vector<uint8_t>> fragments;
    std::vector<bool>                 received;
};

    struct ClientSession {
    uint32_t packetSeq    = 0;
    uint32_t reliableSeq  = 0;
    uint32_t orderIndex   = 0;
    std::map<uint16_t, SplitPacket> splitPackets;
    sockaddr_in addr;
    std::mutex mtx;

    uint8_t  raknetProtocol  = 11;
    uint32_t bedrockProtocol = 0;
    std::string bedrockVersion = "";
};

class ThreadPool {
public:
    ThreadPool(size_t threads) : stop(false) {
        for(size_t i = 0; i<threads; ++i)
            workers.emplace_back([this]{
                for(;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this]{ return this->stop || !this->tasks.empty(); });
                        if(this->stop && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
            });
    }
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type> {
        using return_type = typename std::result_of<F(Args...)>::type;
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if(stop) throw std::runtime_error("enqueue on stopped ThreadPool");
            tasks.emplace([task](){ (*task)(); });
        }
        condition.notify_one();
        return res;
    }
    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for(std::thread &worker: workers) worker.join();
    }
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};

// NAT Discovery via STUN using the provided server socket
static std::string discoverExternalIP(SOCKET serverSock) {
    try {
        std::vector<std::pair<std::string, int>> servers = {
            {"stun.l.google.com", 19302},
            {"stun1.l.google.com", 19302},
            {"stun.sipgate.net", 3478}
        };

        for (auto& s : servers) {
            sockaddr_in stunAddr;
            memset(&stunAddr, 0, sizeof(stunAddr));
            stunAddr.sin_family = AF_INET;
            stunAddr.sin_port = htons(s.second);
            struct hostent* host = gethostbyname(s.first.c_str());
            if (!host) continue;
            memcpy(&stunAddr.sin_addr, host->h_addr_list[0], host->h_length);

            uint8_t request[20] = {0,1, 0,0, 0x21,0x12,0xA4,0x42};
            for(int i=8; i<20; i++) request[i] = (uint8_t)(rand() % 256);

            sendto(serverSock, (const char*)request, 20, 0, (const sockaddr*)&stunAddr, sizeof(stunAddr));

            uint8_t response[512];
            sockaddr_in from;
            socklen_t fromLen = sizeof(from);
            
            #ifdef _WIN32
            int timeout = 1500;
            setsockopt(serverSock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
            #endif

            int len = recvfrom(serverSock, (char*)response, sizeof(response), 0, (sockaddr*)&from, &fromLen);
            
            // Set back to no timeout (or a smaller one for game loop)
            #ifdef _WIN32
            int noTimeout = 0;
            setsockopt(serverSock, SOL_SOCKET, SO_RCVTIMEO, (char*)&noTimeout, sizeof(noTimeout));
            #endif

            if (len >= 20) {
                int pos = 20;
                while (pos + 4 <= len) {
                    uint16_t attrType = (response[pos] << 8) | response[pos+1];
                    uint16_t attrLen  = (response[pos+2] << 8) | response[pos+3];
                    pos += 4;
                    if (attrType == 0x0020 && attrLen >= 8) { // XOR-MAPPED-ADDRESS
                        uint32_t ip = ((uint32_t)response[pos+4] << 24) | ((uint32_t)response[pos+5] << 16) |
                                      ((uint32_t)response[pos+6] << 8) | (uint32_t)response[pos+7];
                        ip ^= 0x2112A442;
                        char buf[16];
                        sprintf(buf, "%d.%d.%d.%d", (ip>>24)&0xFF, (ip>>16)&0xFF, (ip>>8)&0xFF, ip&0xFF);
                        return std::string(buf);
                    }
                    pos += attrLen;
                }
            }
        }
    } catch (...) {}
    return "Unknown (Maybe behind strict NAT)";
}

int main(int argc, char* argv[]) {
    srand((unsigned int)time(nullptr));
    Logger::init();
    
    // Initialize executable directory
    exeDir = getExecutableDir();

    std::cout << "\033[1;36m" << R"(
   _____ _    _      _____      _   ____  ______ 
  / ____| |  (_)    / ____|    | | |  _ \|  ____|
  | (___ | | ___ _ _| |  __  ___| |_| |_) | |__   
   \___ \| |/ / | '_ \ | |_ |/ _ \ __|  _ <|  __|  
   ____) |   <| | | | | |__| |  __/ |_| |_) | |____ 
  |_____/|_|\_\_|_| |_|\_____|\___|\__|____/|______|
                                                   
    )" << "\033[0m" << std::endl;
    Logger::info("SkinGetBE starting up (Multi-threaded)...");
    Logger::info("Working Directory (Exe): " + exeDir);

    std::string filterIp = "";
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--debug" || arg == "-d" || arg == "/debug") {
            Logger::debugEnabled = true;
        } else if (arg == "--config" || arg == "-c" || arg == "/config") {
            useConfig = true;
        } else if (arg == "--filter" && i + 1 < argc) {
            filterIp = argv[++i];
        }
    }

    loadConfig();

    showHelp();
    
    UDP server;
    try {
        server.listen(serverPort);
    } catch (const std::exception& e) {
        Logger::error(e.what());
        return 1;
    }

    // NAT Traversal / Discovery
    Logger::info("Attempting NAT discovery (STUN)...");
    std::string extIP = discoverExternalIP(server.sock);
    Logger::success("External IP detected: " + extIP);
    Logger::info("Note: Ensure UDP Port " + std::to_string(serverPort) + " is open on your router if not reachable.");

    if (!filterIp.empty()) {
        Logger::info("Filter rules applied: " + filterIp);
    }

    ThreadPool pool(std::thread::hardware_concurrency());
    std::map<std::string, std::shared_ptr<ClientSession>> sessions;
    std::mutex sessionMtx;

    // IP Cache for protocol detection across reconnections
    std::map<std::string, uint32_t> ipProtocolCache;
    std::mutex ipCacheMtx;

    auto getSession = [&](const sockaddr_in& addr) -> std::shared_ptr<ClientSession> {
        char key[64];
        sprintf(key, "%s:%d", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
        std::lock_guard<std::mutex> lock(sessionMtx);
        if (sessions.find(key) == sessions.end()) {
            auto s = std::make_shared<ClientSession>();
            s->addr = addr;
            sessions[key] = s;
            Logger::info("New Session: " + std::string(key));
            return s;
        }
        return sessions[key];
    };

    uint8_t recvBuf[4096];
    sockaddr_in clientAddr;

    while (true) {
        Logger::debug("Waiting for packet...");
        int len = server.recv(recvBuf, sizeof(recvBuf), clientAddr);
        if (len > 0) {
            char hexId[8]; sprintf(hexId, "%02X", (uint8_t)recvBuf[0]);
            char expectedId[8]; sprintf(expectedId, "%02X", (uint8_t)0x01); // RakNet::UNCONNECTED_PING
            Logger::debug("Received len=" + std::to_string(len) + " from " + inet_ntoa(clientAddr.sin_addr) 
                        + " [PacketID=0x" + std::string(hexId) + " expected=0x" + std::string(expectedId) + "]");
        }
        if (len <= 0) continue;

        std::string clientIpStr = inet_ntoa(clientAddr.sin_addr);
        if (!filterIp.empty() && clientIpStr != filterIp) continue;

        // Immediate MOTD response in main loop to prevent server list timeout
        if (len > 0 && (uint8_t)recvBuf[0] == 0x01) { // 0x01 = UNCONNECTED_PING
            Logger::debug("PING handler triggered!");
            try {
                Buffer buf(std::vector<uint8_t>(recvBuf, recvBuf + len));
                buf.readByte(); // packetId

                if (buf.data.size() < 1 + 8 + 16) {
                    Logger::warn("UNCONNECTED_PING too short, skipping");
                    continue;
                }

                uint64_t clientTime = buf.readLong();
                buf.offset += 16; // magic
                uint64_t serverGuid = 0x1234567812345678;

                uint32_t proto = fallbackProtocol;
                std::string ver = fallbackVersion;
                {
                    std::lock_guard<std::mutex> lock(ipCacheMtx);
                    auto it = ipProtocolCache.find(clientIpStr);
                    if (it != ipProtocolCache.end()) {
                        proto = it->second;
                        ver = resolveVersion(proto);
                    }
                }

                // Dynamic MOTD Cycling (5 seconds interval)
                uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();
                int stage = (int)((now / 5) % 3);
                std::string motdTitle;
                switch (stage) {
                    // case 0:  motdTitle = "SkinGetBE / Version: " + ver; break;
                    // case 1:  motdTitle = "SkinGetBE / Protocol: " + std::to_string(proto); break;
                    case 0:  motdTitle = "Skin acquisition software!!"; break;
                    case 1:  motdTitle = "SkinGetBE"; break;
                    default: motdTitle = "Created by androidprod"; break;
                }

                Buffer res;
                res.writeByte(RakNet::UNCONNECTED_PONG);
                res.writeLong(clientTime); res.writeLong(serverGuid);
                res.writeBuffer(std::vector<uint8_t>(MAGIC, MAGIC + 16));

                std::string portStr = std::to_string(serverPort);
                std::string motd = "MCPE;" + motdTitle + ";" + std::to_string(proto) + ";" + ver + ";0;100;" + std::to_string(serverGuid) + ";SkinGetBE;Creative;1;" + portStr + ";" + portStr + ";";
                res.writeShort((uint16_t)motd.length());
                res.writeBuffer(std::vector<uint8_t>(motd.begin(), motd.end()));
                server.send(res.data.data(), (int)res.data.size(), clientAddr);
            } catch (const std::exception& e) {
                Logger::error("UNCONNECTED_PING error: " + std::string(e.what()));
            }
            continue;
        }

        // Copy buffer for thread safety if passed to pool
        std::vector<uint8_t> packetData(recvBuf, recvBuf + len);
        auto session = getSession(clientAddr);

        // Use thread pool for complex packet processing
        pool.enqueue([packetData, session, &server, &ipProtocolCache, &ipCacheMtx]() {
            std::lock_guard<std::mutex> sessionLock(session->mtx);
            try {
                Buffer buf(packetData);
                uint8_t packetId = buf.readByte();

                auto sendFrame = [&](const std::vector<uint8_t>& payload, uint8_t reliability, bool ordered) {
                    Buffer res;
                    res.writeByte(0x84);
                    res.writeLTriad(session->packetSeq++);
                    res.writeByte(reliability << 5);
                    res.writeShort((uint16_t)payload.size() * 8);
                    if ((reliability >= 2 && reliability <= 4) || reliability == 6 || reliability == 7) res.writeLTriad(session->reliableSeq++);
                    if (reliability == 1 || reliability == 3 || reliability == 4 || reliability == 7) { res.writeLTriad(session->orderIndex++); res.writeByte(0); }
                    res.writeBuffer(payload);
                    server.send(res.data.data(), (int)res.data.size(), session->addr);
                };

                auto processGamePacket = [&](Buffer& inner) {
                    uint8_t bedrockId = inner.readByte();
                    if (bedrockId != 0xfe) return;
                    if (inner.offset >= inner.data.size()) return;
                    uint8_t algo = inner.data[inner.offset];
                    std::vector<uint8_t> batchRaw;
                    bool useZlib = false;
                    if (algo == 0x00) {
                        inner.offset++;
                        useZlib = true; // client is using zlib 竊・we must also use zlib for responses
                        batchRaw = zlibDecompress(inner.data.data() + inner.offset, inner.data.size() - inner.offset);
                    } else if (algo == 0xFF) {
                        inner.offset++;
                        batchRaw = std::vector<uint8_t>(inner.data.begin() + inner.offset, inner.data.end());
                    } else {
                        batchRaw = std::vector<uint8_t>(inner.data.begin() + inner.offset, inner.data.end());
                    }

                    auto makeBatch = [&](const std::vector<uint8_t>& payload) -> std::vector<uint8_t> {
                        Buffer out;
                        out.writeByte(0xfe);
                        if (useZlib) {
                            // compression established: algo byte 0x00 + raw-deflate
                            out.writeByte(0x00);
                            auto comp = zlibCompress(payload.data(), payload.size());
                            out.writeBuffer(comp);
                        } else {
                            // fallback / raw batch without compression
                            out.writeBuffer(payload);
                        }
                        return out.data;
                    };

                    Buffer batch(batchRaw);
                    while (batch.offset < batch.data.size()) {
                        uint32_t pLen = batch.readVarInt();
                        if (pLen == 0 || batch.offset + pLen > batch.data.size()) break;
                        size_t packetEnd = batch.offset + pLen;
                        uint32_t gId = batch.readVarInt();

                        if (gId == Bedrock::LOGIN) {
                            uint32_t detectedProtocol = handleLogin(batch);
                            if (detectedProtocol != 0) {
                                session->bedrockProtocol = detectedProtocol;
                                session->bedrockVersion  = resolveVersion(detectedProtocol);
                                Logger::success("Version confirmed: " + session->bedrockVersion + " (proto=" + std::to_string(detectedProtocol) + ")");

                                // Prepare PlayStatus (LOGIN_SUCCESS)
                                Buffer playStatus;
                                playStatus.writeVarInt(Bedrock::PLAY_STATUS);
                                playStatus.writeInt(0); // 0 = LOGIN_SUCCESS

                                // Prepare Disconnect
                                std::string discMsg = "Skin captured";
                                
                                Buffer disc;
                                disc.writeVarInt(Bedrock::DISCONNECT);
                                disc.writeSignedVarInt(0);    // reason
                                disc.writeBool(false);        // hideReason = false
                                disc.writeVarString(discMsg); // message
                                disc.writeVarString(discMsg); // filteredMessage

                                // Bundle BOTH packets into a single 0xfe batch for atomic delivery
                                Buffer combined;
                                combined.writeVarInt((uint32_t)playStatus.data.size());
                                combined.writeBuffer(playStatus.data);
                                combined.writeVarInt((uint32_t)disc.data.size());
                                combined.writeBuffer(disc.data);

                                // Send the combined batch
                                sendFrame(makeBatch(combined.data), 3, true);
                                Logger::success("Sent bundled PlayStatus + Disconnect to client.");

                                // Allow time for delivery before potential connection end
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            }
                        } else if (gId == Bedrock::REQUEST_NETWORK_SETTINGS) {
                            // Extract client protocol number (Big Endian 4bytes)
                            uint32_t clientProtocol = 0;
                            if (batch.offset + 4 <= batch.data.size()) {
                                clientProtocol = batch.readInt();
                            }

                            if (clientProtocol != 0) {
                                std::string clientIp = inet_ntoa(session->addr.sin_addr);
                                {
                                    std::lock_guard<std::mutex> lock(ipCacheMtx);
                                    ipProtocolCache[clientIp] = clientProtocol;
                                }
                                session->bedrockProtocol = clientProtocol;
                                session->bedrockVersion = resolveVersion(clientProtocol);
                                Logger::info("RequestNetworkSettings proto=" + std::to_string(clientProtocol) + " (" + session->bedrockVersion + ")");
                            }

                            // Reverting to the exact format that worked previously
                            Buffer settings;
                            settings.writeVarInt(Bedrock::NETWORK_SETTINGS);
                            settings.writeLShort(1); // compression threshold
                            settings.writeLShort(0); // zlib
                            settings.writeBool(false); // client throttle enabled
                            settings.writeByte(0);     // client throttle threshold
                            settings.writeLFloat(0.0f);

                            Buffer batchOut;
                            batchOut.writeByte(0xfe);
                            batchOut.writeVarInt((uint32_t)settings.data.size());
                            batchOut.writeBuffer(settings.data);
                            sendFrame(batchOut.data, 3, true);
                        }
                        batch.offset = packetEnd;
                    }
                };

                auto dispatchRakNetPacket = [&](std::vector<uint8_t>& frameBody) {
                    Buffer inner(frameBody);
                    uint8_t rId = inner.readByte();
                    if (rId == RakNet::CONNECTION_REQUEST) {
                        inner.readLong(); // skip guid
                        uint64_t timestamp = inner.readLong();
                        Buffer accept;
                        accept.writeByte(RakNet::CONNECTION_REQUEST_ACCEPTED);
                        accept.writeByte(0x04);
                        uint32_t ip = session->addr.sin_addr.s_addr;
                        accept.writeByte((ip>>0)&0xFF); accept.writeByte((ip>>8)&0xFF); accept.writeByte((ip>>16)&0xFF); accept.writeByte((ip>>24)&0xFF);
                        accept.writeShort(ntohs(session->addr.sin_port));
                        accept.writeShort(0);
                        for(int i=0; i<10; i++) { accept.writeByte(0x04); accept.writeInt(0); accept.writeShort(0); }
                        accept.writeLong(timestamp); accept.writeLong(0x12345678);
                        sendFrame(accept.data, 3, true);
                    } else if (rId == RakNet::CONNECTED_PING) {
                        uint64_t ts = inner.readLong();
                        Buffer pong;
                        pong.writeByte(RakNet::CONNECTED_PONG);
                        pong.writeLong(ts); pong.writeLong(0);
                        sendFrame(pong.data, 3, true);
                    } else if (rId == 0xfe) {
                        inner.offset--;
                        processGamePacket(inner);
                    }
                };

                // RakNet Handlers
                switch (packetId) {
                    case RakNet::OPEN_CONNECTION_REQUEST_1: {
                        session->packetSeq = 0; session->reliableSeq = 0; session->orderIndex = 0; session->splitPackets.clear();
                        if (packetData.size() > 17) {
                            session->raknetProtocol = packetData[17];
                            Logger::debug("RakNet Protocol: " + std::to_string(session->raknetProtocol));
                        }
                        Buffer res;
                        res.writeByte(RakNet::OPEN_CONNECTION_REPLY_1);
                        res.writeBuffer(std::vector<uint8_t>(MAGIC, MAGIC + 16));
                        res.writeLong(0x1234567812345678); res.writeByte(0x00); res.writeShort(1492);
                        server.send(res.data.data(), (int)res.data.size(), session->addr);
                        break;
                    }
                    case RakNet::OPEN_CONNECTION_REQUEST_2: {
                        Buffer res;
                        res.writeByte(RakNet::OPEN_CONNECTION_REPLY_2);
                        res.writeBuffer(std::vector<uint8_t>(MAGIC, MAGIC + 16));
                        res.writeLong(0x1234567812345678); res.writeByte(0x04);
                        uint32_t ip = session->addr.sin_addr.s_addr;
                        res.writeByte((ip>>0)&0xFF); res.writeByte((ip>>8)&0xFF); res.writeByte((ip>>16)&0xFF); res.writeByte((ip>>24)&0xFF);
                        res.writeShort(ntohs(session->addr.sin_port));
                        res.writeShort(1492); res.writeByte(0x00);
                        server.send(res.data.data(), (int)res.data.size(), session->addr);
                        break;
                    }
                    default: {
                        if (packetId >= 0x80 && packetId <= 0x8d) {
                            uint32_t receivedSeq = buf.readLTriad();
                            while (buf.offset < buf.data.size()) {
                                uint8_t flags = buf.readByte();
                                uint16_t bodyLen = (buf.readShort() + 7) >> 3;
                                uint8_t reliability = (flags & 0xe0) >> 5;
                                bool isSplit = (flags & 0x10) > 0;
                                if ((reliability >= 2 && reliability <= 4) || reliability == 6 || reliability == 7) buf.readLTriad();
                                if (reliability == 1 || reliability == 3 || reliability == 4 || reliability == 7) { buf.readLTriad(); buf.readByte(); }
                                uint32_t splitCount = 0, splitIndex = 0; uint16_t splitId = 0;
                                if (isSplit) { splitCount = buf.readInt(); splitId = buf.readShort(); splitIndex = buf.readInt(); }
                                if (buf.offset + bodyLen > buf.data.size()) break;
                                std::vector<uint8_t> body(buf.data.begin() + buf.offset, buf.data.begin() + buf.offset + bodyLen);
                                buf.offset += bodyLen;

                                if (isSplit) {
                                    auto& sp = session->splitPackets[splitId];
                                    if (sp.count == 0) { sp.count = splitCount; sp.fragments.assign(splitCount, {}); sp.received.assign(splitCount, false); }
                                    if (splitIndex < sp.count && !sp.received[splitIndex]) {
                                        sp.fragments[splitIndex] = body; sp.received[splitIndex] = true; sp.receivedCount++;
                                        if (sp.receivedCount == sp.count) {
                                            std::vector<uint8_t> full;
                                            for(uint32_t i=0; i<sp.count; i++) full.insert(full.end(), sp.fragments[i].begin(), sp.fragments[i].end());
                                            session->splitPackets.erase(splitId);
                                            dispatchRakNetPacket(full);
                                        }
                                    }
                                } else {
                                    dispatchRakNetPacket(body);
                                }
                            }
                            Buffer ack;
                            ack.writeByte(RakNet::PacketID::ACK); ack.writeShort(1); ack.writeByte(1);
                            ack.writeLTriad(receivedSeq); ack.writeLTriad(receivedSeq);
                            server.send(ack.data.data(), (int)ack.data.size(), session->addr);
                        }
                    }
                }
            } catch (const std::exception& e) {
                Logger::error("Packet Handling Error: " + std::string(e.what()));
            }
        });
    }

    return 0;
}


