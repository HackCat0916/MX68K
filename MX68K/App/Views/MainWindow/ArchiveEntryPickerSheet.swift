//
//  ArchiveEntryPickerSheet.swift
//  MX68K
//
//  P554 — zip 内にディスクイメージが 2 個以上見つかったときに表示する選択シート。
//  「どれをマウントするか」だけを扱い、記憶(次回同じ zip で自動選択)は
//  本サイクルのスコープ外(将来検討)。
//
//  提示は EmulatorView 側(viewModel.showArchivePicker)が担当する。
//  確定/キャンセルはコールバックで EmulatorViewModel へ戻し、キャンセル時は
//  展開済み一時ディレクトリを破棄する。
//

import SwiftUI

struct ArchiveEntryPickerSheet: View {
    let images: [URL]
    let onSelect: (URL) -> Void
    let onCancel: () -> Void

    /// 既定で先頭を選択しておき、「マウント」が最初から押せる状態にする。
    @State private var selection: URL?

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("Select a disk image")
                .font(.headline)
                .padding(.horizontal, 16)
                .padding(.top, 16)

            Text("The archive contains multiple disk images. Choose the one to mount.")
                .font(.caption)
                .foregroundColor(.secondary)
                .padding(.horizontal, 16)
                .padding(.top, 4)
                .padding(.bottom, 10)

            List(images, id: \.self, selection: $selection) { url in
                HStack {
                    Text(url.lastPathComponent)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    Spacer()
                    Text(Self.fileSizeText(url))
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                .contentShape(Rectangle())
                // ダブルクリックでも確定できるようにする(単一クリックは選択のみ)。
                .onTapGesture(count: 2) { onSelect(url) }
            }

            Divider()

            HStack {
                Spacer()
                Button("Cancel") { onCancel() }
                    .keyboardShortcut(.cancelAction)
                Button("Mount") {
                    if let selected = selection { onSelect(selected) }
                }
                .keyboardShortcut(.defaultAction)
                .disabled(selection == nil)
            }
            .padding(16)
        }
        .frame(width: 460, height: 360)
        .onAppear {
            if selection == nil { selection = images.first }
        }
    }

    /// 一覧に添えるファイルサイズ表示。取得できない場合は空文字(行は出す)。
    private static func fileSizeText(_ url: URL) -> String {
        guard let attrs = try? FileManager.default.attributesOfItem(atPath: url.path),
              let size = attrs[.size] as? NSNumber else {
            return ""
        }
        return ByteCountFormatter.string(fromByteCount: size.int64Value, countStyle: .file)
    }
}
