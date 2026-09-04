<!--
  このファイルは公開リポジトリ（github.com/HackCat0916/MX68K）に
  `USAGE.md` として置く使い方ガイドの下書き。README_public.md と同様、
  開発者向けの内部設計への言及はしない前提で書く。ソースコード一式を
  公開する方針(2026-09)のため、ビルド方法(macOS/iOS)も含む。

  README.md/README_public.md の機能一覧を更新した場合、本ファイルの
  対応箇所（メニュー構成・ショートカット等）も同時に確認すること。

  各節は「日本語 → English」の順で併記する(2026-08-24追加)。
-->

# MX68K 使い方ガイド / Usage Guide

## 0. ビルド方法 / Building from Source

### macOS

1. `MX68K/MX68K.xcodeproj` をXcodeで開き、`MX68K`スキームを選択して実行(⌘R)します。
   コマンドラインの場合: `xcodebuild -project MX68K/MX68K.xcodeproj -scheme MX68K -destination 'platform=macOS'`
2. 必要環境: macOS 13.0以降・Xcode 15.0以降。

### iOS

1. `MX68K-iOS`スキームを選択し、シミュレータまたは実機を実行先に指定して実行(⌘R)します。
2. **実機へインストールする場合**（App Store・Apple Developer Program加入いずれも不要）:
   - iPhone/iPadをUSB（またはWi-Fi）でMacに接続します。
   - Xcodeの「Signing & Capabilities」タブで、Teamに自分のApple ID（無料の
     Personal Team。Xcode → Settings → Accountsで未サインインの場合は
     ここでサインインすると自動的に作成されます）を選択します。
   - 実行先に接続した実機を選び、⌘Rでビルド・署名・インストールします。
   - 初回起動時はiOS側で「信頼されていない開発者」として拒否されます。
     設定 → 一般 → VPNとデバイス管理 で自分のApple IDを選び「信頼」を
     タップしてください。
   - **無料アカウントの署名は7日間で失効します。** 失効後は再度Xcodeから
     実行し直せば再署名されます（Apple側の制約で、MX68K固有の制限では
     ありません）。
3. BIOSファイルの配置: iOS版はファイルシステムへ直接コピーする代わりに、
   起動時の設定画面（またはメニューの「Settings」）の「BIOS」タブから
   Filesアプリ経由でIPLROM.DAT/CGROM.DAT等を選択します。選択したファイルは
   アプリ内のストレージへ自動的にコピーされます。
4. 必要環境: iOS 16.0以降、Xcode 15.0以降。

**English:**

### macOS

1. Open `MX68K/MX68K.xcodeproj` in Xcode, select the `MX68K` scheme, and
   run it (⌘R). Command line: `xcodebuild -project MX68K/MX68K.xcodeproj -scheme MX68K -destination 'platform=macOS'`
2. Requirements: macOS 13.0+, Xcode 15.0+.

### iOS

1. Select the `MX68K-iOS` scheme, pick a Simulator or a connected device as
   the run destination, and run it (⌘R).
