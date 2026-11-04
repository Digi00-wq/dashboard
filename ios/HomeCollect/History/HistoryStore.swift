import SwiftUI

enum HistoryRange: Int, CaseIterable, Identifiable, Hashable {
    case week = 7
    case month = 30
    case year = 365

    var id: Int { rawValue }
    var days: Int { rawValue }

    var title: LocalizedStringKey {
        switch self {
        case .week: return "Week"
        case .month: return "Month"
        case .year: return "Year"
        }
    }

    /// The year is shown as weekly averages so the bars stay readable.
    var weekly: Bool { self == .year }
    var unit: Calendar.Component { weekly ? .weekOfYear : .day }

    /// X domain of the overview charts: the whole range up to the end of today
    /// (or the end of this week).
    var domain: ClosedRange<Date> {
        let cal = Calendar.current
        let today = cal.startOfDay(for: Date())
        let first = cal.date(byAdding: .day, value: -(days - 1), to: today) ?? today
        if weekly {
            let start = cal.dateInterval(of: .weekOfYear, for: first)?.start ?? first
            let end = cal.dateInterval(of: .weekOfYear, for: today)?.end ?? today
            return start...end
        }
        let end = cal.date(byAdding: .day, value: 1, to: today) ?? today
        return first...end
    }

    var axisStride: Calendar.Component {
        switch self {
        case .week: return .day
        case .month: return .day
        case .year: return .month
        }
    }

    var axisCount: Int { self == .month ? 7 : 1 }

    var axisFormat: Date.FormatStyle {
        switch self {
        case .week: return .dateTime.weekday(.narrow)
        case .month: return .dateTime.day().month(.abbreviated)
        case .year: return .dateTime.month(.narrow)
        }
    }

    var axisCentered: Bool { self == .week }
}

enum HistoryMetric: String, Hashable, CaseIterable {
    case steps, sleep, heart, energy
}

enum HistoryRoute: Hashable {
    case metric(HistoryMetric)
    case day(Date)
    case workouts
}

enum HistorySleepStage: String, CaseIterable, Hashable {
    case awake, rem, core, deep, asleep, inBed

    var label: String {
        switch self {
        case .awake: return String(localized: "Awake")
        case .rem: return String(localized: "REM")
        case .core: return String(localized: "Core")
        case .deep: return String(localized: "Deep")
        case .asleep: return String(localized: "Asleep")
        case .inBed: return String(localized: "In bed")
        }
    }

    var color: Color {
        switch self {
        case .awake: return .orange
        case .rem: return .cyan
        case .core: return .blue
        case .deep: return .indigo
        case .asleep: return .teal
        case .inBed: return .gray
        }
    }

    var isAsleep: Bool { self == .rem || self == .core || self == .deep || self == .asleep }

    /// Stages of the nightly overview bars, bottom to top.
    static let stacked: [HistorySleepStage] = [.deep, .core, .rem, .awake]
}

// MARK: - Chart data

struct HistoryChartPoint: Identifiable, Hashable {
    let date: Date
    let value: Double
    let isCurrent: Bool
    var id: Date { date }
}

struct HistoryStagePoint: Identifiable, Hashable {
    let date: Date
    let stage: HistorySleepStage
    let minutes: Double
    let isCurrent: Bool
    var id: String { "\(date.timeIntervalSince1970)-\(stage.rawValue)" }
}

enum HistoryAggregate {
    /// One point per day, or per week (average of the days with data).
    static func series(_ days: [HistoryDailyEntry], weekly: Bool, _ value: (HistoryDailyEntry) -> Double?) -> [HistoryChartPoint] {
        let cal = Calendar.current
        if !weekly {
            return days.compactMap { d in
                guard let date = d.day, let v = value(d), v > 0 else { return nil }
                return HistoryChartPoint(date: date, value: v, isCurrent: cal.isDateInToday(date))
            }
        }
        var buckets: [Date: [Double]] = [:]
        for d in days {
            guard let date = d.day, let v = value(d), v > 0,
                  let week = cal.dateInterval(of: .weekOfYear, for: date)?.start else { continue }
            buckets[week, default: []].append(v)
        }
        let current = cal.dateInterval(of: .weekOfYear, for: Date())?.start
        return buckets.keys.sorted().compactMap { week in
            guard let values = buckets[week], !values.isEmpty else { return nil }
            return HistoryChartPoint(date: week, value: values.reduce(0, +) / Double(values.count), isCurrent: week == current)
        }
    }

