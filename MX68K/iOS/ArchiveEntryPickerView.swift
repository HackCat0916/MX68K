//
//  ArchiveEntryPickerView.swift
//  MX68K-iOS
//
//  P729: zip 内にディスクイメージが複数あったときの選択 UI。
//
//  macOS `ArchiveEntryPickerSheet.swift` の iOS 版。ただし提示方式は P725
//  `StateListView.swift` の「タップで即確定」パターンを踏襲する —— macOS 版の
//  「選択 → Mount ボタンで確定」という 2 段階操作は AppKit デスクトップ UI の
//  慣習であり、iOS では `StateListView`(P725)がすでに同種の一覧 UI で
//  タップ即確定を採用済み。1 つのプロジェクト内で UI パターンを 2 つ持たない。
//

import SwiftUI

struct ArchiveEntryPickerView: View {
    let images: [URL]
    let onSelect: (URL) -> Void
    let onCancel: () -> Void
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List(images, id: \.self) { url in
                Button {
                    onSelect(url)
                    dismiss()
                } label: {
                    HStack {
                        Text(url.lastPathComponent)
                            .foregroundColor(.primary)
                            .lineLimit(1)
                            .truncationMode(.middle)
                        Spacer()
                        Text(Self.fileSizeText(url))
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                }
            }
            .navigationTitle("Select a disk image")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") {
                        onCancel()
                        dismiss()
                    }
                }
            }
        }
    }

    private static func fileSizeText(_ url: URL) -> String {
        guard let attrs = try? FileManager.default.attributesOfItem(atPath: url.path),
              let size = attrs[.size] as? NSNumber else { return "" }
        return ByteCountFormatter.string(fromByteCount: size.int64Value, countStyle: .file)
    }
}
