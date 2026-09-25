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
   設定画面の「BIOS」タブからFilesアプリ経由でIPLROM.DAT/CGROM.DAT等を
   選択します。設定画面はBIOS未設定時には起動時に全画面で表示され、設定後は
   画面上の操作帯にある歯車ボタンから開けます（iOS版にはメニューバーは
   ありません）。選択したファイルはアプリ内のストレージへ自動的にコピー
   されます。
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
   iOS build's Settings screen has a "BIOS" tab that opens the Files app to
   pick `IPLROM.DAT`/`CGROM.DAT` etc. The Settings screen appears full-screen
   at launch while BIOS files are not yet configured; afterwards, open it
   with the gear button in the on-screen control bar (the iOS build has no
   menu bar). Selected files are copied into the app's own storage
   automatically.
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
| `IPLROM.DAT` | IPL ROM (Original–XVI) | 131,072 bytes | Yes |
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
- **File メニュー** — 「FD ドライブ (FD0)」/「FD ドライブ (FD1)」→「挿入…」（書込み禁止の状態でマウントする場合は「挿入(書込禁止)…」）

対応形式: `.xdf` `.dim` `.d88` `.hdm` `.2hd` `.img` `.zip`（複数イメージを含むzipはマウント時に選択シートが表示されます）

イジェクトはツールバーのイジェクトボタン、または File メニューから行えます。南京錠アイコンで書込み禁止（ライトプロテクト）の切り替えも可能です。

**FD2/FD3（外付けFDDユニット）:** 設定画面の「Hardware」タブで「外付け FDD ユニット(ドライブ 2 / 3)」をオンにし、「適用」後にハードリセット（⌘R）すると、FD2/FD3の2台が追加されます（実機で外付けFDDユニットを接続した状態に相当し、オフのときはHuman68kにC:/D:ドライブが現れません）。FD2/FD3の挿入・イジェクトは File メニューの「FD ドライブ (FD2)」/「FD ドライブ (FD3)」からのみ行えます（ツールバー・ドラッグ&ドロップは FD0/FD1 のみ対応）。

**English:**

You can mount a floppy disk image in any of the following ways:

- **Drag & Drop** — drop a disk image onto the emulator window (mounts to FDD0 if empty, otherwise FDD1)
- **Toolbar** — use the folder icon at the top to choose a file
- **File menu** — "FD Drive (FD0)" / "FD Drive (FD1)" → "Insert…" (use "Insert (Write-Protected)…" to mount it write-protected)

Supported formats: `.xdf` `.dim` `.d88` `.hdm` `.2hd` `.img` `.zip` (a selection sheet appears when mounting a zip containing multiple images).

Eject via the toolbar's eject button or the File menu. The padlock icon toggles write protection.

**FD2/FD3 (external FDD unit):** turn on "External FDD Unit (drives 2 and 3)" in the "Hardware" settings tab, press "Apply", then perform a hard reset (⌘R) to add two more drives, FD2/FD3 (this corresponds to attaching an external FDD unit to a real machine; when it is off, Human68k shows no C:/D: drive). FD2/FD3 can be inserted/ejected only from the File menu's "FD Drive (FD2)" / "FD Drive (FD3)" (the toolbar and drag & drop support FD0/FD1 only).

### ハードディスク（SASI/SCSI）・MO・CD-ROM / Hard Disks (SASI/SCSI), MO, CD-ROM

これらは設定画面（⌘,）の各タブから設定します。メイン画面へのドラッグ&ドロップは受け付けません（実機の構造上、フロッピーとは別の仕組みのため）が、**各タブ内のディスク行へは直接ドラッグ&ドロップでマウントできます**。

| タブ | 内容 |
|---|---|
| SASI | SASI HDD（最大8台）。無印〜PRO世代のマシン構成で使用 |
| SCSI | 内蔵SCSI HDD・外付けSCSI・**MOドライブ（ID5固定）**・**CD-ROMドライブ（ID6固定、`.iso`イメージ、Mode1のみ）** |

