import Charts
import SwiftUI

/// Bigger chart + list of the daily values of one metric.
struct HistoryMetricDetailView: View {
    let metric: HistoryMetric
    @EnvironmentObject private var store: HistoryStore

    private var days: [HistoryDailyEntry] { store.days }
    private var range: HistoryRange { store.range }

    private var rows: [HistoryDailyEntry] {
        days.filter { d in
            switch metric {
            case .steps: return (d.steps ?? 0) > 0
            case .sleep: return (d.asleepMin ?? 0) > 0
            case .heart: return (d.restingHr ?? 0) > 0 || (d.hrvMs ?? 0) > 0 || (d.heartRateAvg ?? 0) > 0
            case .energy: return (d.activeKcal ?? 0) > 0
            }
        }
        .reversed()
    }

    var body: some View {
        List {
            Section {
                chart
                    .padding(.vertical, 8)
                stats
            } footer: {
                if range.weekly { Text("Bars show weekly averages.") }
            }

            Section("Values") {
                if rows.isEmpty {
                    Text("No data in this period").foregroundStyle(.secondary)
                }
                ForEach(rows) { d in
                    if let date = d.day {
                        NavigationLink(value: HistoryRoute.day(date)) {
                            HStack {
                                Text(date.formatted(.dateTime.weekday(.abbreviated).day().month(.abbreviated)))
                                Spacer()
                                valueText(d)
                                    .monospacedDigit()
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }
                }
            }
        }
        .listStyle(.insetGrouped)
        .navigationTitle(title)
        .navigationBarTitleDisplayMode(.inline)
        .refreshable { await store.load(force: true) }
    }

    private var title: LocalizedStringKey {
        switch metric {
        case .steps: return "Steps"
        case .sleep: return "Sleep"
        case .heart: return "Heart"
        case .energy: return "Active energy"
        }
    }

    @ViewBuilder private var chart: some View {
        switch metric {
        case .steps:
            HistoryBarChart(points: HistoryAggregate.series(days, weekly: range.weekly) { $0.steps },
                           range: range, label: String(localized: "Steps"), color: .orange,
                           average: HistoryAggregate.average(days) { $0.steps }, height: 260)
        case .sleep:
            HistorySleepChart(points: HistoryAggregate.sleepStages(days, weekly: range.weekly), range: range,
                             averageHours: HistoryAggregate.average(days) { $0.asleepMin }.map { $0 / 60 },
                             height: 260)
        case .heart:
            VStack(alignment: .leading, spacing: 12) {
                Text("Resting heart rate").font(.subheadline.weight(.semibold)).foregroundStyle(.pink)
                HistoryLineChart(points: HistoryAggregate.series(days, weekly: range.weekly) { $0.restingHr },
                                range: range, label: String(localized: "Resting HR"), color: .pink, height: 170)
                Text("Heart rate variability").font(.subheadline.weight(.semibold)).foregroundStyle(.purple)
                HistoryLineChart(points: HistoryAggregate.series(days, weekly: range.weekly) { $0.hrvMs },
                                range: range, label: String(localized: "HRV"), color: .purple, height: 170)
            }
        case .energy:
            HistoryBarChart(points: HistoryAggregate.series(days, weekly: range.weekly) { $0.activeKcal },
                           range: range, label: String(localized: "Active energy"), color: .red,
                           average: HistoryAggregate.average(days) { $0.activeKcal }, height: 260)
        }
    }

    @ViewBuilder private var stats: some View {
        switch metric {
        case .steps:
            let avg = HistoryAggregate.average(days) { $0.steps } ?? 0
            let total = HistoryAggregate.total(days) { $0.steps }
            let best = days.compactMap { $0.steps }.max() ?? 0
            let km = HistoryAggregate.total(days) { $0.distanceKm }
            HistoryStatRow(label: "Daily average", value: Text("\(Fmt.number(Int(avg.rounded()))) steps"))
            HistoryStatRow(label: "Total", value: Text("\(Fmt.number(Int(total.rounded()))) steps"))
            HistoryStatRow(label: "Best day", value: Text("\(Fmt.number(Int(best.rounded()))) steps"))
            if km > 0 { HistoryStatRow(label: "Distance", value: Text(String(format: "%.1f km", km))) }
        case .sleep:
            let avg = HistoryAggregate.average(days) { $0.asleepMin } ?? 0
            HistoryStatRow(label: "Average time asleep", value: Text(Fmt.minutes(Int(avg.rounded()))))
            ForEach(HistorySleepStage.stacked, id: \.self) { s in
                if let m = HistoryAggregate.average(days, { stageMinutes($0, s) }) {
                    HistoryStatRow(text: Text(verbatim: s.label), value: Text(Fmt.minutes(Int(m.rounded()))), dot: s.color)
                }
            }
        case .heart:
            if let r = HistoryAggregate.average(days, { $0.restingHr }) {
                HistoryStatRow(label: "Average resting heart rate", value: Text("\(Int(r.rounded())) bpm"))
            }
            if let h = HistoryAggregate.average(days, { $0.hrvMs }) {
                HistoryStatRow(label: "Average HRV", value: Text("\(Int(h.rounded())) ms"))
            }
            if let lo = days.compactMap({ $0.heartRateMin }).filter({ $0 > 0 }).min(),
               let hi = days.compactMap({ $0.heartRateMax }).max() {
                HistoryStatRow(label: "Heart rate range", value: Text("\(Int(lo)) – \(Int(hi)) bpm"))
            }
        case .energy:
            let avg = HistoryAggregate.average(days) { $0.activeKcal } ?? 0
            let total = HistoryAggregate.total(days) { $0.activeKcal }
            let exercise = HistoryAggregate.average(days) { $0.exerciseMin }
            HistoryStatRow(label: "Daily average", value: Text("\(Fmt.number(Int(avg.rounded()))) kcal"))
            HistoryStatRow(label: "Total", value: Text("\(Fmt.number(Int(total.rounded()))) kcal"))
            if let exercise {
                HistoryStatRow(label: "Average exercise", value: Text(Fmt.minutes(Int(exercise.rounded()))))
            }
        }
    }

    private func stageMinutes(_ d: HistoryDailyEntry, _ s: HistorySleepStage) -> Double? {
        switch s {
        case .deep: return d.deepMin
        case .core: return d.coreMin
        case .rem: return d.remMin
        case .awake: return d.awakeMin
        default: return nil
        }
    }

    @ViewBuilder private func valueText(_ d: HistoryDailyEntry) -> some View {
        switch metric {
        case .steps:
            Text("\(Fmt.number(Int(d.steps ?? 0))) steps")
        case .sleep:
            Text(Fmt.minutes(Int((d.asleepMin ?? 0).rounded())))
        case .heart:
            if let r = d.restingHr, r > 0, let h = d.hrvMs, h > 0 {
                Text("\(Int(r.rounded())) bpm · \(Int(h.rounded())) ms")
            } else if let r = d.restingHr, r > 0 {
                Text("\(Int(r.rounded())) bpm")
            } else if let h = d.hrvMs, h > 0 {
                Text("HRV \(Int(h.rounded())) ms")
            } else if let a = d.heartRateAvg {
                Text("\(Int(a.rounded())) bpm")
            }
        case .energy:
            Text("\(Fmt.number(Int((d.activeKcal ?? 0).rounded()))) kcal")
        }
    }
}

struct HistoryStatRow: View {
    let label: Text
    let value: Text
    var dot: Color? = nil

    init(label: LocalizedStringKey, value: Text, dot: Color? = nil) {
        self.init(text: Text(label), value: value, dot: dot)
    }

    init(text: Text, value: Text, dot: Color? = nil) {
        self.label = text
        self.value = value
        self.dot = dot
    }

    var body: some View {
        HStack {
            if let dot { Circle().fill(dot).frame(width: 8, height: 8) }
            label
            Spacer()
            value
                .monospacedDigit()
                .foregroundStyle(.secondary)
        }
    }
}
