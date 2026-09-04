import SwiftUI

struct AudioSettingsView: View {
    @EnvironmentObject var settingsViewModel: SettingsViewModel
    @EnvironmentObject var emulatorViewModel: EmulatorViewModel

    var body: some View {
        Form {
            Section(header: Text("Sound Settings")) {
                Toggle("Sound Enabled", isOn: $settingsViewModel.audioEnabled)
                    .onChange(of: settingsViewModel.audioEnabled) { newValue in
                        emulatorViewModel.setSoundEnabled(newValue)
                    }
                // P581: 3 本の音量行を `LabeledContent` へ統一する。grouped Form の
                // ラベル列へ自動整列するので、上下の `Toggle`/`Picker` とも縦位置が
                // 揃い、値表示も固定幅(52pt・等幅数字)でスライダー右端が一直線になる。
                // ラベルは「Volume」だと何の音量か分からないため「Master Volume」へ改名。
                LabeledContent("Master Volume") {
                    HStack {
                        Slider(value: $settingsViewModel.audioVolume, in: 0...1)
                            .onChange(of: settingsViewModel.audioVolume) { newValue in
                                emulatorViewModel.setVolume(newValue)
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
                                emulatorViewModel.setAdpcmVolume(settingsViewModel.adpcmVolume)
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
                                emulatorViewModel.setOpmVolume(settingsViewModel.opmVolume)
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

                    // ★空リスト時のガード: デバイス一覧は Core 側が MIDI_Init() 実行時に
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
                }
            }
        }
        // P580 — macOS 標準のグループスタイル(Form 自体がスクロール可能)。
        // MIDI 有効時に既定高さを超えるこのタブは、P579 の外側 `ScrollView` ではなく
        // このスタイルでスクロールする。
        .formStyle(.grouped)
        .onAppear { emulatorViewModel.refreshMidiDeviceList() }
    }
}