2. **Installing on a real device** (no App Store or paid Apple Developer
   Program membership required):
   - Connect your iPhone/iPad to your Mac via USB (or Wi-Fi).
   - Under Xcode's "Signing & Capabilities" tab, set Team to your own Apple
     ID (a free "Personal Team" — sign in under Xcode → Settings → Accounts
     first if you haven't; Xcode creates the Personal Team automatically).
   - Select your connected device as the run destination and run (⌘R) —
     Xcode builds, signs, and installs the app.
   - On first launch, iOS will refuse to run the app as an "Untrusted
     Developer". Go to Settings → General → VPN & Device Management, select
     your Apple ID, and tap "Trust".
   - **Free-account signatures expire after 7 days.** After that, just run
     from Xcode again to re-sign — this is an Apple platform limitation, not
     specific to MX68K.
3. BIOS files: instead of copying files directly into the filesystem, the
   iOS build's Settings screen (shown on first launch, or from the menu) has
   a "BIOS" tab that opens the Files app to pick `IPLROM.DAT`/`CGROM.DAT`
   etc. — selected files are copied into the app's own storage automatically.
4. Requirements: iOS 16.0+, Xcode 15.0+.

## 1. BIOSファイルの準備 / BIOS File Setup

MX68Kの動作には、実機由来のX68000 BIOS ROMファイルが必要です（著作権の都合上、本リポジトリには含まれていません）。実機をお持ちの方は吸い出しツール等でご用意ください。最低限必要なのは`IPLROM.DAT`・`CGROM.DAT`の2つで、他は用途に応じた任意ファイルです。

| ファイル | 内容 | サイズ | 必須/任意 |
|------|------|------|------|
| `IPLROM.DAT` | IPL ROM（無印〜XVI） | 131,072 バイト | 必須 |
| `CGROM.DAT` | キャラクタジェネレータROM | 786,432 バイト | 必須 |
| `IPLROM30.DAT` | IPL ROM（X68030モード） | 131,072 バイト | 任意 |
| `SCSIINROM.DAT` | 内蔵SCSI起動用IPL ROM | 8,192 バイト | 任意（内蔵SCSIから起動する場合） |
| `SCSIEXROM.DAT` | 外付けSCSIボード（CZ-6BS1相当）起動用IPL ROM | 8,192 バイト | 任意（外付けSCSIから起動する場合） |

配置先: `~/Library/Application Support/MX68K/bios/`

**English:**

MX68K requires original X68000 BIOS ROM files obtained from a real machine (they are not included in this repository due to copyright). Only `IPLROM.DAT`/`CGROM.DAT` are strictly required; the rest are optional depending on which features you use.

| File | Description | Size | Required |
|------|-------------|------|----------|
| `IPLROM.DAT` | IPL ROM (Original ~ XVI) | 131,072 bytes | Yes |
| `CGROM.DAT` | Character Generator ROM | 786,432 bytes | Yes |
| `IPLROM30.DAT` | IPL ROM (X68030 mode) | 131,072 bytes | Optional |
| `SCSIINROM.DAT` | Internal SCSI boot IPL ROM | 8,192 bytes | Optional (booting from internal SCSI) |
| `SCSIEXROM.DAT` | External SCSI board (CZ-6BS1) boot IPL ROM | 8,192 bytes | Optional (booting from external SCSI) |

Location: `~/Library/Application Support/MX68K/bios/`

## 2. 初回起動 / First Launch

1. `MX68K.app` を起動します。
2. BIOSファイルが未設定の場合、設定画面が自動的に表示されます。「BIOS」タブでIPL-ROM/CG-ROMのパスを指定してください。
3. BIOS設定が完了すると、エミュレータのメイン画面が表示され、エミュレーションが開始します。

初回はフロッピーディスクが何も挿入されていない状態で起動します。X68000本体だけが起動し、`SASI/SCSI IOCS`のみが動く状態（実機と同じ）になるため、Human68kを起動するには次の手順でディスクイメージをマウントしてください。

**English:**

1. Launch `MX68K.app`.
2. If the BIOS files are not yet configured, the settings screen opens automatically. Specify the IPL-ROM/CG-ROM paths under the "BIOS" tab.
3. Once BIOS setup is complete, the main emulator screen appears and emulation starts.

On first launch, no floppy disk is inserted. Only the X68000 hardware boots, with just `SASI/SCSI IOCS` running (identical to a real machine). To boot Human68k, mount a disk image using the steps below.

## 3. ディスクイメージのマウント / Mounting Disk Images

### フロッピーディスク（FDD0/FDD1） / Floppy Disks (FDD0/FDD1)

以下のいずれかの方法でマウントできます。

- **ドラッグ&ドロップ** — ディスクイメージをエミュレータウィンドウにドロップ（FDD0が空ならFDD0へ、埋まっていればFDD1へ自動マウント）
- **ツールバー** — 上部のフォルダアイコンからファイル選択
- **File メニュー** — 「FD ドライブ (FD0)」/「FD ドライブ (FD1)」→「挿入…」

対応形式: `.xdf` `.dim` `.d88` `.hdm` `.2hd` `.img` `.zip`（複数イメージを含むzipはマウント時に選択シートが表示されます）

イジェクトはツールバーのイジェクトボタン、または File メニューから行えます。南京錠アイコンで書込み禁止（ライトプロテクト）の切り替えも可能です。

**English:**

You can mount a floppy disk image in any of the following ways:

- **Drag & Drop** — drop a disk image onto the emulator window (mounts to FDD0 if empty, otherwise FDD1)
- **Toolbar** — use the folder icon at the top to choose a file
- **File menu** — "FD Drive (FD0)" / "FD Drive (FD1)" → "Insert…"

Supported formats: `.xdf` `.dim` `.d88` `.hdm` `.2hd` `.img` `.zip` (a selection sheet appears when mounting a zip containing multiple images).

Eject via the toolbar's eject button or the File menu. The padlock icon toggles write protection.

### ハードディスク（SASI/SCSI）・MO・CD-ROM / Hard Disks (SASI/SCSI), MO, CD-ROM

これらは設定画面（⌘,）の各タブから設定します。メイン画面へのドラッグ&ドロップは受け付けません（実機の構造上、フロッピーとは別の仕組みのため）が、**各タブ内のディスク行へは直接ドラッグ&ドロップでマウントできます**。

| タブ | 内容 |
|---|---|
| SASI | SASI HDD（最大8台）。無印〜PRO世代のマシン構成で使用 |
| SCSI | 内蔵SCSI HDD・外付けSCSI・**MOドライブ（ID5固定）**・**CD-ROMドライブ（ID6固定、`.iso`イメージ、Mode1のみ）** |

MOおよびCD-ROMをゲスト側から利用するには、`SUSIE.X`等のサードパーティ製SCSIデバイスドライバをゲスト側のディスクに用意し、常駐させる必要があります（実機と同じ仕様で、純正BIOSにはMO/CD-ROM用のブロックデバイスドライバが含まれていません）。MO・CD-ROMともライブ media 交換（実行中の挿抜）に対応していますが、CD-ROMは音声トラック(CD-DA)およびCDブートには対応していません。

新規の空FD/SASI HDD/SCSI HDD/MOイメージは、**Tools メニュー → Create Image** から作成できます（CD-ROMは読み取り専用メディアのため対象外です）。

**English:**

These are configured from the corresponding tabs in the settings window (⌘,). Drag & drop onto the main window is not supported for these (a real X68000 handles them through a different mechanism than floppies), but **you can drag & drop directly onto the disk row inside each settings tab**.

| Tab | Contents |
|---|---|
| SASI | SASI HDDs (up to 8). Used on the Original ~ PRO generation machine configurations |
| SCSI | Internal SCSI HDD, external SCSI, **MO drive (fixed ID5)**, **CD-ROM drive (fixed ID6, `.iso` image, Mode1 only)** |

To use MO or CD-ROM from the guest side, a third-party SCSI device driver such as `SUSIE.X` must be present and resident on a guest disk (this matches real hardware — the genuine BIOS does not include a block-device driver for MO/CD-ROM). Both MO and CD-ROM support live media swap (insert/eject while running), but CD-ROM does not support audio tracks (CD-DA) or CD-boot.

New blank FD/SASI HDD/SCSI HDD/MO images can be created from **Tools menu → Create Image** (CD-ROM is excluded, as it is a read-only medium).

## 4. 基本操作 / Basic Operation

### ツールバー・ステータスバー / Toolbar & Status Bar

ウィンドウ上部にリセット（ハード/ソフト）・Interrupt（NMI相当）・FDDの各種操作ボタンがあります。下部のステータスバーには実行速度・CPUクロック・メモリ容量・FDD/HDDのアクセスランプ・TIMER-LEDなどが表示されます。

**English:**

The top of the window has buttons for reset (hard/soft), Interrupt (NMI equivalent), and various FDD operations. The status bar at the bottom shows execution speed, CPU clock, memory size, FDD/HDD access lamps, the TIMER-LED, and more.

### キーボードショートカット / Keyboard Shortcuts

| 操作 | ショートカット |
|------|------|
| ハードリセット | ⌘R |
| ソフトリセット | ⌘⇧R |
| Interrupt（NMI） | ⌘⇧N |
| 一時停止 | ⌘P |
| ターボ切替 | ⌘⇧T |
| スクリーンショット | ⌘S（保存先: `~/Pictures/MX68K/`、設定変更可） |
| ステートセーブ | ⌘⌥S |
| ステートロード | ⌘⌥O |
| 設定を開く | ⌘, |
| フルスクリーン切替 | ^⌘F |
| 表示倍率 1x / 1.5x / 2x | ⌘1 / ⌘2 / ⌘3 |
| ソフトウェアキーボード | ⌘⌥K |
| マウスキャプチャ切替 | ⌘⌥M |

X68000本体はJIS配列キーボードを採用しているため、macOSのキーとX68000のキーの対応が一部異なります。主な対応は以下のとおりです（設定画面の「Input」タブでさらに個別リマップも可能）。

| X68000キー | macOSキー |
|-----------|-----------|
| XF1〜XF5 | F6〜F10 |
| HELP | F13 |
| ROLL UP | Page Up（HELPとは別キー） |
| COPY | F14 |
| BREAK | F15 |

**English:**

| Action | Shortcut |
|------|------|
| Hard reset | ⌘R |
| Soft reset | ⌘⇧R |
| Interrupt (NMI) | ⌘⇧N |
| Pause | ⌘P |
| Toggle turbo | ⌘⇧T |
| Screenshot | ⌘S (saved to `~/Pictures/MX68K/`, configurable) |
| State save | ⌘⌥S |
| State load | ⌘⌥O |
| Open settings | ⌘, |
| Toggle fullscreen | ^⌘F |
| Display scale 1x / 1.5x / 2x | ⌘1 / ⌘2 / ⌘3 |
| Software keyboard | ⌘⌥K |
| Toggle mouse capture | ⌘⌥M |

The X68000 uses a JIS keyboard layout, so some macOS keys map differently to X68000 keys. The main mappings are below (further individual remapping is available under the "Input" tab in settings):

| X68000 Key | macOS Key |
|-----------|-----------|
| XF1–XF5 | F6–F10 |
| HELP | F13 |
| ROLL UP | Page Up (a separate key from HELP) |
| COPY | F14 |
| BREAK | F15 |

### ゲームパッド / Gamepad

設定画面の「Input」タブから、ポート1/ポート2へ接続するコントローラを個別に選択できます。標準2ボタンに加え、CPSF-MD（6ボタン）・マジカルパッド（4ボタン）のプロファイルにも対応しています。

**English:**

From the "Input" tab in settings, you can individually choose the controller connected to Port 1 / Port 2. In addition to the standard 2-button pad, CPSF-MD (6-button) and Magical Pad (4-button) profiles are supported.

## 5. モニタパネル / Monitor Panels

Monitor メニューから、CPU・CRTC・ビデオコントローラ・BG・スプライト・サウンド（OPMシンセサイザー含む）・パレット・入力状態・ストレージ・MIDI・RTCなど、21種類のモニタパネルを個別に開けます（System / Processor / Device / Sound / Peripherals / Video / Renderer の7グループに分類）。いずれも読み取り専用の観測用ウィンドウです。

**English:**

From the Monitor menu, you can open any of 21 monitor panels individually — CPU, CRTC, video controller, BG, sprite, sound (including an OPM synthesizer view), palette, input state, storage, MIDI, RTC, and more (organized into 7 groups: System / Processor / Device / Sound / Peripherals / Video / Renderer). All are read-only observation windows.

## 6. トラブルシューティング / Troubleshooting

### 「開発元が未確認のため開けません」と表示される / "Cannot be opened because the developer cannot be verified"

本アプリはApple公証（notarization）を受けていません。以下のいずれかで起動できます。

1. `MX68K.app` を **右クリック（またはControl+クリック）→「開く」** を選択
2. それでも開けない場合は、システム設定 →「プライバシーとセキュリティ」の最下部に表示される「このまま開く」ボタンを押す
3. ターミナルで `xattr -cr /Applications/MX68K.app` を実行してから起動する

**English:**

This app is not notarized by Apple. You can launch it using any of the following:

1. **Right-click (or Control-click) `MX68K.app` → "Open"**
2. If that doesn't work, go to System Settings → "Privacy & Security" and click "Open Anyway" near the bottom of the page
3. Run `xattr -cr /Applications/MX68K.app` in Terminal, then launch it

### エミュレーションが起動しない・画面が真っ黒 / Emulation doesn't start / the screen is black

BIOSファイル（`IPLROM.DAT`/`CGROM.DAT`）が正しく配置・設定されているか、設定画面の「BIOS」タブで確認してください。

**English:**

Check that the BIOS files (`IPLROM.DAT`/`CGROM.DAT`) are correctly placed and configured, under the "BIOS" tab in settings.

### ディスクからHuman68kが起動しない / Human68k doesn't boot from disk

`human302.xdf`等、ブート可能なシステムディスクをFDD0にマウントしているか確認してください。HDD/SCSI起動の場合は、対応するタブでブート可能なイメージが正しいIDに設定されているか確認してください。

**English:**

Check that a bootable system disk (such as `human302.xdf`) is mounted in FDD0. For HDD/SCSI boot, check that a bootable image is set to the correct ID in the corresponding tab.
