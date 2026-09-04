<!--
  このファイルは github.com/HackCat0916/MX68K（公開リポジトリ）に
  README.md として置く下書き(2026-08-24、P682後「保留」解除・公開作業
  開始。同日、日本語の後ろに英訳を併記する形式へ変更。2026-09、ソース
  コード一式(mac/iOS)を公開する方針転換に伴い全面改訂)。

  README.md（本リポジトリのトップ）と共通する節(機能一覧・動作環境・
  対応ディスクイメージ形式・キーボードマッピング・ライセンス・謝辞)を
  編集した場合は、このファイルの対応箇所も同時に更新すること。

  各節は「日本語 → English」の順で併記する。表は日本語版の下に英語版を
  追加する形とし、キーボードマッピング表のようにキー名主体で言語非依存な
  表は見出しのみ英訳し表自体は共有する。
-->

# MX68K

**Sharp X68000 Emulator for macOS and iOS**

Sharp X68000のエミュレータを、macOS/iOS向けにSwiftUIとMetalでネイティブ実装したものです。
エミュレーションコアには実績のある [px68k](https://github.com/hissorii/px68k)（hissorii氏作）のC/C++ソースを流用し、GUIレイヤーをSwift/SwiftUIで構築しています。

このリポジトリでは、**ソースコード一式**（macOS/iOS両ターゲット）を公開しています。
ビルド方法は [USAGE.md](USAGE.md) を参照してください。macOS版のビルド済み実行モジュール
（`.app` / `.dmg`）は [Releases](../../releases) でも配布しています。

A native macOS/iOS port of the Sharp X68000 emulator, built with SwiftUI and Metal.
The emulation core reuses the proven C/C++ sources of [px68k](https://github.com/hissorii/px68k) (by hissorii), with the GUI layer built in Swift/SwiftUI.

This repository publishes the **full source code** (both the macOS and iOS targets).
See [USAGE.md](USAGE.md) for build instructions. Pre-built macOS binaries
(`.app` / `.dmg`) are also distributed via [Releases](../../releases).

---

## 主な機能 / Features

- **ネイティブmacOSアプリ** — SwiftUI + Metal で構築されたモダンなmacOSアプリ
- **px68kコア** — 実績のあるpx68kエミュレーションコアをベースに実機ソフトの動作を目指す
- **Apple Silicon ネイティブ** — arm64アーキテクチャに最適化（M1/M2/M3/M4シリーズ対応）
- **多様なディスクフォーマット対応** — XDF, DIM, D88, HDM, 2HD, IMG, HDF, HDS, ISO, **ZIP**（FD、単一/複数イメージ対応）に対応
- **ドラッグ&ドロップマウント** — ディスクイメージをウィンドウにドロップしてFDDマウント（**FD のみ** — HDD/SCSI/CD-ROM/MOイメージは各設定画面の該当行へD&D可）
- **キーボード入力** — X68000のJISキーボードレイアウトに対応、キーリマップ設定・ソフトウェアキーボード対応
- **ターボ / ノーウェイト** — 2x〜5x固定倍率、または上限無しの専用スレッド駆動ノーウェイトモード
- **ゲームパッド対応** — 2ポート対応、複数ボタンプロファイル（Standard / CPSF-MD / マジカルパッド）
- **ステートセーブ/ロード** — `*.mxstate`形式、個数無制限
- **スクリーンショット** — PNG保存（保存先変更可）
- **SASI / SCSI HDD** — SASI 8台・外付けSCSI/内蔵SCSI対応
- **MOドライブ** — SCSI ID5固定スロット、実行中のライブ媒体交換対応
- **CD-ROM（ISO・Mode1）マウント** — SCSI ID6固定スロット（CD-DA・CDブートは非対応）
- **Windrv** — Macのフォルダを共有ドライブとしてゲストからファイル読み書き
- **走査線エフェクト** — CRTディスプレイ風の表示効果をオン/オフ切替
- **拡張ボード** — MIDI（CZ-6BM1相当）・Mercury Unit（MK-MU1相当）
- **21種のモニタパネル** — CPU/CRTC/ビデオコントローラ/BG/サウンド/パレット/入力/ストレージ/MIDI/RTC/スプライト/OPMシンセサイザー等（System/Processor/Device/Sound/Peripherals/Video/Rendererの7グループに整理）
- **多言語対応** — 日本語/英語切り替え対応

**English:**

- **Native macOS App** — a modern macOS app built with SwiftUI + Metal
- **px68k Core** — based on the proven px68k emulation core, aiming for compatibility with real X68000 software
- **Apple Silicon Native** — optimized for arm64 (M1/M2/M3/M4 series)
- **Multiple Disk Formats** — XDF, DIM, D88, HDM, 2HD, IMG, HDF, HDS, ISO, **ZIP** (FD, single/multi-image archives)
- **Drag & Drop Mounting** — drop a disk image onto the window to mount it as FDD (**FD only** — HDD/SCSI/CD-ROM/MO images can be dropped onto the corresponding row in each settings tab)
- **Keyboard Input** — X68000 JIS keyboard layout support, key remapping, on-screen software keyboard
- **Turbo / No-Wait** — fixed 2x–5x multipliers, or an uncapped dedicated-thread no-wait mode
- **Gamepad Support** — 2 ports, multiple button profiles (Standard / CPSF-MD / Magical Pad)
- **State Save / Load** — `*.mxstate` format, unlimited slots
- **Screenshot** — saved as PNG (destination configurable)
- **SASI / SCSI HDD** — up to 8 SASI drives, internal/external SCSI support
- **MO Drive** — dedicated SCSI ID5 slot, live media swap while running
- **CD-ROM (ISO / Mode1) Mount** — dedicated SCSI ID6 slot (CD-DA and CD-boot are not supported)
- **Windrv** — share a Mac folder as a guest-accessible drive for file read/write
- **Scanline Effect** — toggleable CRT-style display effect
- **Extension Boards** — MIDI (CZ-6BM1 equivalent), Mercury Unit (MK-MU1 equivalent)
- **21 Monitor Panels** — CPU/CRTC/video controller/BG/sound/palette/input/storage/MIDI/RTC/sprite/OPM synthesizer, etc. (organized into 7 groups: System/Processor/Device/Sound/Peripherals/Video/Renderer)
- **Localization** — switchable Japanese/English UI

---

## 動作環境 / System Requirements

| 項目 | 要件 |
|------|-------------|
| macOS | 13.0 Ventura 以降・Apple Silicon (arm64) |
| iOS | 16.0以降（ソースからビルド。詳細は[USAGE.md](USAGE.md)） |

| Item | Requirement |
|------|-------------|
| macOS | 13.0 Ventura or later, Apple Silicon (arm64) |
| iOS | 16.0 or later (build from source — see [USAGE.md](USAGE.md)) |

---

## インストール / Installation

**macOS:**

1. [Releases](../../releases) から最新の `.dmg` をダウンロード
2. マウントして `MX68K.app` を `/Applications` へドラッグ
3. 初回起動時はGatekeeperの確認が出る場合があります（右クリック→開く）

ソースからビルドする場合や、**iOS版**（ビルド済み配布物なし、ソースからのビルドのみ）
については [USAGE.md](USAGE.md) を参照してください。

**English:**

**macOS:**

1. Download the latest `.dmg` from [Releases](../../releases)
2. Mount it and drag `MX68K.app` to `/Applications`
3. On first launch, Gatekeeper may show a warning (right-click → Open)

To build from source, or for the **iOS** target (no pre-built distribution —
build from source only), see [USAGE.md](USAGE.md).

---

## 必要なBIOSファイル / Required BIOS Files

本エミュレータの動作には、実機由来のX68000 BIOS ROMファイルが必要です。**実機から吸い出したものをご用意ください**（本リポジトリには含まれていません）。最低限必要なのは`IPLROM.DAT`・`CGROM.DAT`の2つで、他はいずれも用途に応じた任意ファイルです。

| ファイル | 内容 | サイズ | 必須/任意 |
|------|------|------|------|
| `IPLROM.DAT` | IPL ROM（無印〜XVI） | 131,072 バイト | 必須 |
| `CGROM.DAT` | キャラクタジェネレータROM | 786,432 バイト | 必須 |
| `IPLROM30.DAT` | IPL ROM（X68030モード） | 131,072 バイト | 任意 |
| `SCSIINROM.DAT` | 内蔵SCSI起動用IPL ROM | 8,192 バイト | 任意（内蔵SCSIから起動する場合） |
| `SCSIEXROM.DAT` | 外付けSCSIボード（CZ-6BS1相当）起動用IPL ROM | 8,192 バイト | 任意（外付けSCSIから起動する場合） |

**配置先（既定）:**
```
~/Library/Application Support/MX68K/bios/
```

**English:**

The emulator requires original X68000 BIOS ROM files, obtained from a real machine (they are **not included** in this repository). Only `IPLROM.DAT`/`CGROM.DAT` are strictly required; the rest are optional, depending on which features you use.

| File | Description | Size | Required |
|------|-------------|------|----------|
| `IPLROM.DAT` | IPL ROM (Original ~ XVI) | 131,072 bytes | Yes |
| `CGROM.DAT` | Character Generator ROM | 786,432 bytes | Yes |
| `IPLROM30.DAT` | IPL ROM (X68030 mode) | 131,072 bytes | Optional |
| `SCSIINROM.DAT` | Internal SCSI boot IPL ROM | 8,192 bytes | Optional (booting from internal SCSI) |
| `SCSIEXROM.DAT` | External SCSI board (CZ-6BS1) boot IPL ROM | 8,192 bytes | Optional (booting from external SCSI) |

**Default location:**
```
~/Library/Application Support/MX68K/bios/
```

---

## 対応ディスクイメージ形式 / Supported Disk Image Formats

| 拡張子 | 形式 | 備考 |
|-----------|--------|-------|
| `.xdf` | X68000 FDイメージ | 標準（human302.xdf 等） |
| `.dim` | DIM形式 | 一般的なFDイメージ形式 |
| `.d88` | D88形式 | PC-8801系と互換 |
| `.hdm` | HDM形式 | |
| `.2hd` | 2HDフロッピーイメージ | |
| `.hdf` | ハードディスクイメージ（SASI） | 設定画面（SASIタブ）からマウント、各行へD&D可 |
| `.hds` | ハードディスクイメージ（SCSI） | 設定画面（SCSIタブ）からマウント、各行へD&D可 |
| `.mos` | MOイメージ | 設定画面（SCSIタブ、ID5固定）からマウント、実行中のライブ交換対応 |
| `.iso` | CD-ROMイメージ（ISO・Mode1） | 設定画面（SCSIタブ、ID6固定）からマウント。CD-DA・CDブートは非対応 |
| `.img` | 汎用イメージ | |
| `.zip` | 圧縮FDイメージ | FD専用、複数イメージ格納時は選択シート表示 |

**English:**

| Extension | Format | Notes |
|-----------|--------|-------|
| `.xdf` | X68000 FD image | Primary target (e.g. human302.xdf) |
| `.dim` | DIM format | Common FD image format |
| `.d88` | D88 format | PC-8801 compatible |
| `.hdm` | HDM format | |
| `.2hd` | 2HD floppy image | |
| `.hdf` | Hard disk image (SASI) | Mount from the SASI settings tab; drag & drop onto each row |
| `.hds` | Hard disk image (SCSI) | Mount from the SCSI settings tab; drag & drop onto each row |
| `.mos` | MO image | Mount from the SCSI settings tab (fixed ID5); live swap while running |
| `.iso` | CD-ROM image (ISO, Mode1) | Mount from the SCSI settings tab (fixed ID6); CD-DA and CD-boot are not supported |
| `.img` | Generic image | |
| `.zip` | Compressed FD image | FD only; a selection sheet appears for multi-image archives |

---

## キーボードマッピング / Keyboard Mapping

X68000はJIS配列のキーボードを採用しています。macOSのキーコードをX68000のスキャンコードへマッピングしています。

X68000 uses a JIS keyboard layout. macOS keycodes are mapped to X68000 scan codes.

| X68000キー / X68000 Key | macOSキー / macOS Key |
|-----------|-----------|
| XF1〜XF5 | F6〜F10 |
| SHIFT | Shift |
| CTRL | Control |
| OPT.1 | F11 |
| OPT.2 | F12 |
| HELP | F13 |
| ROLL UP | Page Up（HELPとは別キー / a separate key from HELP） |
| COPY | F14 |
| BREAK | F15 |

---

## ライセンス・法的事項 / License & Legal Notice

本プロジェクトはC68K(MC68000 CPUコア)のGPL-2.0-or-laterを含む結合著作物のため、
**GPL-2.0-or-later**の下でソースコードを公開しています。各コンポーネントの由来・
ライセンスは [LICENSE](LICENSE)・[ATTRIBUTION.md](ATTRIBUTION.md) を、XM6由来
コードの許諾経緯は [NOTICE-THIRD-PARTY.md](NOTICE-THIRD-PARTY.md) を参照してください。

- **SHARP純正のBIOS ROMは含まれていません。** ご自身で合法的に入手したものをご利用ください。
- Human68k自体は2000年にシャープ・ハドソン等の権利各社により無償公開されましたが、これは使用許諾であり著作権の移転ではありません。著作権は権利各社に帰属したままで、**パブリックドメインではありません**。使用は実機X68000/X68030シリーズおよびそのエミュレータ上に限られます。詳細は[許諾条件](http://retropc.net/x68000/software/sharp/license.htm)を参照してください。

**English:**

Because this project statically links C68K (the MC68000 CPU core), which is
licensed under GPL-2.0-or-later, MX68K's source code is published under
**GPL-2.0-or-later**. See [LICENSE](LICENSE) and [ATTRIBUTION.md](ATTRIBUTION.md)
for the provenance and license of each component, and
[NOTICE-THIRD-PARTY.md](NOTICE-THIRD-PARTY.md) for the XM6 permission.

- **Genuine SHARP BIOS ROMs are NOT included.** Users must provide their own legally obtained copies.
- Human68k itself was freely released in 2000 by Sharp, Hudson, and other rights holders — this is a usage license, not a transfer of copyright. Sharp/Hudson retain copyright, and it is **not** public domain. Use is limited to genuine X68000/X68030 hardware and emulators thereof. See the [license terms](http://retropc.net/x68000/software/sharp/license.htm) (Japanese) for details.

---

## 謝辞 / Acknowledgements

- [px68k](https://github.com/hissorii/px68k) by hissorii — X68000エミュレータコア / X68000 emulator core
- [c68k](https://github.com/kenyahiro/px68k/tree/master/m68000/c68k) by Stephane Dallongeville（Yabause同梱版、kenyahiro氏のpx68kフォーク経由） — MC68000 CPUエミュレータ（ARM64対応） / MC68000 CPU emulator (via kenyahiro's px68k fork, ARM64-compatible)
- [fmgen](http://retropc.net/cisc/m88/) by cisc — FM音源生成ライブラリ / FM sound generator library

---

*本プロジェクトは開発中です。 / This project is under active development.*
