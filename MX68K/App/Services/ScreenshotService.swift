import AppKit
import CoreGraphics

enum ScreenshotService {
    /// エミュ framebuffer(BGRA)を PNG として ~/Pictures/MX68K/ に保存。成功時に保存先 URL を返す。
    @discardableResult
    static func capture() -> URL? {
        var w: Int32 = 0, h: Int32 = 0
        guard let ptr = mx68k_get_framebuffer(&w, &h), w > 0, h > 0 else { return nil }
        let width = Int(w), height = Int(h)
        let bytesPerRow = width * 4
        // ptr は次フレームで上書きされるため即コピー。
        let data = Data(bytes: ptr, count: bytesPerRow * height)
        guard let provider = CGDataProvider(data: data as CFData) else { return nil }
        // BGRA(.bgra8Unorm)= byteOrder32Little + premultipliedFirst。
        let bitmapInfo = CGBitmapInfo(rawValue:
            CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        guard let cg = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
                               bytesPerRow: bytesPerRow, space: CGColorSpaceCreateDeviceRGB(),
                               bitmapInfo: bitmapInfo, provider: provider, decode: nil,
                               shouldInterpolate: false, intent: .defaultIntent) else { return nil }
        let rep = NSBitmapImageRep(cgImage: cg)
        guard let png = rep.representation(using: .png, properties: [:]) else { return nil }
        // 保存先: 設定値(UserDefaults "screenshotDirectory")が空/無効ならデフォルト ~/Pictures/MX68K。
        let dir = screenshotDirectory()
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let fmt = DateFormatter()
        fmt.locale = Locale(identifier: "en_US_POSIX")            // ★数値固定 format 保証
        fmt.dateFormat = "yyyyMMdd_HHmmss_SSS"                     // ★ミリ秒付与で同秒2連射の同名衝突回避
        let url = dir.appendingPathComponent("mx68k_\(fmt.string(from: Date())).png")
        do { try png.write(to: url); return url } catch { return nil }
    }

    /// 保存先ディレクトリ。設定値(UserDefaults "screenshotDirectory")が空/無ければ ~/Pictures/MX68K。
    /// (enum ゆえ @AppStorage は使えず UserDefaults.standard を直読 — @AppStorage と同じ UserDefaults ドメイン。)
    static func screenshotDirectory() -> URL {
        if let p = UserDefaults.standard.string(forKey: "screenshotDirectory"), !p.isEmpty {
            return URL(fileURLWithPath: p, isDirectory: true)
        }
        return FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Pictures/MX68K", isDirectory: true)
    }
}
