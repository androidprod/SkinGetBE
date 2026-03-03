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

// zlib raw deflate decompression (BE 1.20.30+ batch format)
// algo byte 0x00 → raw deflate (no zlib 0x78 header)
static std::vector<uint8_t> zlibDecompress(const uint8_t* src, size_t srcLen) {
    std::vector<uint8_t> out;
    out.resize(srcLen * 4 + 1024); // initial guess
    z_stream zs{};
    zs.next_in  = (Bytef*)src;
    zs.avail_in = (uInt)srcLen;
    // windowBits = -15: raw deflate (no zlib header/trailer)
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
        if (zs.avail_out == 0) out.resize(out.size() * 2); // grow buffer if needed
    } while (ret != Z_STREAM_END && zs.avail_in > 0);
    size_t finalSize = zs.total_out;
    inflateEnd(&zs);
    out.resize(finalSize);
    return out;
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
        // Bedrock Login packet binary format (PocketMine-MP / NukkitX 準拠):
        //   protocol  : BE Int32   (big-endian signed 32-bit)  ← NOT VarInt!
        //   chainLen  : LE UInt32  (little-endian 32-bit)      ← NOT VarInt!
        //   chainData : [chainLen bytes] JSON {"chain":["JWT",...]}  
        //   skinLen   : LE UInt32  (little-endian 32-bit)
        //   skinData  : [skinLen bytes] JWT
        //
        // 診断根拠: protocol=0x00000000 となるのは
        //   BE Int32(924) = 00 00 03 9C の最初の 0x00 を
        //   readSignedVarInt が 1byte VarInt(0) として誤読するため
        uint32_t protocol = buf.readInt(); // BE Int32
        Logger::info("Client Protocol: " + std::to_string(protocol));

        // ─── Login packet フォーマット (実測確定) ───────────────────────────
        // BE Int32     : protocol (= 924)
        // VarInt       : payload wrapper length  ← ここを読み飛ばす必要があった
        // LE Int32     : chainData length
        // [chainLen]   : chainData JSON  ({"Authent... で始まる)
        // LE Int32     : skinData length
        // [skinLen]    : skinData JWT
        // ────────────────────────────────────────────────────────────────────
        uint32_t payloadLen = buf.readVarInt();
        Logger::info("payloadWrapper length: " + std::to_string(payloadLen));

        uint32_t chainLen = buf.readLInt();
        Logger::info("chainData length: " + std::to_string(chainLen));
        if (buf.offset + chainLen > buf.data.size()) {
            Logger::error("chainData length overflow: " + std::to_string(chainLen));
            return false;
        }
        std::string chainData((const char*)buf.data.data() + buf.offset, chainLen);
        buf.offset += chainLen;
        Logger::info("chainData preview: " + chainData.substr(0, std::min((size_t)80, chainData.size())));

        // skinData — LE Int32 prefixed JWT
        uint32_t skinLen = buf.readLInt();
        Logger::info("skinData length: " + std::to_string(skinLen));
        if (buf.offset + skinLen > buf.data.size()) {
            Logger::error("skinData length overflow: " + std::to_string(skinLen));
            return false;
        }
        std::string skinJWT((const char*)buf.data.data() + buf.offset, skinLen);
        buf.offset += skinLen;
        Logger::info("skinData JWT size: " + std::to_string(skinJWT.size()));

        // --- JWT payload decode ---
        std::string skinJson = JWT::getPayload(skinJWT);
        Logger::info("skinJson size: " + std::to_string(skinJson.size()));
        if (skinJson.empty()) {
            Logger::error("JWT payload decode failed. JWT preview: " + skinJWT.substr(0, std::min((size_t)100, skinJWT.size())));
            return false;
        }

        std::string skinId    = JWT::getJsonValue(skinJson, "SkinId");
        std::string skinImage = JWT::getJsonValue(skinJson, "SkinData");
        Logger::info("SkinId=\"" + skinId + "\" SkinData size=" + std::to_string(skinImage.size()));

        if (skinId.empty() || skinImage.empty()) {
            Logger::error("Login parse failed: SkinId or SkinData empty.");
            Logger::error("  skinJson preview: " + skinJson.substr(0, std::min((size_t)200, skinJson.size())));
            return false;
        }

        // SkinData は Base64 エンコードされた生 RGBA データ
        std::string skinRaw = Base64::decode(skinImage);
        size_t pixels = skinRaw.size() / 4;
        int imgW, imgH;
        if      (pixels == 64*32 ) { imgW=64;  imgH=32;  }
        else if (pixels == 64*64 ) { imgW=64;  imgH=64;  }
        else if (pixels == 128*64) { imgW=128; imgH=64;  }
        else if (pixels == 128*128){ imgW=128; imgH=128; }
        else { imgW=64; imgH=(int)(pixels/64); if(imgH==0) imgH=64; }

        // ファイル名サニタイズ (Windows 使用不可文字を _ に)
        std::string safeName = skinId;
        for (auto& c : safeName)
            if (c=='/' || c=='\\' || c==':' || c=='*' || c=='?' ||
                c=='"' || c=='<'  || c=='>' || c=='|') c='_';

        // skins/ ディレクトリ 作成
        _mkdir("skins");

        std::string pngPath = "skins/" + safeName + ".png";
        writePNG(pngPath, (const uint8_t*)skinRaw.data(), imgW, imgH);
        Logger::success(">>> Saved PNG: " + pngPath +
                     " (" + std::to_string(imgW) + "x" + std::to_string(imgH) + ")");
        return true;
    } catch (const std::exception& e) {
        Logger::error("Failed to parse login: " + std::string(e.what()));
    }
    return false;
}