    /// Sleep stages per night (or weekly average per stage), in hours.
    static func sleepStages(_ days: [HistoryDailyEntry], weekly: Bool) -> [HistoryStagePoint] {
        let cal = Calendar.current
        func minutes(_ d: HistoryDailyEntry, _ s: HistorySleepStage) -> Double {
            switch s {
            case .deep: return d.deepMin ?? 0
            case .core:
                // Nights without stage data: show the total as "core".
                let staged = (d.deepMin ?? 0) + (d.coreMin ?? 0) + (d.remMin ?? 0)
                return staged > 0 ? (d.coreMin ?? 0) : (d.sleepMin ?? 0)
            case .rem: return d.remMin ?? 0
            case .awake: return d.awakeMin ?? 0
            default: return 0
            }
        }
        let nights = days.filter { ($0.asleepMin ?? 0) > 0 && $0.day != nil }
        if !weekly {
            return nights.flatMap { d -> [HistoryStagePoint] in
                let date = d.day!
                return HistorySleepStage.stacked.compactMap { s in
                    let m = minutes(d, s)
                    return m > 0 ? HistoryStagePoint(date: date, stage: s, minutes: m, isCurrent: cal.isDateInToday(date)) : nil
                }
            }
        }
        var buckets: [Date: [HistoryDailyEntry]] = [:]
        for d in nights {
            guard let week = cal.dateInterval(of: .weekOfYear, for: d.day!)?.start else { continue }
            buckets[week, default: []].append(d)
        }
        let current = cal.dateInterval(of: .weekOfYear, for: Date())?.start
        return buckets.keys.sorted().flatMap { week -> [HistoryStagePoint] in
            let group = buckets[week] ?? []
            return HistorySleepStage.stacked.compactMap { s in
                let avg = group.reduce(0) { $0 + minutes($1, s) } / Double(max(group.count, 1))
                return avg > 0 ? HistoryStagePoint(date: week, stage: s, minutes: avg, isCurrent: week == current) : nil
            }
        }
    }

    static func average(_ days: [HistoryDailyEntry], _ value: (HistoryDailyEntry) -> Double?) -> Double? {
        let values = days.compactMap(value).filter { $0 > 0 }
        guard !values.isEmpty else { return nil }
        return values.reduce(0, +) / Double(values.count)
    }

    static func total(_ days: [HistoryDailyEntry], _ value: (HistoryDailyEntry) -> Double?) -> Double {
        days.compactMap(value).reduce(0, +)
    }
}

// MARK: - Store

@MainActor
final class HistoryStore: ObservableObject {
    @Published var range: HistoryRange = .week
    @Published private(set) var days: [HistoryDailyEntry] = []
    @Published private(set) var workouts: [HistoryWorkout] = []
    @Published private(set) var loading = false
    @Published private(set) var error: String?
    @Published private(set) var forbidden = false
    /// True once data for the current range was shown (from cache or network).
    @Published private(set) var loaded = false

    private struct Cached {
        let days: [HistoryDailyEntry]
        let workouts: [HistoryWorkout]
        let at: Date
    }
    private var cache: [HistoryRange: Cached] = [:]
    private var generation = 0

    var hasData: Bool { days.contains { $0.hasAnyValue } || !workouts.isEmpty }

    /// Shows the cached range right away and refreshes when stale or forced.
    func load(force: Bool = false) async {
        let r = range
        generation += 1
        let gen = generation
        if let c = cache[r] {
            days = c.days
            workouts = c.workouts
            loaded = true
            if !force && Date().timeIntervalSince(c.at) < 300 { loading = false; return }
        } else {
            days = []
            workouts = []
            loaded = false
        }
        guard let api = SyncEngine.shared.apiClient else {
            error = APIError.unauthorized.errorDescription
            return
        }
        loading = true
        defer { if gen == generation { loading = false } }
        do {
            async let d = api.historyDaily(days: r.days)
            async let w = api.historyWorkouts(days: r.days)
            let (newDays, newWorkouts) = try await (d, w)
            let sorted = newDays.sorted { $0.date < $1.date }
            cache[r] = Cached(days: sorted, workouts: newWorkouts, at: Date())
            forbidden = false
            guard gen == generation else { return }
            days = sorted
            workouts = newWorkouts
            error = nil
            loaded = true
        } catch APIError.forbidden {
            forbidden = true
        } catch is CancellationError {
            // view went away / range switched
        } catch {
            if (error as? URLError)?.code == .cancelled { return }
            guard gen == generation else { return }
            self.error = error.localizedDescription
        }
    }

    /// Daily values of `date`, if any range loaded so far contains it.
    func entry(for date: Date) -> HistoryDailyEntry? {
        let key = HistoryDate.string(date)
        for r in HistoryRange.allCases {
            if let e = cache[r]?.days.first(where: { $0.date == key }) { return e }
        }
        return nil
    }
}
