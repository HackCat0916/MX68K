//
//  StateListView.swift
//  MX68K-iOS
//
//  P725: クイックロードの一覧 UI。`viewModel.listSavedStates()` が返す URL 一覧を
//  新しい順に表示し、タップで即読込・スワイプで削除する。
//

import SwiftUI

struct StateListView: View {
    @EnvironmentObject var viewModel: MX68KiOSViewModel
    @Environment(\.dismiss) private var dismiss

    @State private var states: [URL] = []

    var body: some View {
        NavigationStack {
            List {
                if states.isEmpty {
                    Text("No saved states")
                        .foregroundColor(.secondary)
                }
                ForEach(states, id: \.self) { url in
                    Button {
                        viewModel.loadState(url: url)
                        dismiss()
                    } label: {
                        Text(Self.displayName(for: url))
                            .foregroundColor(.primary)
                    }
                }
                .onDelete(perform: delete)
            }
            .navigationTitle("Load State")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { dismiss() }
                }
            }
        }
        .onAppear { reload() }
    }

    private func reload() {
        states = viewModel.listSavedStates()
    }

    private func delete(at offsets: IndexSet) {
        for index in offsets { viewModel.deleteState(url: states[index]) }
        reload()
    }

    /// ファイル名 "mx68k_yyyyMMdd_HHmmss.mxstate" を読める日時表示へ変換する。
    /// パースに失敗した場合(想定外のファイル名)はファイル名そのものを出す
    /// (無言で空表示にしない)。
    private static func displayName(for url: URL) -> String {
        let base = url.deletingPathExtension().lastPathComponent
        guard base.hasPrefix("mx68k_") else { return url.lastPathComponent }
        let stamp = String(base.dropFirst("mx68k_".count))
        let parseFmt = DateFormatter()
        parseFmt.locale = Locale(identifier: "en_US_POSIX")
        parseFmt.dateFormat = "yyyyMMdd_HHmmss"
        guard let date = parseFmt.date(from: stamp) else { return url.lastPathComponent }
        let displayFmt = DateFormatter()
        displayFmt.dateStyle = .medium
        displayFmt.timeStyle = .medium
        return displayFmt.string(from: date)
    }
}
