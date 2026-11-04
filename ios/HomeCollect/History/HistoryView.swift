import SwiftUI

/// History tab: trends of steps, sleep, heart and energy from the server,
/// workouts, and a per-day view with heart rate, sleep stages and the map.
struct HistoryView: View {
    @StateObject private var store = HistoryStore()
    @State private var path: [HistoryRoute] = []
    @State private var showPicker = false

    var body: some View {
        NavigationStack(path: $path) {
            ScrollView {
                VStack(spacing: 16) {
                    Picker("Range", selection: $store.range) {
                        ForEach(HistoryRange.allCases) { r in
                            Text(r.title).tag(r)
                        }
                    }
                    .pickerStyle(.segmented)
                    .padding(.top, 4)

                    content
                }
                .padding(.horizontal, 16)
                .padding(.bottom, 24)
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("History")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button { showPicker = true } label: { Image(systemName: "calendar") }
                        .accessibilityLabel("Choose day")
                }
            }
            .refreshable { await store.load(force: true) }
            .task(id: store.range) { await store.load() }
            .sheet(isPresented: $showPicker) {
                HistoryDayPicker(initial: Calendar.current.startOfDay(for: Date())) { day in
                    path.append(.day(day))
                }
            }
            .navigationDestination(for: HistoryRoute.self) { route in
                switch route {
                case .metric(let m): HistoryMetricDetailView(metric: m)
                case .day(let d): HistoryDayView(date: d)
                case .workouts: HistoryWorkoutsView()
                }
            }
        }
        .environmentObject(store)
    }

    @ViewBuilder private var content: some View {
        if store.forbidden {
            ContentUnavailableView {
                Label("History needs the iPhone data permission", systemImage: "lock.fill")
            } description: {
                Text("Ask the admin to enable \"iPhone data\" for your account.")
            }
            .frame(minHeight: 420)
        } else if !store.loaded {
            if let error = store.error {
                ContentUnavailableView {
                    Label("Couldn't load history", systemImage: "wifi.exclamationmark")
                } description: {
                    Text(error)
                } actions: {
                    Button("Try again") { Task { await store.load(force: true) } }
                        .buttonStyle(.borderedProminent)
                }
                .frame(minHeight: 420)
            } else {
                ProgressView()
                    .controlSize(.large)
                    .frame(maxWidth: .infinity, minHeight: 420)
            }
        } else if !store.hasData {
            ContentUnavailableView {
                Label("No data yet", systemImage: "chart.xyaxis.line")
            } description: {
                Text("Health data uploaded from this iPhone will show up here.")
            }
            .frame(minHeight: 420)
        } else {
            if let error = store.error {
                Label(error, systemImage: "exclamationmark.triangle.fill")
                    .font(.footnote)
                    .foregroundStyle(.orange)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            cards
        }
    }

    @ViewBuilder private var cards: some View {
        let days = store.days
        let range = store.range
        NavigationLink(value: HistoryRoute.metric(.steps)) { HistoryStepsCard(days: days, range: range) }
            .buttonStyle(.plain)
        NavigationLink(value: HistoryRoute.metric(.sleep)) { HistorySleepCard(days: days, range: range) }
            .buttonStyle(.plain)
        NavigationLink(value: HistoryRoute.metric(.heart)) { HistoryHeartCard(days: days, range: range) }
            .buttonStyle(.plain)
        NavigationLink(value: HistoryRoute.metric(.energy)) { HistoryEnergyCard(days: days, range: range) }
            .buttonStyle(.plain)
        HistoryWorkoutsCard(workouts: store.workouts)
        if range.weekly {
            Text("Year charts show weekly averages.")
                .font(.footnote)
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}
