import SwiftUI

/// 選択可能な UI 表示言語。`system` は macOS のシステム言語に従い、
/// `ja` / `en` はそれぞれアプリの UI を日本語 / 英語に固定する。
enum AppLanguage: String, CaseIterable, Identifiable {
    case system, ja, en

    var id: String { rawValue }

    /// 言語 Picker に表示する名前。
    /// `System` はローカライズする(en="System" / ja="システム")。各言語名は
    /// その言語自身の表記で表示し、意図的に翻訳しない。
    var displayName: String {
        switch self {
        case .system: return String(localized: "System")
        case .ja:     return "日本語"
        case .en:     return "English"
        }
    }
}

/// 選択された UI 言語を、本アプリのドメインに限って `AppleLanguages` の
/// UserDefaults キーを上書きすることで適用する(他のアプリには影響しない)。
/// 変更は次回起動時に反映される(再起動方式 — P188 の計画を参照)。
enum LocalizationManager {
    static let key = "appLanguage"

    static func apply(_ lang: AppLanguage) {
        switch lang {
        case .system:
            UserDefaults.standard.removeObject(forKey: "AppleLanguages")
        case .ja:
            UserDefaults.standard.set(["ja"], forKey: "AppleLanguages")
        case .en:
            UserDefaults.standard.set(["en"], forKey: "AppleLanguages")
        }
    }
}
