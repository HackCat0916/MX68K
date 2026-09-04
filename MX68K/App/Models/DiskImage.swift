import Foundation

struct DiskImage: Identifiable, Hashable {
    let id = UUID()
    var path: String
    var name: String
    var type: DiskType

    enum DiskType: String, Codable {
        case xdf
        case dim
        case d88
        case hdm
        case img
        case unknown
    }

    init(path: String) {
        self.path = path
        self.name = (path as NSString).lastPathComponent
        let ext = (path as NSString).pathExtension.lowercased()
        self.type = DiskType(rawValue: ext) ?? .unknown
    }
}