MOおよびCD-ROMをゲスト側から利用するには、`SUSIE.X`等のサードパーティ製SCSIデバイスドライバをゲスト側のディスクに用意し、常駐させる必要があります（実機と同じ仕様で、純正BIOSにはMO/CD-ROM用のブロックデバイスドライバが含まれていません）。MO・CD-ROMともライブ媒体交換（実行中の挿抜）に対応していますが、CD-ROMは音声トラック(CD-DA)およびCDブートには対応していません。

新規の空FD/SASI HDD/SCSI HDD/MOイメージは、**Tools メニュー → Create Image** から作成できます（CD-ROMは読み取り専用メディアのため対象外です）。

**English:**

These are configured from the corresponding tabs in the settings window (⌘,). Drag & drop onto the main window is not supported for these (a real X68000 handles them through a different mechanism than floppies), but **you can drag & drop directly onto the disk row inside each settings tab**.

| Tab | Contents |
|---|---|
| SASI | SASI HDDs (up to 8). Used on the Original–PRO generation machine configurations |
| SCSI | Internal SCSI HDD, external SCSI, **MO drive (fixed ID5)**, **CD-ROM drive (fixed ID6, `.iso` image, Mode1 only)** |

To use MO or CD-ROM from the guest side, a third-party SCSI device driver such as `SUSIE.X` must be present and resident on a guest disk (this matches real hardware — the genuine BIOS does not include a block-device driver for MO/CD-ROM). Both MO and CD-ROM support live media swap (insert/eject while running), but CD-ROM does not support audio tracks (CD-DA) or CD-boot.

New blank FD/SASI HDD/SCSI HDD/MO images can be created from **Tools menu → Create Image** (CD-ROM is excluded, as it is a read-only medium).

## 4. 基本操作 / Basic Operation

### ツールバー・ステータスバー / Toolbar & Status Bar

ウィンドウ上部にリセット（ハード/ソフト）・Interrupt（NMI相当）・FDDの各種操作ボタンがあります。下部のステータスバーには実行速度・CPUクロック・メモリ容量・FDD/HDDのアクセスランプ・TIMER-LEDなどが表示されます。

ステータスバー右端には電源ランプと電源ボタンがあります。電源ボタンを押すと、実機の電源OFFに近い形で約4秒かけて画面が暗転し（その間は電源ランプが点滅）、エミュレーションが停止します（ランプは赤）。もう一度押すと電源ONとなり、コールドブートします。また、ゲストソフト側からソフトウェアで電源OFFが要求された場合も自動的に検出し、電源ボタンを押したときと同じ電源OFF処理を行います。

**English:**

The top of the window has buttons for reset (hard/soft), Interrupt (NMI equivalent), and various FDD operations. The status bar at the bottom shows execution speed, CPU clock, memory size, FDD/HDD access lamps, the TIMER-LED, and more.

The right end of the status bar has a power lamp and a power button. Pressing the power button approximates a real machine's power-off: the screen fades to black over about 4 seconds (the power lamp blinks meanwhile), then emulation stops (the lamp turns red). Press it again to power on with a cold boot. A software power-off request from the guest is also detected automatically and handled the same way as pressing the power button.

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
| クイックセーブ | ⌘⇧S（`~/Documents/MX68K/states/`へファイル名を自動で付けて即保存） |
| クイックロード | ⌘⇧O（直近に保存したステートを即読込） |
| 録画開始/停止 | ショートカットなし（Emulator メニューの「録画開始」/「録画停止」、保存先: `~/Movies/MX68K/`、設定変更可） |
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
| Quick save | ⌘⇧S (saves instantly to `~/Documents/MX68K/states/` with an auto-generated file name) |
| Quick load | ⌘⇧O (instantly loads the most recently saved state) |
| Start/stop recording | No shortcut ("Start Recording" / "Stop Recording" in the Emulator menu; saved to `~/Movies/MX68K/`, configurable) |
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

## 5. iOS版の操作 / Operating the iOS Version

iOS版にはメニューバー・キーボードショートカットが無く、画面上の操作帯と設定画面から操作します。

### 操作帯 / Control Bar

縦向きでは画面上部に操作ボタン、下部に状態表示（CPU・メモリ・実行速度・FD0/FD1/HDDのアクセスランプ）が並びます。横向きなど左右に十分な余白がある場合は、左側に操作ボタン、右側に状態表示が縦に並びます。操作ボタンは左（上）から次の順です。

