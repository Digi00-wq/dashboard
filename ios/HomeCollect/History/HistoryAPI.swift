import CoreLocation
import Foundation

// MARK: - Models (/api/history/*)
//
// Times are Unix milliseconds, keys without data are omitted by the server,
// so every field is optional. Numbers are decoded as Double to be lenient
// about "12" vs "12.0".

struct HistoryDailyResponse: Decodable {
    let days: [HistoryDailyEntry]?
}

struct HistoryDailyEntry: Decodable, Identifiable, Hashable {
    let date: String
    let steps: Double?
    let activeKcal: Double?
    let distanceKm: Double?
    let exerciseMin: Double?
    let flights: Double?
    let restingHr: Double?
    let hrvMs: Double?
    let heartRateMin: Double?
    let heartRateMax: Double?
    let heartRateAvg: Double?
    let sleepMin: Double?
    let deepMin: Double?
    let remMin: Double?
    let coreMin: Double?
    let awakeMin: Double?
    let workouts: Double?

    var id: String { date }
    /// Local midnight of `date`.
    var day: Date? { HistoryDate.parse(date) }

    /// Time asleep: the server total, else the sum of the stages.
    var asleepMin: Double? {
        if let s = sleepMin, s > 0 { return s }
        let sum = (deepMin ?? 0) + (coreMin ?? 0) + (remMin ?? 0)
        return sum > 0 ? sum : nil
    }

    var hasAnyValue: Bool {
        [steps, activeKcal, distanceKm, exerciseMin, restingHr, hrvMs, heartRateAvg, asleepMin, workouts]
            .contains { ($0 ?? 0) > 0 }
    }
}

struct HistoryHeartRateResponse: Decodable {
    let samples: [HistoryHeartRateSample]?
}

struct HistoryHeartRateSample: Decodable, Hashable {
    let t: Double
    let bpm: Double
    var time: Date { Date(timeIntervalSince1970: t / 1000) }
}

struct HistorySleepResponse: Decodable {
    let segments: [HistorySleepSegment]?
    let source: String?
}

struct HistorySleepSegment: Decodable, Hashable {
    let start: Double
    let end: Double
    let stage: String
    var startDate: Date { Date(timeIntervalSince1970: start / 1000) }
    var endDate: Date { Date(timeIntervalSince1970: end / 1000) }
    var minutes: Double { max(0, end - start) / 60_000 }
}

struct HistoryWorkoutsResponse: Decodable {
    let workouts: [HistoryWorkout]?
}

struct HistoryWorkout: Decodable, Identifiable, Hashable {
    let uuid: String?
    let start: Double?
    let end: Double?
    let activity: String?
    let durationS: Double?
    let kcal: Double?
    let distanceKm: Double?
    let avgHeartRate: Double?

    var id: String { uuid ?? "\(start ?? 0)-\(activity ?? "")" }
    var startDate: Date? { start.map { Date(timeIntervalSince1970: $0 / 1000) } }
    var endDate: Date? { end.map { Date(timeIntervalSince1970: $0 / 1000) } }
    var durationMin: Int? {
        if let d = durationS { return Int((d / 60).rounded()) }
        if let s = start, let e = end, e > s { return Int(((e - s) / 60_000).rounded()) }
        return nil
    }
}

struct HistoryLocationsResponse: Decodable {
    let points: [HistoryLocationPoint]?
    let visits: [HistoryLocationVisit]?
}

struct HistoryLocationPoint: Decodable, Hashable {
    let t: Double
    let lat: Double
    let lon: Double
    let accuracy: Double?
    let speed: Double?
    var time: Date { Date(timeIntervalSince1970: t / 1000) }
    var coordinate: CLLocationCoordinate2D { CLLocationCoordinate2D(latitude: lat, longitude: lon) }
}

struct HistoryLocationVisit: Decodable, Hashable {
    let arrive: Double?
    let depart: Double?
    let lat: Double
    let lon: Double
    var arriveDate: Date? { arrive.map { Date(timeIntervalSince1970: $0 / 1000) } }
    var departDate: Date? { depart.map { Date(timeIntervalSince1970: $0 / 1000) } }
    var coordinate: CLLocationCoordinate2D { CLLocationCoordinate2D(latitude: lat, longitude: lon) }
}

// MARK: - Dates

enum HistoryDate {
    private static let formatter: DateFormatter = {
        let f = DateFormatter()
        f.calendar = Calendar(identifier: .gregorian)
        f.locale = Locale(identifier: "en_US_POSIX")
        f.timeZone = TimeZone.current
        f.dateFormat = "yyyy-MM-dd"
        return f
    }()

    static func parse(_ s: String) -> Date? {
        guard let d = formatter.date(from: s) else { return nil }
        return Calendar.current.startOfDay(for: d)
    }

    static func string(_ date: Date) -> String { formatter.string(from: date) }
}

// MARK: - Requests

extension APIClient {
    func historyDaily(days: Int) async throws -> [HistoryDailyEntry] {
        let r: HistoryDailyResponse = try await historyGet("api/history/daily", ["days": String(min(max(days, 1), 365))])
        return r.days ?? []
    }

    func historyHeartRate(date: Date) async throws -> [HistoryHeartRateSample] {
        let r: HistoryHeartRateResponse = try await historyGet("api/history/heartrate", ["date": HistoryDate.string(date)])
        return (r.samples ?? []).sorted { $0.t < $1.t }
    }

    func historySleep(date: Date) async throws -> HistorySleepResponse {
        try await historyGet("api/history/sleep", ["date": HistoryDate.string(date)])
    }

    func historyWorkouts(days: Int) async throws -> [HistoryWorkout] {
        let r: HistoryWorkoutsResponse = try await historyGet("api/history/workouts", ["days": String(min(max(days, 1), 365))])
        return r.workouts ?? []
    }

    func historyLocations(date: Date) async throws -> HistoryLocationsResponse {
        try await historyGet("api/history/locations", ["date": HistoryDate.string(date)])
    }

    /// GET with query parameters (`send` would percent-encode the "?").
    private func historyGet<T: Decodable>(_ path: String, _ query: [String: String]) async throws -> T {
        guard var comps = URLComponents(url: base.appendingPathComponent(path), resolvingAgainstBaseURL: false) else {
            throw APIError.invalidURL
        }
        comps.queryItems = query.map { URLQueryItem(name: $0.key, value: $0.value) }
        guard let url = comps.url else { throw APIError.invalidURL }
        var req = URLRequest(url: url)
        req.timeoutInterval = 60
        req.setValue("application/json", forHTTPHeaderField: "Accept")
        if let token { req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization") }

        let data: Data, resp: URLResponse
        do {
            (data, resp) = try await URLSession.shared.data(for: req)
        } catch let e as URLError where e.code == .cancelled {
            throw CancellationError()
        } catch {
            throw APIError.network(error.localizedDescription)
        }
        guard let http = resp as? HTTPURLResponse else { throw APIError.network("No response") }
        switch http.statusCode {
        case 200..<300:
            return try JSONDecoder().decode(T.self, from: data)
        case 401: throw APIError.unauthorized
        case 403: throw APIError.forbidden
        default:
            let json = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
            let message = json?["error"] as? String ?? HTTPURLResponse.localizedString(forStatusCode: http.statusCode)
            throw APIError.server(http.statusCode, message)
        }
    }
}
