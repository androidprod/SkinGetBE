<div align="center">

<img src="res/SkinGetBE-Logo.ico" width="80" alt="SkinGetBE Logo" />

# SkinGetBE

**A C++ research tool that captures Minecraft Bedrock Edition player skins via a from-scratch RakNet + Bedrock protocol implementation**

[![Build](https://img.shields.io/github/actions/workflow/status/androidprod/SkinGetBE/build.yml?style=flat-square&label=Build)](https://github.com/androidprod/SkinGetBE/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow?style=flat-square)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?style=flat-square&logo=cplusplus)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/Platform-Win%20%7C%20Linux%20%7C%20ARM64-lightgrey?style=flat-square)](#building)
[![Protocol](https://img.shields.io/badge/Bedrock%20Protocol-Dynamic%20Version-green?style=flat-square)](#configuration)

**English** | [日本語](README.ja.md)

</div>

---

## Overview

**SkinGetBE** implements the Minecraft Bedrock Edition network stack in C++ from the ground up — from raw UDP sockets through RakNet handshaking to Bedrock Login packet parsing — and automatically saves connecting clients' skin images as PNG files.

No Mojang/Microsoft APIs or Xbox authentication infrastructure required. Everything from the RakNet handshake to the PNG encoder is self-contained.

> ⚠️ **Disclaimer**: This is an unofficial research and educational tool. It is not intended for commercial use or deployment on public servers. Use at your own risk.

---

## Features

### ✅ Implemented

| Category | Feature |
|----------|---------|
| **RakNet** | Unconnected Ping/Pong (with MOTD) |
| | Open Connection Request/Reply 1 & 2 |
| | Connection Request / Connection Request Accepted |
| | Connected Ping/Pong |
| | Frame Set Packet (0x80–0x8D) parsing |
| | Split Packet reassembly |
| | ACK transmission |
| **Bedrock** | `RequestNetworkSettings` → `NetworkSettings` (enables zlib compression) |
| | `Login` packet reception and parsing (multiple format variants) |
| | zlib raw deflate decompression/compression (Bedrock 1.20.30+ batch format) |
| | `PlayStatus` (LOGIN_SUCCESS) transmission |
| | `Disconnect` (capture complete notification) transmission |
| **Skin Extraction** | Player name from JWT chain (xname / ThirdPartyName / displayName) |
| | Skin Data (Base64 RGBA) extraction |
| | Automatic image dimension detection (64×32, 64×64, 128×64, 128×128) |
| | Dependency-free minimal PNG encoder (using zlib) |
| | Automatic sequential numbering for duplicate filenames |
| **Infrastructure** | Thread pool (`std::thread` × CPU core count) |
| | Per-client session state with individual mutexes |
| | External IP discovery via STUN (Google STUN servers) |
| | Color-coded timestamped logger (INFO / WARN / ERROR / SUCCESS / DEBUG) |
| | `config.json` for version and protocol number |
| | `--debug` / `--filter <IP>` CLI options |
| **Build** | CMake + FetchContent (auto-downloads zlib 1.3.1) |
| | GitHub Actions: Windows x64 / Linux x64 / Linux ARM64 cross-compile |

### 🔲 Not Yet Implemented

| Feature | Notes |
|---------|-------|
| Cape image extraction | Present in Login JWT but not yet parsed |
| Geometry JSON saving | Skin shape/animation data |
| Encrypted connection support | Required for Xbox Live connections |

---

## How It Works

```
[BE Client connects]
        │
        ▼
[RakNet Handshake]
  Ping → Pong (MOTD)
  OCR1 → OCReply1
  OCR2 → OCReply2
  ConnectionRequest → ConnectionRequestAccepted
        │
        ▼
[Bedrock Negotiation]
  RequestNetworkSettings → NetworkSettings (enable zlib compression)
        │
        ▼
[Login Packet received]
  Chain Data (JWT) → extract player name
  Skin Data (JWT) → Base64 RGBA → PNG encode → save to skins/
        │
        ▼
[Send PlayStatus (LOGIN_SUCCESS) + Disconnect]
  → Client immediately disconnected
```

---

## Project Structure

```
SkinGetBE/
├── main.cpp                    # Main loop & full protocol handling
├── RakNet/
│   └── RakNet.h                # RakNet packet ID constants & MAGIC bytes
├── Bedrock/
│   └── Packets.h               # Bedrock packet ID constants
├── Crypto/
│   ├── JWT.h                   # JWT payload extraction & JSON value lookup
│   └── Base64.h                # Standard & URL-safe Base64 decoder
├── Net/
│   └── UDP.h                   # UDP socket wrapper (Windows / Linux cross-platform)
├── Util/
│   ├── Buffer.h                # Binary read/write buffer (VarInt, strings, etc.)
│   ├── Json.h                  # Recursive descent JSON parser
│   ├── Logger.h                # Color-coded console logger
│   └── Logger.cpp
├── .github/workflows/build.yml # CI: Multi-platform build & release workflow
├── CMakeLists.txt
├── config.json                 # Version & protocol number configuration
├── build.bat                   # Windows build script
├── build.sh                    # Linux build script
└── app.rc                      # Windows resource file (icon)
```

---

## Building

### Requirements

- CMake 3.11 or later
- C++17-capable compiler (MSVC 2022 / GCC / Clang)
- Internet connection (for FetchContent zlib auto-download)

### Windows (MSVC)

```batch
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Or use the included script:

```batch
build.bat
```

### Linux

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Or use the included script:

```bash
chmod +x build.sh && ./build.sh
```

### Linux → ARM64 Cross-Compile

```bash
sudo apt-get install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

cat > arm64-toolchain.cmake <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
EOF

cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=arm64-toolchain.cmake
cmake --build build
```

---

## Usage

### Running

```bash
# Start normally (listens on port 19132)
./SkinGetBE

# Enable verbose debug logging
./SkinGetBE --debug

# Only process packets from a specific IP
./SkinGetBE --filter 192.168.1.100

# Combine options
./SkinGetBE --filter 192.168.1.100 --debug
```

### Connecting from Minecraft

1. Open Minecraft Bedrock Edition and go to the **Play** → **Servers** tab (or **LAN** tab depending on version)
2. Add a server with the address `127.0.0.1` (or the IP of the machine running SkinGetBE)
3. Attempt to connect — the skin will be captured and saved, then the client will be disconnected

### Output

Skins are saved as PNG files in the `skins/` directory:

```
skins/
├── Steve_Standard_Steve.png
├── Alex_CustomSkinId.png
└── Player_AnotherSkin_1.png   ← sequential suffix on duplicates
```

---

## Configuration

Edit `config.json` to target a specific Bedrock version's protocol number:

```json
{
  "version": "1.26.0",
  "protocol": 924
}
```

The file is auto-generated with defaults on first run if it does not exist.

Common protocol numbers:

| Bedrock Version | Protocol |
|-----------------|----------|
| 1.21.x          | 766      |
| 1.21.50         | 786      |
| 1.21.80         | 818      |
| 1.26.0          | 924      |

---

## Authentication Note

| Item | This tool's behavior |
|------|---------------------|
| Xbox Live / XSTS authentication | Not used |
| JWT signature verification | Not performed (data parsing only) |
| Effective mode | `offline: true` equivalent |

As a consequence, **this tool cannot function as a legitimate Bedrock server** and should only be used in local/testing environments.

---

## References

- [PrismarineJS / bedrock-protocol](https://github.com/PrismarineJS/bedrock-protocol)
- [RakNet Protocol Documentation](https://github.com/facebookarchive/RakNet)
- [wiki.vg/Bedrock Protocol](https://wiki.vg/Bedrock_Protocol)
- Minecraft Bedrock reverse engineering community resources

---

## License

[MIT License](LICENSE) © androidprod