struct SplitPacket {
    uint32_t count        = 0;
    uint32_t receivedCount = 0;
    std::vector<std::vector<uint8_t>> fragments;  // indexed by splitIndex
    std::vector<bool>                 received;    // duplicate guard
};

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
    Logger::info("SkinGetBE starting up...");
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
    if (Logger::debugEnabled) Logger::debug("Debug mode enabled.");

    UDP server;
    try {
        server.listen(19132);
    } catch (const std::exception& e) {
        Logger::error(e.what());
        return 1;
    }

    if (!filterIp.empty()) {
        Logger::info("Filter rules applied. Only processing packets from IP: " + filterIp);
    }

    uint8_t recvBuf[4096];
    sockaddr_in clientAddr;
    std::map<uint16_t, SplitPacket> splitPackets;

    uint32_t packetSeq = 0;
    uint32_t reliableSeq = 0;
    uint32_t orderIndex = 0;

    auto sendFrame = [&](const std::vector<uint8_t>& payload, uint8_t reliability, bool ordered) {
        Buffer res;
        res.writeByte(0x84); // Standard datagram ID with reliable/ordered flags
        res.writeLTriad(packetSeq++);
        
        uint8_t flags = (reliability << 5);
        res.writeByte(flags);
        res.writeShort((uint16_t)payload.size() * 8);
        
        if ((reliability >= 2 && reliability <= 4) || reliability == 6 || reliability == 7) {
            res.writeLTriad(reliableSeq++);
        }
        if (reliability == 1 || reliability == 3 || reliability == 4 || reliability == 7) {
            res.writeLTriad(orderIndex++);
            res.writeByte(0); // Channel 0
        }
        
        res.writeBuffer(payload);
        server.send(res.data.data(), (int)res.data.size(), clientAddr);
        if (Logger::debugEnabled) Logger::debug("Sent RakNet Frame. Rel=" + std::to_string(reliability) + " Seq=" + std::to_string(packetSeq-1));
    };

    auto processGamePacket = [&](Buffer& inner) {
        uint8_t bedrockId = inner.readByte();
        if (bedrockId != 0xfe) return;

        if (inner.offset >= inner.data.size()) return;

        // ---------------------------------------------------------------
        // BE 1.20.30+ Batch format:
        //   [algorithm_byte: 1] [payload]
        //   0x00 = zlib (raw deflate, no 0x78 header)
        //   0x01 = snappy  (not implemented)
        //   0xFF = no compression (raw packets)
        //
        // Older format (rare): no algorithm byte → treat as raw packets
        // ---------------------------------------------------------------
        uint8_t algo = inner.data[inner.offset];

        std::vector<uint8_t> batchRaw;

        if (algo == 0x00) {
            // zlib / raw deflate
            inner.offset++; // consume algo byte
            const uint8_t* comp = inner.data.data() + inner.offset;
            size_t   compLen    = inner.data.size()  - inner.offset;
            Logger::info("Recv Bedrock Batch (0xfe): zlib compressed, size=" + std::to_string(compLen));
            try {
                batchRaw = zlibDecompress(comp, compLen);
                Logger::info("  Decompressed to " + std::to_string(batchRaw.size()) + " bytes");
            } catch (const std::exception& e) {
                Logger::error("zlib decompress failed: " + std::string(e.what()));
                return;
            }
        } else if (algo == 0x01) {
            // snappy — not implemented yet
            inner.offset++;
            Logger::error("Recv Bedrock Batch: snappy compression not supported");
            return;
        } else if (algo == 0xFF) {
            // no compression
            inner.offset++;
            batchRaw = std::vector<uint8_t>(inner.data.begin() + inner.offset, inner.data.end());
            Logger::info("Recv Bedrock Batch (0xfe): uncompressed, size=" + std::to_string(batchRaw.size()));
        } else {
            // Unknown or old format without algorithm byte — treat as raw
            batchRaw = std::vector<uint8_t>(inner.data.begin() + inner.offset, inner.data.end());
            char algHex[8]; sprintf(algHex, "0x%02X", algo);
            Logger::debug("Recv Bedrock Batch (0xfe): unknown algo byte " + std::string(algHex) + ", treating as raw");
        }

        // Parse VarInt-length-prefixed Bedrock packets from batch
        Buffer batch(batchRaw);
        while (batch.offset < batch.data.size()) {
            uint32_t pLen = batch.readVarInt();
            if (pLen == 0 || batch.offset + pLen > batch.data.size()) break;

            size_t packetEnd = batch.offset + pLen;
            uint32_t gId = batch.readVarInt();

            auto toHex = [](int i){ char b[10]; sprintf(b, "%02x", i); return std::string(b); };
            if (Logger::debugEnabled)
                Logger::debug("  Batch Packet ID: 0x" + toHex(gId) + " Len: " + std::to_string(pLen));

            if (gId == Bedrock::LOGIN) {
                if (handleLogin(batch)) {
                    // Skin capture success! Now disconnect the client gracefully.
                    Buffer disc;
                    disc.writeVarInt(Bedrock::DISCONNECT);
                    disc.writeSignedVarInt(0); // Reason: Disconnected
                    disc.writeBool(false);     // skipMessage: false
                    disc.writeVarString("Skin Captured! Thank you!");

                    Buffer batchOut;
                    batchOut.writeByte(0xfe);
                    batchOut.writeByte(0); // No compression (sending raw since we disconnect anyway)
                    batchOut.writeVarInt((uint32_t)disc.data.size());
                    batchOut.writeBuffer(disc.data);
                    
                    sendFrame(batchOut.data, 3, true);
                    Logger::success("Goal achieved! Skin captured and disconnection initiated.");
                }
            } else if (gId == Bedrock::REQUEST_NETWORK_SETTINGS) {
                uint32_t protocol = batch.readInt();
                Logger::info("Recv Request Network Settings (0xc1). Protocol: " + std::to_string(protocol));

                // NetworkSettings (0x8F):
                //   compressionThreshold : LShort  — 0=compress all, 65535=compress nothing
                //   compressionAlgorithm : LShort  — 0=zlib, 1=snappy, 255=none
                //   clientThrottleEnabled: byte
                //   clientThrottleThresh : byte
                //   clientThrottleScalar : LFloat
                Buffer payload;
                payload.writeVarInt(Bedrock::NETWORK_SETTINGS);
                payload.writeLShort(0);       // compressionThreshold=0 → compress everything (zlib below)
                payload.writeLShort(0);       // compressionAlgorithm=0 (zlib)
                payload.writeByte(0);         // clientThrottleEnabled = false
                payload.writeByte(0);         // clientThrottleThreshold
                payload.writeLFloat(0.0f);    // clientThrottleScalar

                // NetworkSettings レスポンスは圧縮ネゴシエーション「前」に送るので
                // algo byte を含めない（旧来の生バッチ形式）。
                // algo byte があると、クライアントが 0xFF を VarInt として
                // 誤読して Login フェーズに進めなくなる。
                Buffer batchOut;
                batchOut.writeByte(0xfe);
                // ← algo byte なし（圧縮確立前）
                batchOut.writeVarInt((uint32_t)payload.data.size());
                batchOut.writeBuffer(payload.data);

                sendFrame(batchOut.data, 3, true);
                Logger::info("Sent Network Settings (0x8f) — zlib, threshold=0");
            }

            batch.offset = packetEnd;
        }
    };

    while (true) {
        int len = server.recv(recvBuf, sizeof(recvBuf), clientAddr);
        if (len <= 0) continue;

        std::string clientIpStr = inet_ntoa(clientAddr.sin_addr);
        if (!filterIp.empty() && clientIpStr != filterIp) {
            continue; // 指定されたIP以外の通信を無視
        }

        try {
            Buffer buf(recvBuf, len);
            uint8_t packetId = buf.readByte();
        
        if (Logger::debugEnabled) {
            char hex[10]; sprintf(hex, "%02X", packetId);
            Logger::debug("Recv Raw Packet ID: 0x" + std::string(hex) + " (" + std::to_string(len) + " bytes)");
        }

        switch (packetId) {
            case RakNet::UNCONNECTED_PING: {
                uint64_t clientTime = buf.readLong();
                buf.offset += 16; // Skip magic
                uint64_t clientGuid = buf.readLong();
                
                if (Logger::debugEnabled) {
                    Logger::debug("Recv RakNet Ping (0x01) from " + clientIpStr);
                }
                
                uint64_t serverGuid = 0x1234567812345678;
                Buffer res;
                res.writeByte(RakNet::UNCONNECTED_PONG);
                res.writeLong(clientTime); 
                res.writeLong(serverGuid);
                res.writeBuffer(std::vector<uint8_t>(MAGIC, MAGIC + 16));
                
                std::string verStr = fallbackVersion;
                std::string protocolStr = std::to_string(fallbackProtocol);
                
                if (Logger::debugEnabled) {
                    Logger::debug("MOTD Config - Protocol: " + protocolStr + ", Version: " + verStr);
                }

                // Modern Bedrock MOTD: MCPE;MOTD;Protocol;Version;Players;Max;GUID;SubMOTD;Mode;ModeInt;Port4;Port6;
                std::string motd = "MCPE;SkinGetBE;" + protocolStr + ";" + verStr + ";0;100;" + std::to_string(serverGuid) + ";BedrockServer;Creative;1;19132;19132;";
                if (Logger::debugEnabled) Logger::debug("Sending MOTD: " + motd);

                res.writeShort((uint16_t)motd.length());
                res.writeBuffer(std::vector<uint8_t>(motd.begin(), motd.end()));
                server.send(res.data.data(), (int)res.data.size(), clientAddr);
                Logger::debug("Sent UNCONNECTED_PONG to " + std::string(inet_ntoa(clientAddr.sin_addr)));
                break;
            }

            case RakNet::OPEN_CONNECTION_REQUEST_1: {
                Logger::info("Recv Open Connection Request 1 (0x05) - Resetting Session");
                packetSeq = 0;
                reliableSeq = 0;
                orderIndex = 0;
                splitPackets.clear();
                
                Buffer res;
                res.writeByte(RakNet::OPEN_CONNECTION_REPLY_1);
                res.writeBuffer(std::vector<uint8_t>(MAGIC, MAGIC + 16));
                res.writeLong(0x1234567812345678); res.writeByte(0x00); res.writeShort(1492);
                server.send(res.data.data(), (int)res.data.size(), clientAddr);
                Logger::debug("Sent Open Connection Reply 1 (0x06)");
                break;
            }

            case RakNet::OPEN_CONNECTION_REQUEST_2: {
                Logger::info("Recv Open Connection Request 2 (0x07)");
                Buffer res;
                res.writeByte(RakNet::OPEN_CONNECTION_REPLY_2);
                res.writeBuffer(std::vector<uint8_t>(MAGIC, MAGIC + 16));
                res.writeLong(0x1234567812345678);
                res.writeByte(0x04);
                uint32_t ip = clientAddr.sin_addr.s_addr;
                res.writeByte((ip >> 0) & 0xFF); res.writeByte((ip >> 8) & 0xFF); res.writeByte((ip >> 16) & 0xFF); res.writeByte((ip >> 24) & 0xFF);
                res.writeShort(ntohs(clientAddr.sin_port));
                res.writeShort(1492); res.writeByte(0x00);
                server.send(res.data.data(), (int)res.data.size(), clientAddr);
                Logger::debug("Sent Open Connection Reply 2 (0x08)");
                break;
            }

            default: {
                if (packetId >= 0x80 && packetId <= 0x8d) {
                    uint32_t receivedSeq = buf.readLTriad();
                    while (buf.offset < buf.data.size()) {
                        uint8_t flags = buf.readByte();
                        uint16_t bitLen = buf.readShort();
                        uint16_t bodyLen = (bitLen + 7) >> 3;
                        uint8_t reliability = (flags & 0xe0) >> 5;
                        bool isSplit = (flags & 0x10) > 0;
                        
                        if ((reliability >= 2 && reliability <= 4) || reliability == 6 || reliability == 7) buf.readLTriad();
                        if (reliability == 1 || reliability == 3 || reliability == 4 || reliability == 7) { buf.readLTriad(); buf.readByte(); }
                        
                        uint32_t splitCount = 0, splitIndex = 0;
                        uint16_t splitId = 0;
                        if (isSplit) {
                            splitCount = buf.readInt();
                            splitId = buf.readShort();
                            splitIndex = buf.readInt();
                        }

                        if (Logger::debugEnabled) {
                            Logger::debug(" RakNet Frame: Reliability=" + std::to_string(reliability) + " Len=" + std::to_string(bodyLen) + (isSplit ? " SPLIT(cnt=" + std::to_string(splitCount) + " id=" + std::to_string(splitId) + " idx=" + std::to_string(splitIndex) + ")" : ""));
                        }

                        if (buf.offset + bodyLen > buf.data.size()) {
                            Logger::error(" RakNet Frame body overflow! bodyLen=" + std::to_string(bodyLen) + " rem=" + std::to_string(buf.data.size() - buf.offset));
                            break;
                        }
                        std::vector<uint8_t> body(buf.data.begin() + buf.offset, buf.data.begin() + buf.offset + bodyLen);
                        buf.offset += bodyLen;

                        // Helper lambda: RakNet session-level packet dispatcher (used for both
                        // non-split frames AND reassembled split packets).
                        auto dispatchRakNetPacket = [&](std::vector<uint8_t>& frameBody) {
                            Buffer inner(frameBody);
                            uint8_t rId = inner.readByte();
                            if (Logger::debugEnabled)
                                Logger::debug("Recv RakNet Session Packet. ID: 0x" + [](int i){ char b[10]; sprintf(b, "%02X", i); return std::string(b); }(rId));

                            if (rId == RakNet::CONNECTION_REQUEST) {
                                uint64_t clientGuid = inner.readLong();
                                uint64_t timestamp = inner.readLong();
                                bool secure = inner.readByte() > 0;
                                Logger::info("Recv RakNet Connection Request (0x09) GUID=" + std::to_string(clientGuid));

                                Buffer accept;
                                accept.writeByte(RakNet::CONNECTION_REQUEST_ACCEPTED);
                                uint32_t ip = clientAddr.sin_addr.s_addr;
                                accept.writeByte(0x04);
                                accept.writeByte((ip >> 0) & 0xFF); accept.writeByte((ip >> 8) & 0xFF); accept.writeByte((ip >> 16) & 0xFF); accept.writeByte((ip >> 24) & 0xFF);
                                accept.writeShort(ntohs(clientAddr.sin_port));
                                accept.writeShort(0); // System Index
                                for (int i = 0; i < 10; i++) {
                                    accept.writeByte(0x04);
                                    accept.writeInt(0); // 0.0.0.0
                                    accept.writeShort(0); // port 0
                                }
                                accept.writeLong(timestamp);
                                accept.writeLong(0x1234567812345678); // Server timestamp (dummy)
                                sendFrame(accept.data, 3, true); // RELIABLE_ORDERED (Bedrock requires ordered)
                                Logger::debug("Sent Connection Request Accepted (0x10) for TS=" + std::to_string(timestamp));
                            } else if (rId == RakNet::CONNECTED_PING) {
                                uint64_t ts = inner.readLong();
                                Buffer pong;
                                pong.writeByte(RakNet::CONNECTED_PONG);
                                pong.writeLong(ts);
                                pong.writeLong(0);
                                sendFrame(pong.data, 3, true);
                                Logger::debug("Sent Connected Pong for TS=" + std::to_string(ts));
                            } else if (rId == RakNet::NEW_INCOMING_CONNECTION) {
                                Logger::info("Recv New Incoming Connection (0x13)");
                            } else if (rId == 0xfe) {
                                inner.offset--;
                                processGamePacket(inner);
                            }
                        };

                        if (isSplit) {
                            auto& sp = splitPackets[splitId];

                            // 初回受信時にバッファを確保
                            if (sp.count == 0) {
                                sp.count         = splitCount;
                                sp.receivedCount = 0;
                                sp.fragments.assign(splitCount, {});
                                sp.received.assign(splitCount, false);
                            }

                            // 重複・再送チェック（RakNet は普通に再送してくる）
                            if (splitIndex >= sp.count || sp.received[splitIndex]) {
                                if (Logger::debugEnabled)
                                    Logger::debug("  [Split] id=" + std::to_string(splitId)
                                        + " idx=" + std::to_string(splitIndex)
                                        + " DUPLICATE — skip");
                            } else {
                                sp.fragments[splitIndex] = body;
                                sp.received[splitIndex]  = true;
                                sp.receivedCount++;
                                Logger::debug("  [Split] id=" + std::to_string(splitId)
                                    + " idx=" + std::to_string(splitIndex)
                                    + " received=" + std::to_string(sp.receivedCount)
                                    + "/" + std::to_string(sp.count));

                                if (sp.receivedCount == sp.count) {
                                    // 全フラグメント揃った → 再構築
                                    std::vector<uint8_t> full;
                                    full.reserve(sp.count * body.size());
                                    for (uint32_t i = 0; i < sp.count; i++)
                                        full.insert(full.end(), sp.fragments[i].begin(), sp.fragments[i].end());

                                    if (!full.empty()) {
                                        char diagHex[10]; sprintf(diagHex, "0x%02X", full[0]);
                                        Logger::info("Reassembled split id=" + std::to_string(splitId)
                                            + " size=" + std::to_string(full.size())
                                            + " first=" + std::string(diagHex));
                                    }
                                    splitPackets.erase(splitId);
                                    dispatchRakNetPacket(full);
                                }
                            }
                        } else {
                            // 非splitフレームも同じディスパッチャで処理
                            dispatchRakNetPacket(body);
                        }
                    }
                    Buffer ack;
                    ack.writeByte(RakNet::PacketID::ACK);
                    ack.writeShort(1); // 1 range
                    ack.writeByte(1);  // IsRange = true
                    ack.writeLTriad(receivedSeq); // Start
                    ack.writeLTriad(receivedSeq); // End
                    server.send(ack.data.data(), (int)ack.data.size(), clientAddr);
                    if (Logger::debugEnabled) Logger::debug("Sent ACK (Range) for Seq " + std::to_string(receivedSeq));
                }
                break;
            }
        }
        } catch (const std::exception& e) {
            Logger::error("Packet Handling Error: " + std::string(e.what()));
        }
    }
    return 0;
}
