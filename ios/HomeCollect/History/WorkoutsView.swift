import SwiftUI

struct HistoryWorkoutRow: View {
    let workout: HistoryWorkout

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: HealthCard.workoutSymbol(workout.activity))
                .font(.title3)
                .foregroundStyle(.green)
                .frame(width: 36, height: 36)
                .background(Color.green.opacity(0.15), in: Circle())
            VStack(alignment: .leading, spacing: 2) {
                Text(HealthCard.workoutName(workout.activity))
                    .font(.subheadline.weight(.semibold))
                if let start = workout.startDate {
                    Text(start.formatted(.dateTime.weekday(.abbreviated).day().month(.abbreviated).hour().minute()))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            Spacer(minLength: 8)
            VStack(alignment: .trailing, spacing: 2) {
                if let m = workout.durationMin {
                    Text(Fmt.minutes(m)).font(.subheadline.monospacedDigit())
                }
                HStack(spacing: 6) {
                    if let k = workout.kcal, k > 0 {
                        Text("\(Int(k.rounded())) kcal")
                    }
                    if let d = workout.distanceKm, d > 0.05 {
                        Text(String(format: "%.1f km", d))
                    }
                }
                .font(.caption.monospacedDigit())
                .foregroundStyle(.secondary)
            }
        }
        .contentShape(Rectangle())
    }
}

/// Latest workouts on the History overview.
struct HistoryWorkoutsCard: View {
    let workouts: [HistoryWorkout]

    var body: some View {
        Card(title: "Workouts", systemImage: "figure.run", tint: .green,
             trailing: workouts.isEmpty ? nil : AnyView(
                Text("\(workouts.count)").font(.caption.monospacedDigit()).foregroundStyle(.secondary)
             )) {
            if workouts.isEmpty {
                HistoryNoData()
            } else {
                VStack(spacing: 12) {
                    ForEach(workouts.prefix(4)) { w in
                        NavigationLink(value: w.startDate.map { HistoryRoute.day(Calendar.current.startOfDay(for: $0)) }) {
                            HistoryWorkoutRow(workout: w)
                        }
                        .buttonStyle(.plain)
                        if w.id != workouts.prefix(4).last?.id { Divider() }
                    }
                }
                if workouts.count > 4 {
                    Divider()
                    NavigationLink(value: HistoryRoute.workouts) {
                        HStack {
                            Text("Show all workouts")
                            Spacer()
                            Image(systemName: "chevron.right")
                                .font(.footnote.weight(.semibold))
                                .foregroundStyle(.tertiary)
                        }
                        .font(.subheadline)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(Color.accentColor)
                }
            }
        }
    }
}

/// All workouts of the selected range, grouped by month.
struct HistoryWorkoutsView: View {
    @EnvironmentObject private var store: HistoryStore

    private struct Month: Identifiable {
        let start: Date
        let workouts: [HistoryWorkout]
        var id: Date { start }
    }

    private var months: [Month] {
        let cal = Calendar.current
        let grouped = Dictionary(grouping: store.workouts.filter { $0.startDate != nil }) { w in
            cal.dateInterval(of: .month, for: w.startDate!)?.start ?? w.startDate!
        }
        return grouped.keys.sorted(by: >).map { key in
            Month(start: key, workouts: (grouped[key] ?? []).sorted { ($0.start ?? 0) > ($1.start ?? 0) })
        }
    }

    var body: some View {
        List {
            if months.isEmpty {
                ContentUnavailableView("No workouts", systemImage: "figure.run",
                                       description: Text("No workouts in this period."))
            }
            ForEach(months) { month in
                Section {
                    ForEach(month.workouts) { w in
                        NavigationLink(value: HistoryRoute.day(Calendar.current.startOfDay(for: w.startDate ?? Date()))) {
                            HistoryWorkoutRow(workout: w)
                        }
                    }
                } header: {
                    HStack {
                        Text(month.start.formatted(.dateTime.month(.wide).year()))
                        Spacer()
                        Text(summary(month.workouts))
                            .textCase(nil)
                    }
                }
            }
        }
        .listStyle(.insetGrouped)
        .navigationTitle("Workouts")
        .navigationBarTitleDisplayMode(.inline)
        .refreshable { await store.load(force: true) }
    }

    private func summary(_ ws: [HistoryWorkout]) -> String {
        let minutes = ws.compactMap(\.durationMin).reduce(0, +)
        let kcal = Int(ws.compactMap(\.kcal).reduce(0, +).rounded())
        return String(localized: "\(ws.count) workouts · \(Fmt.minutes(minutes)) · \(kcal) kcal")
    }
}