| ボタン | 動作 |
|---|---|
| ハードリセット | 確認ダイアログの後にハードリセット |
| ソフトリセット | 即時にソフトリセット |
| Interrupt | NMI相当（即時実行） |
| 一時停止/再開 | エミュレーションの一時停止・再開 |
| FD0… / FD1… | 「選択…」でFilesアプリからディスクイメージを選んでマウント、「取り出し」でイジェクト |
| ステート | 「ステートを保存」で即時保存、「ステートを読込…」で保存済み一覧を表示 |
| ソフトウェアキーボード | 画面下部のソフトウェアキーボードの表示/非表示（画面幅が足りる場合のみ表示） |
| 仮想パッド | 仮想パッドの表示/非表示 |
| タッチマウス | タッチマウスの有効/無効 |
| 歯車 | 設定画面を開く |

### タッチ操作・キーボード / Touch Controls & Keyboard

- **仮想パッド** — 画面上のジョイスティックとトリガーボタンでジョイスティック入力を行います。画面幅が選択中のトリガーボタンのサイズに対して足りない場合（iPhone縦向きでサイズを大きくしたときなど）は表示されません。
- **タッチマウス** — 有効にすると画面全体がトラックパッドになります（1本指ドラッグ=マウス移動、1本指タップ=左クリック、2本指タップ=右クリック）。タッチマウスが有効な間は仮想パッドは表示されません。
- **ソフトウェアキーボード** — iPadやiPhone横向きなど、十分な画面幅がある場合に画面下部へ重ねて表示できます。
- **物理キーボード** — Bluetooth等で接続したハードウェアキーボードから入力できます（押し続けるとキーリピートします）。

仮想パッドの不透明度・トリガーボタンのサイズ（Small/Medium/Large）・A/Bボタンの連射（オートファイア）・A/Bボタンの入れ替え、およびソフトウェアキーボードの不透明度は、設定画面「Hardware」タブの「タッチ操作」セクションで変更できます（即時反映）。

### 設定画面 / Settings

iOS版の設定画面は **BIOS / Hardware / Audio / SASI / SCSI** の5タブ構成です（macOS版にある General / Input / Windrv タブはありません）。macOS版では Emulator メニュー（⌘⇧T）で行うターボのON/OFFは、iOS版では「Hardware」タブの「高速化オプション」にあるボタンで切り替えます。設定の変更は「適用」で反映されます。

### ディスクイメージ / Disk Images

- **フロッピー（FD0/FD1）** — 操作帯の「FD0…」/「FD1…」→「選択…」でFilesアプリからイメージを選びます。選択したファイルはアプリ内のストレージへコピーされてからマウントされます。`.zip`を選ぶと中のFDイメージを展開してマウントします（複数イメージを含む場合は選択シートが表示されます）。
- **HDD（SASI/SCSI）・MO・CD-ROM** — 設定画面の「SASI」/「SCSI」タブで選択します。大容量イメージのコピーを避けるため、これらは**アプリ内へコピーせず、選択した元の場所のファイルを直接参照**します（参照情報はアプリ再起動後も保持されます）。そのため、マウント中は元のファイルを移動・削除しないでください。

### ステートセーブ/ロード / State Save & Load

操作帯の「ステート」→「ステートを保存」で、ファイル名（日時）を自動で付けて即座に保存します。「ステートを読込…」を選ぶと保存済みステートの一覧（新しい順）が表示され、タップで読込、スワイプで削除できます。保存先はアプリ内部の領域（`Library/Application Support/MX68K/states/`）で、Filesアプリには表示されません。

**English:**

The iOS version has no menu bar or keyboard shortcuts; it is operated from the on-screen control bar and the Settings screen.

### Control Bar

In portrait, the control buttons run along the top of the screen and the status display (CPU, memory, execution speed, FD0/FD1/HDD access lamps) along the bottom. When there is enough margin on the left and right (e.g. in landscape), the control buttons are stacked on the left and the status display on the right. The control buttons appear in this order, from left (or top):

