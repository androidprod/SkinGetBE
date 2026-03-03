#include <iostream>
#include <vector>
#include <fstream>
#include <map>
#ifdef _WIN32
    #include <direct.h>   // _mkdir on Windows
#else
    #include <sys/stat.h> // mkdir on Linux
    #include <sys/types.h>
    #include <arpa/inet.h>
    #define _mkdir(name) mkdir(name, 0777)
#endif
#include <zlib.h>
#include "Net/UDP.h"
#include "RakNet/RakNet.h"
#include "Bedrock/Packets.h"
#include "Crypto/JWT.h"
#include "Util/Buffer.h"
#include "Util/Logger.h"
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <functional>
#include <chrono>
#include <memory>
#include <future>

// zlib raw deflate decompression (BE 1.20.30+ batch format)
// algo byte 0x00 → raw deflate (no zlib 0x78 header)
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

// ミニマルPNGエンコーダー (zlibのcompress2を利用)
static void writePNG(const std::string& path,
                     const uint8_t* rgba, int w, int h) {
    // 各スキャンラインに filter byte 0 (フィルタなし) を付与
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

std::string fallbackVersion = "1.21.x";
uint16_t fallbackProtocol = 766;

void loadConfig() {
    std::ifstream in("config.json");
    if (!in.good()) {
        std::ofstream out("config.json");
        out << "{\n  \"version\": \"1.21.x\",\n  \"protocol\": 766\n}\n";
        out.close();
        Logger::info("Created default config.json");
    } else {
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        size_t vPos = content.find("\"version\"");
        if (vPos != std::string::npos) {
            size_t start = content.find_first_of("\"", content.find(":", vPos));
            if (start != std::string::npos) {
                size_t end = content.find_first_of("\"", start + 1);
                if (end != std::string::npos) {
                    fallbackVersion = content.substr(start + 1, end - start - 1);
                }
            }
        }
        size_t pPos = content.find("\"protocol\"");
        if (pPos != std::string::npos) {
            size_t startP = content.find_first_of("0123456789", content.find(":", pPos));
            if (startP != std::string::npos) {
                 size_t endP = startP;
                 while(endP < content.length() && isdigit(content[endP])) endP++;
                 fallbackProtocol = std::stoi(content.substr(startP, endP - startP));
            }
        }
        Logger::info("Loaded config.json - Version: " + fallbackVersion + " Protocol: " + std::to_string(fallbackProtocol));
    }
}

void showHelp() {
    Logger::info("--- SkinGetBE Help ---");
    Logger::info("Usage: SkinGetBE.exe [options]");
    Logger::info("Options:");
    Logger::info("  --debug             Enable verbose debug logging");
    Logger::info("  --filter <ip>       Filter communications to only specified IP address");
    Logger::info("Example: SkinGetBE.exe --filter 127.0.0.1 --debug");
    Logger::info("----------------------");
}

bool handleLogin(Buffer& buf) {
    Logger::info("Handling Bedrock Login Packet...");
    try {
        uint32_t protocol = buf.readInt();
        Logger::info("Client Protocol: " + std::to_string(protocol));

        uint32_t payloadLen = buf.readVarInt();
        uint32_t chainLen = buf.readLInt();
        if (buf.offset + chainLen > buf.data.size()) return false;
        
        std::string chainJson((const char*)buf.data.data() + buf.offset, chainLen);
        buf.offset += chainLen;
        
        // Extract Player Name from chain JSON
        // The chain contains multiple JWT strings. We need to find the one with identity data.
        std::string playerName = "UnknownPlayer";
        size_t searchPos = 0;
        while ((searchPos = chainJson.find("\"", searchPos)) != std::string::npos) {
            size_t jwtStart = searchPos + 1;
            size_t jwtEnd = chainJson.find("\"", jwtStart);
            if (jwtEnd == std::string::npos) break;
            
            std::string token = chainJson.substr(jwtStart, jwtEnd - jwtStart);
            searchPos = jwtEnd + 1;
            
            // Tokens are fairly long and have exactly two dots
            if (token.length() > 50 && std::count(token.begin(), token.end(), '.') == 2) {
                std::string payload = JWT::getPayload(token);
                if (!payload.empty()) {
                    // Priority: xname (Xbox Live gamertag) > ThirdPartyName > displayName
                    std::string name = JWT::getJsonValue(payload, "xname");
                    if (name.empty()) name = JWT::getJsonValue(payload, "ThirdPartyName");
                    if (name.empty()) name = JWT::getJsonValue(payload, "displayName");
                    
                    if (!name.empty()) {
                        playerName = name;
                        // Continue scanning; a later token might have a higher-priority name
                    }
                }
            }
        }
        Logger::info("Detected Player: " + playerName);

        uint32_t skinLen = buf.readLInt();
        if (buf.offset + skinLen > buf.data.size()) return false;
        std::string skinJWT((const char*)buf.data.data() + buf.offset, skinLen);
        buf.offset += skinLen;

        std::string skinJson = JWT::getPayload(skinJWT);
        if (skinJson.empty()) return false;

        std::string skinId    = JWT::getJsonValue(skinJson, "SkinId");
        std::string skinImage = JWT::getJsonValue(skinJson, "SkinData");

        if (skinImage.empty()) return false;

        std::string skinRaw = Base64::decode(skinImage);
        size_t pixels = skinRaw.size() / 4;
        int imgW, imgH;
        if      (pixels == 64*32 ) { imgW=64;  imgH=32;  }
        else if (pixels == 64*64 ) { imgW=64;  imgH=64;  }
        else if (pixels == 128*64) { imgW=128; imgH=64;  }
        else if (pixels == 128*128){ imgW=128; imgH=128; }
        else { imgW=64; imgH=(int)(pixels/64); if(imgH==0) imgH=64; }

        auto sanitize = [](std::string s) {
            for (auto& c : s) if (c=='/' || c=='\\' || c==':' || c=='*' || c=='?' || c=='"' || c=='<' || c=='>' || c=='|') c='_';
            return s;
        };
        playerName = sanitize(playerName);
        skinId = sanitize(skinId);

        _mkdir("skins");

        std::string baseName = playerName + "_" + skinId;
        std::string targetPath = "skins/" + baseName + ".png";
        int count = 1;
        while (true) {
            std::ifstream f(targetPath);
            if (!f.good()) break;
            targetPath = "skins/" + baseName + "_" + std::to_string(count++) + ".png";
        }

        writePNG(targetPath, (const uint8_t*)skinRaw.data(), imgW, imgH);
        Logger::success(">>> Saved PNG: " + targetPath + " (" + std::to_string(imgW) + "x" + std::to_string(imgH) + ")");
        return true;
    } catch (const std::exception& e) {
        Logger::error("Failed to parse login: " + std::string(e.what()));
    }
    return false;
}


struct SplitPacket {
    uint32_t count        = 0;
    uint32_t receivedCount = 0;
    std::vector<std::vector<uint8_t>> fragments;
    std::vector<bool>                 received;
};

struct ClientSession {
    uint32_t packetSeq = 0;
    uint32_t reliableSeq = 0;
    uint32_t orderIndex = 0;
    std::map<uint16_t, SplitPacket> splitPackets;
    sockaddr_in addr;
    std::mutex mtx; // Individual session mutex
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

// NAT Discovery via STUN
static std::string discoverExternalIP() {
    UDP client;
    try {
        sockaddr_in stunAddr;
        memset(&stunAddr, 0, sizeof(stunAddr));
        stunAddr.sin_family = AF_INET;
        stunAddr.sin_port = htons(19302);
        struct hostent* host = gethostbyname("stun.l.google.com");
        if (!host) return "Unknown (DNS Failed)";
        memcpy(&stunAddr.sin_addr, host->h_addr_list[0], host->h_length);

        // STUN Message Header (20 bytes)
        // Type: 0x0001 (Binding Request)
        // Length: 0x0000
        // Magic: 0x2112A442
        // Transaction ID: 12 random bytes
        uint8_t request[20] = {0,1, 0,0, 0x21,0x12,0xA4,0x42};
        for(int i=8; i<20; i++) request[i] = (uint8_t)(rand() % 256);

        client.send(request, 20, stunAddr);

        uint8_t response[512];
        sockaddr_in from;
        #ifdef _WIN32
        // Set timeout for STUN recv
        int timeout = 1000;
        setsockopt(client.sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
        #endif

        int len = client.recv(response, sizeof(response), from);
        if (len < 20) return "Unknown (Timeout)";

        // Skip header and look for XOR-MAPPED-ADDRESS (0x0020) or MAPPED-ADDRESS (0x0001)
        int pos = 20;
        while (pos + 4 <= len) {
            uint16_t attrType = (response[pos] << 8) | response[pos+1];
            uint16_t attrLen  = (response[pos+2] << 8) | response[pos+3];
            pos += 4;
            if (attrType == 0x0020 && attrLen >= 8) { // XOR-MAPPED-ADDRESS
                uint8_t family = response[pos+1];
                if (family == 0x01) { // IPv4
                    uint32_t ip = ((uint32_t)response[pos+4] << 24) | ((uint32_t)response[pos+5] << 16) |
                                  ((uint32_t)response[pos+6] << 8) | (uint32_t)response[pos+7];
                    ip ^= 0x2112A442; // XOR with Magic Cookie
                    char buf[16];
                    sprintf(buf, "%d.%d.%d.%d", (ip>>24)&0xFF, (ip>>16)&0xFF, (ip>>8)&0xFF, ip&0xFF);
                    return std::string(buf);
                }
            }
            pos += attrLen;
        }
    } catch (...) {}
    return "Unknown";
}

int main(int argc, char* argv[]) {
    Logger::init();
    std::cout << "\033[1;36m" << R"(
   _____ _    _      _____      _   ____  ______ 
  / ____| |  (_)    / ____|    | | |  _ \|  ____|
  | (___ | | ___ _ _| |  __  ___| |_| |_) | |__   
   \___ \| |/ / | '_ \ | |_ |/ _ \ __|  _ <|  __|  
   ____) |   <| | | | | |__| |  __/ |_| |_) | |____ 
  |_____/|_|\_\_|_| |_|\_____|\___|\__|____/|______|
                                                   
    )" << "\033[0m" << std::endl;
    Logger::info("SkinGetBE starting up (Multi-threaded)...");
    loadConfig();
    
    std::string filterIp = "";
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--debug" || arg == "-d" || arg == "/debug") {
            Logger::debugEnabled = true;
        } else if (arg == "--filter" && i + 1 < argc) {
            filterIp = argv[++i];
        }
    }

    showHelp();
    
    // NAT Traversal / Discovery
    Logger::info("Attempting NAT discovery (STUN)...");
    std::string extIP = discoverExternalIP();
    Logger::success("External IP detected: " + extIP);
    Logger::info("Note: Ensure UDP Port 19132 is open on your router if not reachable.");

    UDP server;
    try {
        server.listen(19132);
    } catch (const std::exception& e) {
        Logger::error(e.what());
        return 1;
    }

    if (!filterIp.empty()) {
        Logger::info("Filter rules applied: " + filterIp);
    }

    ThreadPool pool(std::thread::hardware_concurrency());
    std::map<std::string, std::shared_ptr<ClientSession>> sessions;
    std::mutex sessionMtx;

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
        int len = server.recv(recvBuf, sizeof(recvBuf), clientAddr);
        if (len <= 0) continue;

        std::string clientIpStr = inet_ntoa(clientAddr.sin_addr);
        if (!filterIp.empty() && clientIpStr != filterIp) continue;

        // Copy buffer for thread safety if passed to pool
        std::vector<uint8_t> packetData(recvBuf, recvBuf + len);
        auto session = getSession(clientAddr);

        // Use thread pool for complex packet processing
        pool.enqueue([packetData, session, &server]() {
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
                        useZlib = true; // client is using zlib → we must also use zlib for responses
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
                            // fallback / raw batch: no algo byte, just varint length prefix
                            out.writeVarInt((uint32_t)payload.size());
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
                            if (handleLogin(batch)) {
                                // Prepare PlayStatus (LOGIN_SUCCESS)
                                Buffer playStatus;
                                playStatus.writeVarInt(Bedrock::PLAY_STATUS);
                                playStatus.writeInt(0); // 0 = LOGIN_SUCCESS

                                // Prepare Disconnect
                                std::string discMsg;
                                discMsg += "Skin Captured!\nThank you for using SkinGetBE.";
                                
                                Buffer disc;
                                disc.writeVarInt(Bedrock::DISCONNECT);
                                disc.writeSignedVarInt(0);    // reason
                                disc.writeBool(false);        // skipMessage = false
                                disc.writeVarString(discMsg); // message
                                disc.writeVarString(discMsg); // filteredMessage (Same as message)

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
                            // Reverting to the exact format that worked previously
                            Buffer settings;
                            settings.writeVarInt(Bedrock::NETWORK_SETTINGS);
                            settings.writeLShort(0);
                            settings.writeLShort(0); // zlib
                            settings.writeByte(0); settings.writeByte(0); settings.writeLFloat(0.0f);

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
                    case RakNet::UNCONNECTED_PING: {
                        uint64_t clientTime = buf.readLong();
                        buf.offset += 16;
                        uint64_t serverGuid = 0x1234567812345678;
                        Buffer res;
                        res.writeByte(RakNet::UNCONNECTED_PONG);
                        res.writeLong(clientTime); res.writeLong(serverGuid);
                        res.writeBuffer(std::vector<uint8_t>(MAGIC, MAGIC + 16));
                        std::string motd = "MCPE;SkinGetBE;" + std::to_string(fallbackProtocol) + ";" + fallbackVersion + ";0;100;" + std::to_string(serverGuid) + ";BedrockServer;Creative;1;19132;19132;";
                        res.writeShort((uint16_t)motd.length());
                        res.writeBuffer(std::vector<uint8_t>(motd.begin(), motd.end()));
                        server.send(res.data.data(), (int)res.data.size(), session->addr);
                        break;
                    }
                    case RakNet::OPEN_CONNECTION_REQUEST_1: {
                        session->packetSeq = 0; session->reliableSeq = 0; session->orderIndex = 0; session->splitPackets.clear();
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
