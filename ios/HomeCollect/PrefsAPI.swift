import Foundation

// Server features beyond data collection: per-user preferences, the morning
// briefing, the learned sleep pattern and presence (home zone) events.
// JSON is camelCase, times are Unix milliseconds. Keys the server leaves out
// mean "no data", so everything is optional.

/// GET/PUT /api/prefs. A PUT sends only the keys that are set (nil keys are
/// left out by the synthesized encoder) and returns the full prefs.
struct ServerPrefs: Codable, Equatable {
    var language: String?            // "de" | "en" (push texts)
    var notifyLightsLeftOn: Bool?
    var lightsIdleMin: Int?          // 5...240
    var notifyServer: Bool?
    var morningBriefing: Bool?
    var notifyViaHa: Bool?           // also send through the Home Assistant app
    var arriveLights: Bool?
    var leaveLightsOff: Bool?
    var leavePauseMedia: Bool?
    var adaptiveWakeLight: Bool?
    var bedtimeReminder: Bool?
}

/// GET /api/briefing (204 = none yet).
struct Briefing: Decodable {
    struct SleepMinutes: Decodable {
        var total: Double?
        var deep: Double?
        var rem: Double?
        var core: Double?
        var awake: Double?

        /// The dashboard model, so the dashboard's SleepView can draw it.
        var dashboardSleep: Sleep {
            func i(_ v: Double?) -> Int? { v.map { Int($0.rounded()) } }
            return Sleep(total: i(total), deep: i(deep), rem: i(rem), core: i(core), awake: i(awake))
        }
    }

    struct WeatherInfo: Decodable {
        var temperature: Double?
        var condition: String?
        var high: Double?
        var low: Double?
        var precipitationProbability: Double?
    }

    struct TaskItem: Decodable {
        var summary: String?
        var due: String?
        var list: String?

        private enum CodingKeys: String, CodingKey { case summary, due, list }

        init(from decoder: Decoder) throws {
            let c = try decoder.container(keyedBy: CodingKeys.self)
            summary = try? c.decodeIfPresent(String.self, forKey: .summary)
            list = try? c.decodeIfPresent(String.self, forKey: .list)
            // "due" may be a date / time string or Unix ms.
            if let s = try? c.decodeIfPresent(String.self, forKey: .due) {
                due = s
            } else if let n = try? c.decodeIfPresent(Double.self, forKey: .due) {
                let date = Date(timeIntervalSince1970: n / 1000)
                due = date.formatted(date: .omitted, time: .shortened)
            } else {
                due = nil
            }
        }
    }

    struct ServerHealth: Decodable {
        var ok: Bool?
        var issues: [String]?
    }

    struct ActivityInfo: Decodable {
        var steps: Double?
        var restingHr: Double?
        var hrvMs: Double?
    }

    var generatedAt: Flex64?
    var greeting: String?
    var summary: String?
    var sleep: SleepMinutes?
    var weather: WeatherInfo?
    var alarm: Flex64?
    var tasks: [TaskItem]?
    var server: ServerHealth?
    var activity: ActivityInfo?
    var tips: [String]?
}

/// GET /api/sleep-pattern (204 = not enough data).
struct SleepPattern: Decodable {
    struct Window: Decodable {
        var bedtime: String?  // "23:10"
        var wake: String?     // "07:05"
        var nights: Int?
    }
    var weekday: Window?
    var weekend: Window?
    var updatedAt: Flex64?
}

/// GET /api/home: the home zone from Home Assistant (204 = not configured).
struct HomeZone: Codable, Equatable {
    var lat: Double?
    var lon: Double?
    var radius: Double?  // meters
}

extension APIClient {
    /// GET /api/prefs.
    func prefs() async throws -> ServerPrefs {
        try JSONDecoder().decode(ServerPrefs.self, from: try await send("api/prefs"))
    }

    /// Sends only the keys set in `change`; returns the full prefs.
    func updatePrefs(_ change: ServerPrefs) async throws -> ServerPrefs {
        let data = try await send("api/prefs", method: "PUT", body: try JSONEncoder().encode(change))
        return try JSONDecoder().decode(ServerPrefs.self, from: data)
    }

    /// nil when the server has none yet (204).
    func briefing() async throws -> Briefing? {
        try decodeOptional(Briefing.self, from: try await send("api/briefing"))
    }

    /// nil when there isn't enough sleep data yet (204).
    func sleepPattern() async throws -> SleepPattern? {
        try decodeOptional(SleepPattern.self, from: try await send("api/sleep-pattern"))
    }

    /// nil when no home zone is configured (204). Needs "collect".
    func homeZone() async throws -> HomeZone? {
        try decodeOptional(HomeZone.self, from: try await send("api/home"))
    }

    /// event: "arrive" | "leave". Needs "collect".
    func sendPresence(event: String, t: Int64) async throws {
        let body = try JSONSerialization.data(withJSONObject: ["event": event, "t": t] as [String: Any])
        _ = try await send("api/presence", method: "POST", body: body)
    }

    /// An empty body (204 No Content) means "no data".
    private func decodeOptional<T: Decodable>(_ type: T.Type, from data: Data) throws -> T? {
        if data.isEmpty { return nil }
        return try JSONDecoder().decode(T.self, from: data)
    }
}
