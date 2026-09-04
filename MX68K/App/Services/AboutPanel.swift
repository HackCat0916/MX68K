import AppKit

/// P701 — 「MX68Kについて」(About)パネルのクレジット欄。
///
/// macOS 標準の About パネル(アプリ名・バージョン・ビルド番号)はそのまま使い、
/// `NSApplication.AboutPanelOptionKey.credits` にだけ独自の文面を渡して
/// クレジット欄を追加する。状態を持たないため `ScreenshotService` と同型の enum。
///
/// 文面は公開リポジトリの `LICENSE`(非商用フリーウェア)と
/// `NOTICE-THIRD-PARTY.md`(同ファイルが実際にカバーする範囲 = XM6 のみ)へ
/// リンクで案内する。参照先文書に存在しない移植元を案内しないこと。
///
/// P701改訂(2026-08-27): タグライン・px68k クレジット行は日本語UIでも**英語固定**
/// (`String(localized:)` を通さない)、全行**中央寄せ**。NOTICE / LICENSE / HomePage は
/// 「NOTICE | LICENSE | HomePage」の1行にまとめ、各語を個別のリンクにする。
enum AboutPanel {
    /// GitHub 公開リポジトリ。言語に依存しないので `String(localized:)` を通さない。
    private static let repositoryURLString = "https://github.com/HackCat0916/MX68K"
    /// サードパーティ表記(XM6)。公開リポジトリ上の実ファイルを指す。
    private static let noticeURLString =
        "https://github.com/HackCat0916/MX68K/blob/main/NOTICE-THIRD-PARTY.md"
    /// 非商用フリーウェアライセンス本文。公開リポジトリ上の実ファイルを指す。
    private static let licenseURLString =
        "https://github.com/HackCat0916/MX68K/blob/main/LICENSE"

    /// リンク行の並び(表示語, リンク先URL文字列)。
    private static let linkEntries: [(label: String, urlString: String)] = [
        ("NOTICE", noticeURLString),
        ("LICENSE", licenseURLString),
        ("HomePage", repositoryURLString),
    ]

    /// クレジット欄付きの標準 About パネルを表示する。
    static func show() {
        let font = NSFont.systemFont(ofSize: NSFont.smallSystemFontSize)

        // 中央寄せ段落スタイル。生成後は一切変更しないため copy() で不変化しておく。
        let centeredStyle: NSParagraphStyle = {
            let style = NSMutableParagraphStyle()
            style.alignment = .center
            return (style.copy() as? NSParagraphStyle) ?? style
        }()

        // ★共通(非リンク)属性。この辞書は `.link` キーを決して含まない。
        // ★foregroundColor は敢えて指定しない: 未指定なら NSTextView 既定の
        //   textColor(ダイナミックカラー)で描かれ、ダーク/ライト双方で読める。
        let plainAttributes: [NSAttributedString.Key: Any] = [
            .font: font,
            .paragraphStyle: centeredStyle,
        ]

        let credits = NSMutableAttributedString()

        // 1行目: タグライン(英語固定・中央寄せ)。
        credits.append(NSAttributedString(
            string: "SHARP X68000 emulator for macOS.\n\n",
            attributes: plainAttributes))

        // 2行目: エミュレーションコアのクレジット(英語固定・中央寄せ)。
        credits.append(NSAttributedString(
            string: "Emulation core based on px68k (by hissorii).\n\n",
            attributes: plainAttributes))

        // 3行目: 「NOTICE | LICENSE | HomePage」。各語のみをリンクにする。
        for (index, entry) in linkEntries.enumerated() {
            if index > 0 {
                // ★区切り文字は `.link` を含まない `plainAttributes` で構築する
                //   (属性辞書を使い回して `.link` を外し忘れると区切り自体が
                //   リンク化する典型的な事故を構造的に避けるため)。
                credits.append(NSAttributedString(string: " | ", attributes: plainAttributes))
            }

            // リンクセグメントは plainAttributes の**値コピー**に `.link` を足した
            // 独立した辞書で構築する(Swift の Dictionary は値型なので
            // plainAttributes 側は書き換わらない)。
            var linkAttributes = plainAttributes
            if let url = URL(string: entry.urlString) {
                linkAttributes[.link] = url
            }
            credits.append(NSAttributedString(string: entry.label, attributes: linkAttributes))
        }

        NSApp.orderFrontStandardAboutPanel(options: [.credits: credits])
    }
}
