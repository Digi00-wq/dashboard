import Foundation

// Wire format of POST /api/collect (see backend/src/collector.cpp).
// Times are Unix milliseconds.

/// Body of POST /api/collect.
struct UploadBody: Encodable {
    var health: [HealthSampleDTO] = []
    var deleted: [String] = []
    var locations: [LocationDTO] = []
    var device: [DeviceStatDTO] = []
}

enum MetaValue: Codable {
    case s(String)
    case n(Double)

    func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        switch self {
        case .s(let v): try c.encode(v)
        case .n(let v): try c.encode(v)
        }
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if let d = try? c.decode(Double.self) { self = .n(d) } else { self = .s(try c.decode(String.self)) }
    }
}

/// One HealthKit sample as uploaded.
struct HealthSampleDTO: Encodable {
    let uuid: String
    let type: String
    let start: Int64
    let end: Int64
    let value: Double?
    let unit: String?
    let source: String?
    let meta: [String: MetaValue]?
}

/// One location point as uploaded.
struct LocationDTO: Codable {
    let t: Int64
    var kind: String = "point" // "point" | "visit"
    let lat: Double
    let lon: Double
    var accuracy: Double?
    var altitude: Double?
    var speed: Double?
    var course: Double?
    var arrive: Int64?
    var depart: Int64?
}

/// Battery / storage / network snapshot as uploaded.
struct DeviceStatDTO: Codable {
    let t: Int64
    let device: String?
    let battery: Double?
    let batteryState: String?
    let lowPower: Bool?
    let storageFree: Int64?
    let storageTotal: Int64?
    let network: String?
}

/// Response of POST /api/devices/login.
struct LoginResponse: Decodable {
    let token: String
    let user: String
    let deviceId: String
}

/// Response of POST /api/collect.
struct UploadResult: Decodable {
    let health: Int
    let deleted: Int
    let locations: Int
    let device: Int
}

/// Response of GET /api/collect/status.
struct ServerStatus: Decodable {
    struct Summary: Decodable {
        let count: Int
        let newest: Int64?
    }
    let health: Summary?
    let locations: Summary?
    let device: Summary?
    let healthTypes: [String: Int]?
    let user: String?
    let deviceName: String?
}

// MARK: - Helpers

/// Date -> Unix milliseconds (the server's time format).
func ms(_ date: Date) -> Int64 { Int64((date.timeIntervalSince1970 * 1000).rounded()) }

/// Removes control characters and cuts to `maxBytes` of UTF-8 without
/// splitting a character (the server rejects anything else).
func clean(_ s: String, maxBytes: Int) -> String {
    var out = ""
    var bytes = 0
    for ch in s {
        if ch.unicodeScalars.contains(where: { $0.value < 0x20 || $0.value == 0x7F }) { continue }
        let n = String(ch).utf8.count
        if bytes + n > maxBytes { break }
        out.append(ch)
        bytes += n
    }
    return out
}
