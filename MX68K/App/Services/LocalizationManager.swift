import SwiftUI

/// Selectable UI display language. `system` follows the macOS system language;
/// `ja` / `en` force the app UI to Japanese / English respectively.
enum AppLanguage: String, CaseIterable, Identifiable {
    case system, ja, en

    var id: String { rawValue }

    /// Human-readable name shown in the language Picker.
    /// `System` is localized (en="System" / ja="システム"); the language names
    /// are shown in their own script and intentionally not translated.
    var displayName: String {
        switch self {
        case .system: return String(localized: "System")
        case .ja:     return "日本語"
        case .en:     return "English"
        }
    }
}

/// Applies the chosen UI language by overriding the `AppleLanguages`
/// UserDefaults key for this app's domain only (does not affect other apps).
/// The change takes effect at the next launch (restart method — see P188 plan).
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
