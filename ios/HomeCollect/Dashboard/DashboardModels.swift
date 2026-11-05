import Foundation

// JSON form of the `Dashboard` protobuf (proto/dashboard.proto), as sent by
// GET /api/events?format=json. Proto3 JSON leaves out default values
// (false, 0, "", empty lists), so everything is optional here, and 64-bit
// numbers arrive as strings (see Flex64).

/// uint64 from proto3 JSON: a string like "1793823659214" (or a number).
struct Flex64: Codable, Hashable {
    let value: Int64

    init(_ value: Int64) { self.value = value }

    init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if let i = try? c.decode(Int64.self) {
            value = i
        } else if let s = try? c.decode(String.self), let i = Int64(s) {
            value = i
        } else if let d = try? c.decode(Double.self) {
            value = Int64(d)
        } else {
            throw DecodingError.dataCorruptedError(in: c, debugDescription: "not an int64")
        }
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        try c.encode(String(value))
    }

    /// Interprets the value as Unix milliseconds.
    var date: Date { Date(timeIntervalSince1970: Double(value) / 1000) }
}

struct Dashboard: Decodable {
    var presence: Presence?
    var light: RoomLight?
    var weather: Weather?
    var phone: Phone?
    var todoist: Todoist?
    var lamps: [Lamp]?
    var server: ServerInfo?
    var updatedAt: Flex64?
    var errors: [String]?
    var glance: Glance?
    var sleep: Sleep?
    var voiceAvailable: Bool?
    var activity: Activity?
}

/// From the HomeCollect app (server-side summary of the uploads).
struct Activity: Decodable {
    var user: String?
    var syncedAt: Flex64?
    var steps: Int?
    var activeKcal: Double?
    var distanceKm: Double?
    var exerciseMin: Int?
    var flights: Int?
    var heartRate: Int?
    var restingHr: Int?
    var hrvMs: Double?
    var vo2max: Double?
    var sleep: Sleep?
    var lastWorkout: Workout?
    var battery: Double?
    var batteryState: String?
    var location: GeoPoint?
}

struct Workout: Decodable {
    var activity: String?
    var durationS: Int?
    var kcal: Double?
    var distanceKm: Double?
    var end: Flex64?
}

struct GeoPoint: Decodable {
    var lat: Double?
    var lon: Double?
    var accuracy: Double?
    var time: Flex64?
    var home: Bool?
}

struct Glance: Decodable {
    var nextAlarm: Flex64?
    var network: NetworkInfo?
    var media: [MediaPlayer]?
}

struct Sleep: Decodable {
    var total: Int?
    var deep: Int?
    var rem: Int?
    var core: Int?
    var awake: Int?
}

struct NetworkInfo: Decodable {
    var online: Bool?
    var downloadKib: Double?
    var uploadKib: Double?
}

struct MediaPlayer: Decodable, Identifiable {
    var entityId: String
    var name: String?
    var state: String?
    var title: String?
    var artist: String?
    var album: String?
    var source: String?
    var sources: [String]?
    var volume: Double?
    var duration: Int?
    var position: Double?
    var positionUpdatedAt: Flex64?
    var cover: String?
    var shuffle: Bool?

    var id: String { entityId }
    var isPlaying: Bool { state == "playing" }
    var isActive: Bool { state == "playing" || state == "paused" || !(title ?? "").isEmpty }

    /// Current position in seconds, extrapolated while playing.
    func position(at now: Date) -> Double? {
        guard let position else { return nil }
        guard isPlaying, let updated = positionUpdatedAt else { return position }
        let p = position + now.timeIntervalSince(updated.date)
        return min(p, Double(duration ?? Int.max))
    }
}

struct Presence: Decodable {
    var isNow: Bool?
    var lastSeen: Flex64?
}

struct RoomLight: Decodable {
    var luxValue: Int?
    var isLight: Bool?
}

struct Weather: Decodable {
    var temperature: Double?
    var condition: String?
    var humidity: Double?
    var windSpeed: Double?
    var temperatureUnit: String?
    var windSpeedUnit: String?
    var forecast: [Forecast]?
}

struct Forecast: Decodable, Identifiable {
    var time: Flex64?
    var condition: String?
    var temperature: Double?
    var tempLow: Double?
    var precipitation: Double?
    var precipitationProbability: Int?

    var id: Int64 { time?.value ?? 0 }
}

struct Phone: Decodable {
    var steps: Int?
    var heartRate: Int?
    var battery: Int?
}

struct Todoist: Decodable {
    var lists: [TodoList]?
}

struct TodoList: Decodable, Identifiable {
    var entityId: String
    var name: String?
    var task: [TodoTask]?

    var id: String { entityId }
    var tasks: [TodoTask] { task ?? [] }
}

struct TodoTask: Decodable, Identifiable, Hashable {
    var uid: String
    var summary: String?
    var due: String?
    var description: String?

    var id: String { uid }
}

struct Lamp: Decodable, Identifiable {
    var entityId: String
    var name: String?
    var isOn: Bool?
    var available: Bool?
    var brightness: Int?   // 0...255
    var rgb: Int?          // 0xRRGGBB
    var supportsColor: Bool?
    var supportsBrightness: Bool?

    var id: String { entityId }
    var on: Bool { isOn ?? false }
}

struct ServerInfo: Decodable {
    var hostname: String?
    var uptimeSeconds: Flex64?
    var load1: Double?
    var cpuPercent: Double?
    var memUsed: Flex64?
    var memTotal: Flex64?
    var diskUsed: Flex64?
    var diskTotal: Flex64?
    var temperature: Double?
}

// MARK: - Commands (POST /api/command, JSON form of `Command`)

struct DashCommand: Encodable {
    var light: LightCommand?
    var completeTask: CompleteTaskCommand?
    var refresh: Bool?
    var allLights: Bool?
    var media: MediaCommand?
    var announce: AnnounceCommand?
}

struct LightCommand: Encodable {
    let entityId: String
    var on: Bool?
    var brightness: Int?
    var rgb: Int?
}

struct CompleteTaskCommand: Encodable {
    let uid: String
    let entityId: String
}

struct MediaCommand: Encodable {
    enum Action: String, Encodable {
        case playPause = "PLAY_PAUSE", next = "NEXT", previous = "PREVIOUS"
        case setVolume = "SET_VOLUME", selectSource = "SELECT_SOURCE", setShuffle = "SET_SHUFFLE"
    }
    let entityId: String
    let action: Action
    var volume: Double?
    var source: String?
    var shuffle: Bool?
}

struct AnnounceCommand: Encodable {
    let text: String
}

struct MeResponse: Decodable {
    let name: String
    let displayName: String?
    let admin: Bool
    let permissions: [String]
}
