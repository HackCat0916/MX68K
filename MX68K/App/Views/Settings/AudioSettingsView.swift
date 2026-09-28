import SwiftUI

// P761: iOS へ移植。macOS 側の実効コードパスは変更せず、`#if os(macOS)` 分岐を
// 追加するだけ(`SASISettingsView` が確立した「宣言自体を `#if` で囲む」流儀を踏襲)。
// Mercury Unit / MIDI ボード設定は iOS 側に CoreMIDI デバイス一覧取得等の基盤配線が
// 無いため、セクションごと `#if os(macOS)` で除外する(ユーザー合意済みのスコープ外)。
// P830: 上記のうち MIDI ボード・内蔵 MT-32 関連は iOS でも表示するよう分割した
// (除外は Mercury Unit・CoreMIDI デバイス選択のみ)。
// P831: 内蔵 SC-55 も iOS で表示するようにした(iOS では性能警告文を追加表示)。
struct AudioSettingsView: View {
    @EnvironmentObject var settingsViewModel: SettingsViewModel
    #if os(macOS)
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel
    #else
    @EnvironmentObject var iosViewModel: MX68KiOSViewModel
    #endif

    var body: some View {
        Form {
            Section(header: Text("Sound Settings")) {
                Toggle("Sound Enabled", isOn: $settingsViewModel.audioEnabled)
                    .onChange(of: settingsViewModel.audioEnabled) { newValue in
                        #if os(macOS)
                        emulatorViewModel.setSoundEnabled(newValue)
                        #else
                        iosViewModel.setSoundEnabled(newValue)
                        #endif
                    }
                // P581: 3 本の音量行を `LabeledContent` へ統一する。grouped Form の
                // ラベル列へ自動整列するので、上下の `Toggle`/`Picker` とも縦位置が
                // 揃い、値表示も固定幅(52pt・等幅数字)でスライダー右端が一直線になる。
                // ラベルは「Volume」だと何の音量か分からないため「Master Volume」へ改名。
                LabeledContent("Master Volume") {
                    HStack {
                        Slider(value: $settingsViewModel.audioVolume, in: 0...1)
                            .onChange(of: settingsViewModel.audioVolume) { newValue in
                                #if os(macOS)
                                emulatorViewModel.setVolume(newValue)
                                #else
                                iosViewModel.setVolume(newValue)
                                #endif
                            }
                        Text("\(Int(settingsViewModel.audioVolume * 100))%")
                            .frame(width: 52, alignment: .trailing)
                            .monospacedDigit()
                    }
                }
                // P512: チップ別音量(Core の ADPCM_SetVolume / OPM_SetVolume と同じ
                // 0-16 スケール)。上のマスター Volume が AudioEngine のポストミックス
                // ゲインなのに対し、こちらは Core の音量ステートを直接書き換える。
                // ドラッグ中に即座に音が変わる(ライブプレビュー)。config.json への
                // 永続化はこのタブ既存の流儀どおり画面下部「Apply」ボタンでのみ行う。
                LabeledContent("ADPCM Volume") {
                    HStack {
                        Slider(value: Binding(
                            get: { Double(settingsViewModel.adpcmVolume) },
                            set: { newValue in
                                settingsViewModel.adpcmVolume = Int(newValue.rounded())
                                #if os(macOS)
                                emulatorViewModel.setAdpcmVolume(settingsViewModel.adpcmVolume)
                                #else
                                iosViewModel.setAdpcmVolume(settingsViewModel.adpcmVolume)
                                #endif
                            }
                        ), in: 0...16, step: 1)
                        Text("\(settingsViewModel.adpcmVolume)/16")
                            .frame(width: 52, alignment: .trailing)
                            .monospacedDigit()
                    }
                }
                LabeledContent("OPM (FM) Volume") {
                    HStack {
                        Slider(value: Binding(
                            get: { Double(settingsViewModel.opmVolume) },
                            set: { newValue in
                                settingsViewModel.opmVolume = Int(newValue.rounded())
                                #if os(macOS)
                                emulatorViewModel.setOpmVolume(settingsViewModel.opmVolume)
                                #else
                                iosViewModel.setOpmVolume(settingsViewModel.opmVolume)
                                #endif
                            }
                        ), in: 0...16, step: 1)
                        Text("\(settingsViewModel.opmVolume)/16")
                            .frame(width: 52, alignment: .trailing)
                            .monospacedDigit()
                    }
                }
                Picker("Sample Rate", selection: $settingsViewModel.audioSampleRate) {
                    // P627: 選択肢を 6 値へ上方拡張(昇順)。値集合の真実源は Bridge 側
                    // mx68k_set_audio_sample_rate() のホワイトリストであり、ここは表示用。
                    // 62500 Hz は参照実装 XM6 本家の既定値で、fmgen の OPM レート換算が
                    // 整数で割り切れる(ピッチ誤差ゼロ)唯一のレート。
                    Text("22050 Hz").tag(22050)
                    Text("44100 Hz").tag(44100)
                    Text("48000 Hz").tag(48000)
                    Text("62500 Hz").tag(62500)
                    Text("88200 Hz").tag(88200)
                    Text("96000 Hz").tag(96000)
                }
                // P581: 注意記号を `⚠` へ統一。
                Text("⚠ Sample rate takes effect at next launch")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                // P761: Mercury Unit は macOS 限定。
                // P830: MIDI ボード・出力先・内蔵 MT-32・リセット/遅延設定は iOS でも表示する
                // (いずれも CoreMIDI デバイス一覧に依存しない)。CoreMIDI デバイス選択
                // だけを `#if os(macOS)` に残す(内蔵 SC-55 は P831 で iOS 対応)。
                #if os(macOS)
                Toggle("Mercury Unit (16-bit linear PCM)", isOn: $settingsViewModel.mercuryUnit)
                    .disabled(settingsViewModel.midiEnabled)
                // P581: 日本語リテラル直書き(英語モードでも日本語が出ていた)を
                // 「英語ソース + xcstrings の ja 訳」という他タブと同じ方式へ揃える。
                Text("⚠ Supports PCM 2ch + FM (YMF288) 6ch + SSG 3ch. FM and SSG can be inspected in the Sound Monitor. Takes effect after pressing Apply and then performing a hard reset (⌘R).")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                // P633: 実音出力が未検証である旨を正直に表示する(Docs/01 3-3 の
                // 「実 PCM 音は未検証」「実 PCM 音・実 FM 音とも未検証」と同趣旨)。
                Text("⚠ Wiring is complete, but neither real PCM nor real FM sound has been verified — no compatible software has been obtained yet.")
                    .font(.subheadline).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                #endif
                // P488: MIDI ボード(CZ-6BM1)。Mercury Unit と割込みレベル 4 を共有する
                // ため相互排他(SASI/SCSI 排他(P268)と同型)。
                Toggle("MIDI Board (CZ-6BM1)", isOn: $settingsViewModel.midiEnabled)
                    .disabled(settingsViewModel.mercuryUnit)
                // P581: 三項演算子で 2 つの文字列リテラルを渡すとオーバーロード解決が
                // `Text(S: StringProtocol)`(非ローカライズ)側へ倒れる余地があるため、
                // if/else で分けて必ず `Text(LocalizedStringKey)` になるようにする。
                if settingsViewModel.mercuryUnit {
                    Text("⚠ Cannot be installed together with the Mercury Unit — both use interrupt level 4. Disable the Mercury Unit first.")
                        .font(.subheadline).foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                } else {
                    // P633: 「何がどこまで確認済みか」を具体的に書き分ける。
                    // ボード検出とドライバ側の認識は hands-on 確認済み(Docs/01 3-2)、
                    // 実 MIDI 楽器での音出力だけが未検証。
                    Text("⚠ Both MIDI output and input are supported. Board detection and recognition by game/music drivers are confirmed, but sound output on a real MIDI instrument has not been verified.")
                        .font(.subheadline).foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }

                // P490: MIDI Stage 2 の設定群。
                // ★反映タイミングの設計判断: 個別の .onChange によるライブ反映は行わず、
                //   Mercury/MIDI 装着トグルと同じく「Apply ボタン → applySettings →
                //   pushConfig」の一本道に揃える(SettingsView.swift:58-60)。理由は
                //   (a) この設定画面の他の項目(サンプルレート・機種・メモリ等)が全て
                //   同じ経路であり、MIDI だけ挙動を変えると一貫性が崩れる、
                //   (b) リセット送信・音源種別は MIDI_Init() 呼出時にしか読まれないため、
                //   そもそもライブ反映の意味が無い(ハードリセット待ち)。
                //   なお送信遅延とデバイス選択は pushConfig 時点で即時効く
                //   (フレームループが g_midi_delay_ms を毎回読む / midOutChg を直接呼ぶ)。
                if settingsViewModel.midiEnabled {
                    // P825/P826: 出力先の排他選択(外部 CoreMIDI / 内蔵 MT-32 / 内蔵 SC-55)。
                    // P831: 内蔵 SC-55 は iOS でも選択できる(P830 までは macOS 専用だった)。
                    Picker("Output", selection: $settingsViewModel.midiOutputDestination) {
                        Text("External MIDI").tag(0)
                        Text("Internal MT-32").tag(1)
                        Text("Internal SC-55").tag(2)
                    }
                    // ★他の MIDI 設定(ハードリセットで反映)と異なり、内蔵音源の
                    //   有効/無効・ROM 選択は Apply 直後に反映される。
                    Text("⚠ Output and internal-synth ROM changes take effect immediately after pressing Apply — no hard reset needed.")
                        .font(.subheadline).foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    if settingsViewModel.midiOutputDestination == 1 {
                        mt32RomRows
                    }
                    if settingsViewModel.midiOutputDestination == 2 {
                        sc55RomRows
                    }

                    Picker("Reset Command", selection: $settingsViewModel.midiResetType) {
                        Text("LA (MT-32)").tag(0)
                        Text("GM").tag(1)
                        Text("GS").tag(2)
                        Text("XG").tag(3)
                    }
                    Toggle("Send Reset on Init", isOn: $settingsViewModel.midiResetOnInit)
                    Text("⚠ Takes effect after pressing Apply and then performing a hard reset (⌘R).")
                        .font(.subheadline).foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)

                    Stepper("Output Delay: \(settingsViewModel.midiDelayMs) ms",
                            value: $settingsViewModel.midiDelayMs, in: 0...1000, step: 10)
                    Text("⚠ For reference: XM6 defaults to 84 ms (28 ms in the TypeG build). 0 = no delay (the MX68K default).")
                        .font(.subheadline).foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)

                    #if os(macOS)
                    // ★空リスト時のガード:デバイス一覧は Core 側が MIDI_Init() 実行時に
                    //   埋めるため、MIDI を有効化した直後(ハードリセット前)や CoreMIDI に
                    //   ポートが 1 つも無い環境では空になる。空の Picker は選択肢ゼロで
                    //   操作不能かつ選択中インデックスの表示も破綻するため、案内文へ差し替える。
                    if emulatorViewModel.midiOutputDeviceNames.isEmpty
                        && emulatorViewModel.midiInputDeviceNames.isEmpty {
                        // 配線済みで一覧が空 = このMac自体にMIDIポートが無い。この場合は
                        // 何度ハードリセットしても一覧は増えないため、案内文を分ける。
                        if emulatorViewModel.midiWired {
                            Text("⚠ No MIDI destination is available on this Mac. Enable the IAC Driver in Audio MIDI Setup, or connect a MIDI interface.")
                                .font(.subheadline).foregroundColor(.secondary)
                                .fixedSize(horizontal: false, vertical: true)
                        } else {
                            Text("⚠ No MIDI devices detected. Reopen this tab after a hard reset (⌘R) and the list will be refreshed.")
                                .font(.subheadline).foregroundColor(.secondary)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    } else {
                        if !emulatorViewModel.midiOutputDeviceNames.isEmpty {
                            Picker("Output Device", selection: $settingsViewModel.midiOutDeviceIndex) {
                                ForEach(Array(emulatorViewModel.midiOutputDeviceNames.enumerated()),
                                        id: \.offset) { i, name in
                                    Text(name).tag(i)
                                }
                            }
                        }
                        if !emulatorViewModel.midiInputDeviceNames.isEmpty {
                            Picker("Input Device", selection: $settingsViewModel.midiInDeviceIndex) {
                                ForEach(Array(emulatorViewModel.midiInputDeviceNames.enumerated()),
                                        id: \.offset) { i, name in
                                    Text(name).tag(i)
                                }
                            }
                        }
                    }
                    #else
                    // P830: iOS には CoreMIDI デバイス選択の配線が無い(P761 の判断を継承)。
                    // config.json に外部 MIDI(0)が入っている場合でも表示が崩れないよう案内する。
                    if settingsViewModel.midiOutputDestination == 0 {
                        Text("⚠ External MIDI device selection is not available on iOS. Use Internal MT-32 or SC-55.")
                            .font(.subheadline).foregroundColor(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    #endif
                }
            }
        }
        // P580 — macOS 標準のグループスタイル(Form 自体がスクロール可能)。
        // MIDI 有効時に既定高さを超えるこのタブは、P579 の外側 `ScrollView` ではなく
        // このスタイルでスクロールする。
        .formStyle(.grouped)
        #if os(macOS)
        .onAppear { emulatorViewModel.refreshMidiDeviceList() }
        #endif
    }

    // ---- P825: 内蔵 MT-32 の ROM 選択 ----
    // P830: macOS/iOS 共通(`settingsViewModel` と共通 Bridge 関数
    // `mx68k_mt32_get_status()` のみに依存し、`emulatorViewModel` を参照しない)。
    // サイズは目安表示専用(BIOSSettingsView の validationColor/validationText と同型だが、
    // CM-32L PCM のように正当なサイズが複数あるため厳密一致の合否判定はしない)。
    // 実際の受理可否は mt32emu の SHA1 識別と open_synth の結果(下の状態行)で決まる。
    private let mt32ControlRomSizes: [Int64] = [65_536]
    private let mt32PcmRomSizes: [Int64] = [524_288, 1_048_576]   // MT-32 / CM-32L

    @ViewBuilder
    private var mt32RomRows: some View {
        HStack {
            Text("MT-32 Control ROM")
            TextField("", text: $settingsViewModel.mt32ControlRomPath)
            FilePickerButton(titleKey: "Browse...", destination: .mt32ControlRom) { path in
                settingsViewModel.mt32ControlRomPath = path
            }
        }
        HStack {
            Text("MT-32 PCM ROM")
            TextField("", text: $settingsViewModel.mt32PcmRomPath)
            FilePickerButton(titleKey: "Browse...", destination: .mt32PcmRom) { path in
                settingsViewModel.mt32PcmRomPath = path
            }
        }
        // P827: mt32emu_set_partial_count。次回の再構成(Apply)時にのみ反映される。
        Stepper("Max Partials: \(settingsViewModel.mt32PartialCount)",
                value: $settingsViewModel.mt32PartialCount, in: 1...256)
        Text("⚠ Takes effect after pressing Apply. 32 (the default) matches the real MT-32 hardware. Increasing it can reduce notes cutting off when too many partials play at once, but no longer matches real hardware behavior.")
            .font(.subheadline).foregroundColor(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        mt32RomInfoRow(path: settingsViewModel.mt32ControlRomPath,
                       expected: mt32ControlRomSizes, name: String(localized: "Control ROM"))
        mt32RomInfoRow(path: settingsViewModel.mt32PcmRomPath,
                       expected: mt32PcmRomSizes, name: String(localized: "PCM ROM"))
        TimelineView(.periodic(from: .now, by: 1.0)) { _ in
            mt32StatusRow(mx68k_mt32_get_status())
        }
        Text("⚠ Roland MT-32 / CM-32L ROMs are not included. Select your own Control and PCM ROM files; the file names do not matter (the ROM type is identified by its contents).")
            .font(.subheadline).foregroundColor(.secondary)
            .fixedSize(horizontal: false, vertical: true)
    }

    private func mt32FileSize(of path: String) -> Int64? {
        guard !path.isEmpty else { return nil }
        let attrs = try? FileManager.default.attributesOfItem(atPath: path)
        return attrs?[.size] as? Int64
    }

    @ViewBuilder
    private func mt32RomInfoRow(path: String, expected: [Int64], name: String) -> some View {
        if path.isEmpty {
            Text("\(name): Not selected")
                .font(.subheadline).foregroundColor(.secondary)
        } else if let size = mt32FileSize(of: path) {
            if expected.contains(size) {
                Text("\(name): \(size) bytes")
                    .font(.subheadline).foregroundColor(.secondary)
            } else {
                Text("\(name): \(size) bytes — unusual size for this ROM type (it is accepted only if MT-32 emulation recognizes it)")
                    .font(.subheadline).foregroundColor(.orange)
                    .fixedSize(horizontal: false, vertical: true)
            }
        } else {
            Text("\(name): File not found")
                .font(.subheadline).foregroundColor(.red)
        }
    }

    // mx68k_mt32_get_status(): 0=無効, 1=動作中, 2=適用待ち, 負値=MT32EMU_RC_* 失敗コード。
    @ViewBuilder
    private func mt32StatusRow(_ status: Int32) -> some View {
        switch status {
        case 1:
            Text("MT-32 status: Running").font(.subheadline).foregroundColor(.green)
        case 2:
            Text("MT-32 status: Applying…").font(.subheadline).foregroundColor(.secondary)
        case 0:
            Text("MT-32 status: Not loaded (press Apply)").font(.subheadline).foregroundColor(.secondary)
        case -1:
            Text("MT-32 status: Error — ROM not recognized").font(.subheadline).foregroundColor(.red)
        case -2:
            Text("MT-32 status: Error — ROM file not found").font(.subheadline).foregroundColor(.red)
        case -4:
            Text("MT-32 status: Error — both a Control ROM and a PCM ROM are required").font(.subheadline).foregroundColor(.red)
        case -7:
            Text("MT-32 status: Error — the Control ROM and PCM ROM do not match").font(.subheadline).foregroundColor(.red)
        default:
            Text("MT-32 status: Error (code \(status))").font(.subheadline).foregroundColor(.red)
        }
    }

    // ---- P826: 内蔵 SC-55 の ROM 選択 ----
    // P831: Nuked-SC55 コアを iOS ターゲットにも追加したため、プラットフォーム共通。
    // サイズは目安表示専用(MT-32 と同じ方針)。実際の受理可否は Nuked-SC55 の読込結果
    // (下の状態行)で決まる。rom2 は 256KB / 512KB のどちらも受理される。
    private let sc55Rom1Sizes: [Int64] = [32_768]
    private let sc55Rom2Sizes: [Int64] = [262_144, 524_288]
    private let sc55WaveRomSizes: [Int64] = [1_048_576]

    @ViewBuilder
    private var sc55RomRows: some View {
        HStack {
            Text("SC-55 ROM 1")
            TextField("", text: $settingsViewModel.sc55Rom1Path)
            FilePickerButton(titleKey: "Browse...", destination: .sc55Rom1) { path in
                settingsViewModel.sc55Rom1Path = path
            }
        }
        HStack {
            Text("SC-55 ROM 2")
            TextField("", text: $settingsViewModel.sc55Rom2Path)
            FilePickerButton(titleKey: "Browse...", destination: .sc55Rom2) { path in
                settingsViewModel.sc55Rom2Path = path
            }
        }
        HStack {
            Text("SC-55 Wave ROM 1")
            TextField("", text: $settingsViewModel.sc55WaveRom1Path)
            FilePickerButton(titleKey: "Browse...", destination: .sc55WaveRom1) { path in
                settingsViewModel.sc55WaveRom1Path = path
            }
        }
        HStack {
            Text("SC-55 Wave ROM 2")
            TextField("", text: $settingsViewModel.sc55WaveRom2Path)
            FilePickerButton(titleKey: "Browse...", destination: .sc55WaveRom2) { path in
                settingsViewModel.sc55WaveRom2Path = path
            }
        }
        HStack {
            Text("SC-55 Wave ROM 3")
            TextField("", text: $settingsViewModel.sc55WaveRom3Path)
            FilePickerButton(titleKey: "Browse...", destination: .sc55WaveRom3) { path in
                settingsViewModel.sc55WaveRom3Path = path
            }
        }
        sc55RomInfoRow(path: settingsViewModel.sc55Rom1Path,
                       expected: sc55Rom1Sizes, name: String(localized: "ROM 1"))
        sc55RomInfoRow(path: settingsViewModel.sc55Rom2Path,
                       expected: sc55Rom2Sizes, name: String(localized: "ROM 2"))
        sc55RomInfoRow(path: settingsViewModel.sc55WaveRom1Path,
                       expected: sc55WaveRomSizes, name: String(localized: "Wave ROM 1"))
        sc55RomInfoRow(path: settingsViewModel.sc55WaveRom2Path,
                       expected: sc55WaveRomSizes, name: String(localized: "Wave ROM 2"))
        sc55RomInfoRow(path: settingsViewModel.sc55WaveRom3Path,
                       expected: sc55WaveRomSizes, name: String(localized: "Wave ROM 3"))
        TimelineView(.periodic(from: .now, by: 1.0)) { _ in
            sc55StatusRow(mx68k_sc55_get_status())
        }
        Text("⚠ Roland SC-55 (mk1) ROMs are not included. Select your own five ROM files (typically sc55_rom1.bin, sc55_rom2.bin, sc55_waverom1.bin–sc55_waverom3.bin).")
            .font(.subheadline).foregroundColor(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        Text("⚠ The SC-55 is silent for about 0.3 seconds right after it is enabled and right after a hard reset, while its firmware starts up.")
            .font(.subheadline).foregroundColor(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        Text("⚠ For the SC-55, setting Reset Command to GS is recommended (the default, LA, is for the MT-32).")
            .font(.subheadline).foregroundColor(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        #if os(iOS)
        Text("⚠ The SC-55 is a full firmware emulation and is CPU-intensive. On some iPad models it may stutter or drop notes — if so, switch to Internal MT-32.")
            .font(.subheadline).foregroundColor(.orange)
            .fixedSize(horizontal: false, vertical: true)
        #endif
    }

    @ViewBuilder
    private func sc55RomInfoRow(path: String, expected: [Int64], name: String) -> some View {
        if path.isEmpty {
            Text("\(name): Not selected")
                .font(.subheadline).foregroundColor(.secondary)
        } else if let size = mt32FileSize(of: path) {
            if expected.contains(size) {
                Text("\(name): \(size) bytes")
                    .font(.subheadline).foregroundColor(.secondary)
            } else {
                Text("\(name): \(size) bytes — unexpected size for this SC-55 ROM (it will likely be rejected)")
                    .font(.subheadline).foregroundColor(.orange)
                    .fixedSize(horizontal: false, vertical: true)
            }
        } else {
            Text("\(name): File not found")
                .font(.subheadline).foregroundColor(.red)
        }
    }

    // mx68k_sc55_get_status(): 0=未読込/無効, 1=動作中, 2=適用中(warm-up 中),
    // -1=ROM ファイルが無い/読めない, -2=ROM のサイズ不正。
    @ViewBuilder
    private func sc55StatusRow(_ status: Int32) -> some View {
        switch status {
        case 1:
            Text("SC-55 status: Running").font(.subheadline).foregroundColor(.green)
        case 2:
            Text("SC-55 status: Starting up…").font(.subheadline).foregroundColor(.secondary)
        case 0:
            Text("SC-55 status: Not loaded (press Apply)").font(.subheadline).foregroundColor(.secondary)
        case -1:
            Text("SC-55 status: Error — a ROM file was not found or could not be read").font(.subheadline).foregroundColor(.red)
        case -2:
            Text("SC-55 status: Error — a ROM file has the wrong size").font(.subheadline).foregroundColor(.red)
        default:
            Text("SC-55 status: Error (code \(status))").font(.subheadline).foregroundColor(.red)
        }
    }
}
