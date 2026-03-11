# SkinGetBE

## 概要

**SkinGetBE** は、Minecraft Bedrock Edition（以下 BE）の通信仕様を解析・実装し、
接続してくるクライアントから
**スキンデータ（Skin / Cape / Geometry など）を取得・保存すること**を目的とした
研究・開発向けプロジェクトです。
※現時点ではSkin.pngのみ

本プロジェクトは公式 API や Mojang / Microsoft の認証基盤に依存せず、
**RakNet + Bedrock Protocol の低レイヤ通信を C++ で直接実装**する点に特徴があります。

---

## 目的

- Minecraft Bedrock のネットワーク構造（RakNet / Bedrock Protocol）の理解
- Node.js 製ライブラリ（`bedrock-protocol`, `node-minecraft-protocol` 等）の挙動解析
- Bedrock クライアントが送信するスキン情報の内部構造の把握
- C++ による高速・軽量なスキン取得ツールの実装
- 将来的な自動収集・解析・変換ツールへの発展

※ 本プロジェクトは **研究・技術検証目的** を前提としています。

---

## 想定ユースケース

- BE サーバーに接続してきたプレイヤーのスキンを自動保存
- LAN / ローカル環境での Bedrock 通信テスト
- RakNet 実装の学習用サンプル
- Bedrock プロトコルを用いた独自ツール開発の基盤

---

## 全体構成

```

SkinGetBE/
├─ main.cpp            # エントリポイント
├─ RakNet/
│  ├─ RakNet.h
│  ├─ RakNet.cpp       # RakNet 基本実装
│  └─ Reliability.cpp  # 信頼制御 / Split Packet
├─ Bedrock/
│  ├─ Packets.h        # Bedrock Packet 定義
│  ├─ Login.cpp        # Login / ResourcePack 処理
│  ├─ PlayStatus.cpp
│  └─ Skin.cpp         # Skin / Cape 解析
├─ Crypto/
│  ├─ JWT.cpp          # JWT 解析（署名検証なし）
│  └─ Base64.cpp
├─ Net/
│  └─ UDP.cpp          # UDP ソケット制御
├─ Util/
│  ├─ Logger.cpp
│  └─ Buffer.cpp
└─ README.md

```

---

## 技術的特徴

### 1. RakNet の自前実装

- UDP ベースの RakNet プロトコルを C++ で実装
- 対応要素
  - Open Connection Request / Reply
  - Reliability (ACK / NACK)
  - Split Packet 再構成
  - RakNet Control Packet (0x00, 0x01 等)

Node 実装を参考にしつつ、
**最低限 Skin 取得に必要な機能にフォーカス**しています。

---

### 2. Bedrock Protocol の解析

以下の BE パケットを中心に処理します。

- Login Packet
- PlayStatus
- ResourcePack Info / Stack
- ClientCacheStatus
- StartGame

特に **Login Packet 内の Chain Data / Skin Data** を重点的に解析します。

---

### 3. スキンデータ取得の仕組み

Bedrock クライアントは Login 時に以下の情報を送信します。

- Skin Image (PNG / RGBA)
- Skin ID
- Skin Geometry Name
- Geometry JSON
- Cape Image（存在する場合）

SkinGetBE では、

1. Login Packet を受信
2. JWT チェーンを Base64 デコード
3. JSON から Skin 関連フィールドを抽出
4. バイナリデータを復号せずそのまま取得
5. PNG / JSON として保存

という流れで処理します。

---

## 認証について

- Xbox Live 認証 / XSTS 認証は **使用しません**
- `offline: true` 相当の挙動
- 署名検証は行わず、データ構造の解析に限定

そのため、

- 正規サーバーとしての運用は不可
- ローカル / 検証用途向け

という位置付けになります。

---

## パフォーマンス設計

- Node.js 実装と比較して
  - 低レイテンシ
  - メモリ使用量削減
  - 起動直後から即接続処理

を重視しています。

また、

- 非同期 I/O（select / poll）
- コピー回数削減

を前提とした設計です。

---

## 現状のステータス

- [ ] RakNet ハンドシェイク
- [ ] Split Packet 再構成
- [ ] Bedrock Login 受信
- [ ] Skin JSON / Image 抽出
- [ ] 暗号化通信対応（将来）
- [ ] バージョン差異吸収

---

## 今後の展望

- Bedrock バージョン自動判定
- スキン形式の自動変換
- Web UI / API 化
- Linux / ARM 向け最適化
- Bot / Proxy への応用

---

## 注意事項

- 本プロジェクトは **非公式** です
- 利用は自己責任で行ってください
- 商用利用・公開サーバーでの使用は推奨されません

---

## 参考資料

- PrismarineJS / bedrock-protocol
- RakNet Protocol Documentation
- Minecraft Bedrock Reverse Engineering 資料

---

## ライセンス

MIT License




