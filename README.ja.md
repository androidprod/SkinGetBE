<div align="center">

<img src="res/SkinGetBE-Logo.ico" width="80" alt="SkinGetBE Logo" />

# SkinGetBE

**Minecraft Bedrock Edition プレイヤーのスキンを、RakNet + Bedrockプロトコルの自前実装で丸ごとキャプチャするC++製研究ツール**

[![Build](https://img.shields.io/github/actions/workflow/status/androidprod/SkinGetBE/build.yml?style=flat-square&label=Build)](https://github.com/androidprod/SkinGetBE/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow?style=flat-square)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?style=flat-square&logo=cplusplus)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/Platform-Win%20%7C%20Linux%20%7C%20ARM64-lightgrey?style=flat-square)](#ビルド方法)
[![Protocol](https://img.shields.io/badge/Bedrock%20Protocol-924%20(1.26.0)-green?style=flat-square)](#設定)

[English](README.md) | **日本語**

</div>

---

## 概要

**SkinGetBE** は、Minecraft Bedrock Edition（BE）の通信スタックを C++ で一から実装し、接続してきたクライアントからスキン画像（PNG）を自動的に取得・保存する研究目的のツールです。

Mojang / Microsoft の公式 API・Xbox 認証には一切依存せず、**UDP ソケットレベルから RakNet ハンドシェイク・Bedrock Login パケット解析・スキン PNG 出力まで**をすべて自前で処理します。

> ⚠️ **注意**: 本プロジェクトは非公式かつ研究・技術検証目的のツールです。商用利用・公開サーバーへの展開は推奨しません。利用は自己責任でお願いします。

---

## 機能

### ✅ 実装済み

| カテゴリ | 機能 |
|----------|------|
| **RakNet** | Unconnected Ping/Pong（MOTD付き） |
| | Open Connection Request/Reply 1 & 2 |
| | Connection Request / Connection Request Accepted |
| | Connected Ping/Pong |
| | Frame Set Packet（0x80〜0x8D）解析 |
| | Split Packet 分割受信・再構成 |
| | ACK 送信 |
| **Bedrock** | `RequestNetworkSettings` → `NetworkSettings`（圧縮設定） |
| | `Login` パケット受信・解析（複数フォーマット対応） |
| | zlib raw deflate 解凍 / 圧縮（Bedrock 1.20.30+ バッチ形式） |
| | `PlayStatus`（LOGIN_SUCCESS）送信 |
| | `Disconnect`（キャプチャ完了通知）送信 |
| **スキン抽出** | JWT チェーンからプレイヤー名取得（xname / ThirdPartyName / displayName） |
| | Skin Data（Base64 RGBA）抽出 |
| | 画像サイズ自動判定（64×32, 64×64, 128×64, 128×128） |
| | 外部ライブラリ不要の最小 PNG エンコーダ（zlib 使用） |
| | 重複ファイル名の自動連番処理 |
| **インフラ** | スレッドプール（`std::thread` × CPU コア数） |
| | セッション管理（クライアントごとに独立した状態・Mutex） |
| | STUN による外部 IP 自動検出（Google STUN サーバー使用） |
| | カラー付きタイムスタンプロガー（INFO / WARN / ERROR / SUCCESS / DEBUG） |
| | `config.json` によるバージョン・プロトコル番号設定 |
| | `--debug` / `--filter <IP>` CLIオプション |
| **ビルド** | CMake + FetchContent（zlib 1.3.1 自動ダウンロード） |
| | GitHub Actions：Windows x64 / Linux x64 / Linux ARM64 クロスコンパイル |

### 🔲 未実装（予定）

| 機能 | 備考 |
|------|------|
| Cape（マント）画像の抽出 | Login JWT に含まれているが未処理 |
| Geometry JSON の保存 | スキン形状データ |
| 暗号化通信（Encryption）対応 | Xbox Live 接続時に必要 |

---

## 動作の仕組み

```
[BE クライアント 接続]
        │
        ▼
[RakNet ハンドシェイク]
  Ping → Pong (MOTD)
  OCR1 → OCReply1
  OCR2 → OCReply2
  ConnectionRequest → ConnectionRequestAccepted
        │
        ▼
[Bedrock ネゴシエーション]
  RequestNetworkSettings → NetworkSettings (zlib 圧縮有効化)
        │
        ▼
[Login パケット受信]
  Chain Data (JWT) → プレイヤー名抽出
  Skin Data (JWT) → Base64 RGBA → PNG エンコード → skins/ 保存
        │
        ▼
[PlayStatus (LOGIN_SUCCESS) + Disconnect 送信]
  → クライアントを即座に切断
```

---

## プロジェクト構成

```
SkinGetBE/
├── main.cpp                    # メインループ・全プロトコル処理
├── RakNet/
│   └── RakNet.h                # RakNet パケット ID 定数・MAGIC バイト列
├── Bedrock/
│   └── Packets.h               # Bedrock パケット ID 定数
├── Crypto/
│   ├── JWT.h                   # JWT ペイロード抽出・JSON 値取得
│   └── Base64.h                # Base64 / Base64URL デコード
├── Net/
│   └── UDP.h                   # UDP ソケット（Windows / Linux クロスプラットフォーム）
├── Util/
│   ├── Buffer.h                # バイナリ読み書きバッファ（VarInt・文字列対応）
│   ├── Json.h                  # 再帰下降 JSON パーサ
│   ├── Logger.h                # カラー付きコンソールロガー
│   └── Logger.cpp
├── .github/workflows/build.yml # CI：マルチプラットフォームビルド & リリース
├── CMakeLists.txt
├── config.json                 # バージョン・プロトコル設定
├── build.bat                   # Windows ビルドスクリプト
├── build.sh                    # Linux ビルドスクリプト
└── app.rc                      # Windows リソース（アイコン）
```

---

## ビルド方法

### 必要なもの

- CMake 3.11 以上
- C++17 対応コンパイラ（MSVC 2022 / GCC / Clang）
- インターネット接続（zlib の FetchContent 自動取得のため）

### Windows（MSVC）

```batch
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

または付属の `build.bat` を実行：

```batch
build.bat
```

### Linux

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

または付属の `build.sh` を実行：

```bash
chmod +x build.sh && ./build.sh
```

### Linux → ARM64 クロスコンパイル

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

## 使い方

### 起動

```bash
# 通常起動（ポート 19132 で待ち受け）
./SkinGetBE

# デバッグログを有効にして起動
./SkinGetBE --debug

# 特定 IP アドレスのみ処理
./SkinGetBE --filter 192.168.1.100

# 組み合わせ
./SkinGetBE --filter 192.168.1.100 --debug
```

### Minecraft クライアント側の操作

1. BE クライアントを起動し、**ローカルサーバー** タブを開く
2. `127.0.0.1`（または SkinGetBE を動かしているマシンのIPアドレス）に接続
3. 接続を試みると自動的にスキンが取得・保存され、切断される

### 取得結果

スキンは `skins/` ディレクトリに PNG 形式で保存されます。

```
skins/
├── Steve_Standard_Steve.png
├── Alex_CustomSkinId.png
└── Player_AnotherSkin_1.png   ← 重複時は連番が付く
```

---

## 設定

`config.json` でターゲットバージョンのプロトコル番号を設定します。

```json
{
  "version": "1.26.0",
  "protocol": 924
}
```

ファイルが存在しない場合は起動時に自動生成されます。

主要なプロトコル番号の対応表：

| Bedrock バージョン | プロトコル番号 |
|-------------------|---------------|
| 1.21.x            | 766           |
| 1.21.50           | 786           |
| 1.21.80           | 818           |
| 1.26.0            | 924           |

---

## 認証について

| 項目 | 本ツールの扱い |
|------|--------------|
| Xbox Live / XSTS 認証 | 使用しない |
| JWT 署名検証 | 行わない（データ解析のみ） |
| 動作モード | `offline: true` 相当 |

そのため、**正規 Bedrock サーバーとしての運用はできません**。ローカル・検証環境でのみ使用してください。

---

## 参考資料

- [PrismarineJS / bedrock-protocol](https://github.com/PrismarineJS/bedrock-protocol)
- [RakNet Protocol Documentation](https://github.com/facebookarchive/RakNet)
- [wiki.vg/Bedrock Protocol](https://wiki.vg/Bedrock_Protocol)
- Minecraft Bedrock リバースエンジニアリング コミュニティ資料

---

## ライセンス

[MIT License](LICENSE) © androidprod