| Button | Action |
|---|---|
| Hard Reset | Hard reset, after a confirmation dialog |
| Soft Reset | Immediate soft reset |
| Interrupt | NMI equivalent (runs immediately) |
| Pause/Resume | Pauses or resumes emulation |
| FD0… / FD1… | "Select…" picks a disk image from the Files app and mounts it; "Eject" ejects it |
| State | "Save State" saves instantly; "Load State…" shows the list of saved states |
| Software Keyboard | Shows/hides the software keyboard at the bottom of the screen (shown only when the screen is wide enough) |
| Virtual Pad | Shows/hides the virtual pad |
| Touch Mouse | Enables/disables the touch mouse |
| Gear | Opens the Settings screen |

### Touch Controls & Keyboard

- **Virtual Pad** — an on-screen joystick with trigger buttons for joystick input. It is not shown when the screen is too narrow for the selected trigger-button size (e.g. an iPhone in portrait with a larger button size).
- **Touch Mouse** — when enabled, the whole screen acts as a trackpad (one-finger drag = move the mouse, one-finger tap = left click, two-finger tap = right click). The virtual pad is hidden while the touch mouse is enabled.
- **Software Keyboard** — can be overlaid at the bottom of the screen when it is wide enough, e.g. on iPad or an iPhone in landscape.
- **Physical Keyboard** — you can type on a hardware keyboard connected via Bluetooth etc. (holding a key down repeats it).

The virtual pad's opacity, the trigger-button size (Small/Medium/Large), A/B auto-fire, A/B button swap, and the software keyboard's opacity can be changed in the "Touch Controls" section of the "Hardware" settings tab (changes apply immediately).

### Settings

The iOS Settings screen has five tabs: **BIOS / Hardware / Audio / SASI / SCSI** (the General / Input / Windrv tabs of the macOS version are not available). Turbo, which the macOS version toggles from the Emulator menu (⌘⇧T), is toggled on iOS with the button in the "Speed-up Options" section of the "Hardware" tab. Settings changes take effect when you tap "Apply".

### Disk Images

- **Floppy disks (FD0/FD1)** — tap "FD0…" / "FD1…" → "Select…" in the control bar and pick an image from the Files app. The selected file is copied into the app's own storage and then mounted. Picking a `.zip` extracts and mounts the FD image inside it (a selection sheet appears if it contains multiple images).
- **HDD (SASI/SCSI), MO, CD-ROM** — selected from the "SASI" / "SCSI" settings tabs. To avoid copying large images, these are **not copied into the app; the file is used directly from the location you selected** (the reference is kept across app restarts). Do not move or delete the original file while it is mounted.

### State Save & Load

"State" → "Save State" in the control bar saves instantly, with a file name generated from the date and time. "Load State…" shows the list of saved states (newest first): tap one to load it, or swipe to delete it. States are stored in the app's internal area (`Library/Application Support/MX68K/states/`) and do not appear in the Files app.

## 6. モニタパネル / Monitor Panels

Monitor メニューから、CPU・CRTC・ビデオコントローラ・BG・スプライト・サウンド（OPMシンセサイザー含む）・パレット・入力状態・ストレージ・MIDI・RTC・逆アセンブル・DMAC・割込みレジスタなど、26種類のモニタパネルを個別に開けます（System / Processor / Device / Sound / Peripherals / Video / Renderer の7グループに分類）。ほとんどは読み取り専用の観測用ウィンドウですが、Debugger（PCブレークポイント+シングルステップ実行）とLog Viewer（診断ログのライブ表示、記録のON/OFF切替）の2つは実行制御・操作機能を持ちます。

**English:**

From the Monitor menu, you can open any of 26 monitor panels individually — CPU, CRTC, video controller, BG, sprite, sound (including an OPM synthesizer view), palette, input state, storage, MIDI, RTC, disassembly, DMAC, interrupt registers, and more (organized into 7 groups: System / Processor / Device / Sound / Peripherals / Video / Renderer). Most are read-only observation windows, except for two: the Debugger (PC breakpoint + single-instruction stepping) and the Log Viewer (live diagnostic-log display with an on/off toggle for logging), which provide execution control and interactive operations.

## 7. トラブルシューティング / Troubleshooting

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
