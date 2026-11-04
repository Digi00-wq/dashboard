import Charts
import MapKit
import SwiftUI

/// One day: heart rate, the night's sleep stages and the locations.
struct HistoryDayView: View {
    @State var date: Date
    @EnvironmentObject private var store: HistoryStore

    @State private var heart: [HistoryHeartRateSample]?
    @State private var sleep: [HistorySleepSegment]?
    @State private var sleepSource: String?
    @State private var locations: HistoryLocationsResponse?
    @State private var loading = false
    @State private var forbidden = false
    @State private var error: String?
    @State private var showPicker = false

    private var isToday: Bool { Calendar.current.isDateInToday(date) }

    var body: some View {
        ScrollView {
            VStack(spacing: 16) {
                dayHeader
                if forbidden {
                    ContentUnavailableView("History needs the iPhone data permission", systemImage: "lock.fill",
                                           description: Text("Ask the admin to enable \"iPhone data\" for your account."))
                } else {
                    if let error {
                        Label(error, systemImage: "exclamationmark.triangle.fill")
                            .font(.footnote)
                            .foregroundStyle(.orange)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    if let e = store.entry(for: date), e.hasAnyValue { HistoryDaySummaryCard(entry: e) }
                    DayHeartCard(samples: heart, date: date)
                    DaySleepCard(segments: sleep, source: sleepSource)
                    DayLocationsCard(response: locations)
                }
            }
            .padding(.horizontal, 16)
            .padding(.bottom, 24)
        }
        .background(Color(.systemGroupedBackground))
        .navigationTitle(date.formatted(.dateTime.weekday(.wide).day().month(.wide)))
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button { showPicker = true } label: { Image(systemName: "calendar") }
                    .accessibilityLabel("Choose day")
            }
        }
        .sheet(isPresented: $showPicker) {
            HistoryDayPicker(initial: date) { picked in date = picked }
        }
        .refreshable { await load() }
        .task(id: date) { await load() }
    }

    private var dayHeader: some View {
        HStack {
            Button { shift(-1) } label: {
                Image(systemName: "chevron.left").padding(8)
            }
            .accessibilityLabel("Previous day")
            Spacer()
            VStack(spacing: 2) {
                Text(date.formatted(.dateTime.weekday(.wide).day().month(.wide).year()))
                    .font(.headline)
                if loading { ProgressView().controlSize(.small) }
            }
            Spacer()
            Button { shift(1) } label: {
                Image(systemName: "chevron.right").padding(8)
            }
            .disabled(isToday)
            .accessibilityLabel("Next day")
        }
        .padding(.top, 8)
    }

    private func shift(_ days: Int) {
        if let d = Calendar.current.date(byAdding: .day, value: days, to: date) {
            date = min(Calendar.current.startOfDay(for: d), Calendar.current.startOfDay(for: Date()))
        }
    }

    private func load() async {
        guard let api = SyncEngine.shared.apiClient else { return }
        let day = date
        loading = true
        defer { if day == date { loading = false } }
        heart = nil; sleep = nil; sleepSource = nil; locations = nil; error = nil

        async let h = captureResult { try await api.historyHeartRate(date: day) }
        async let s = captureResult { try await api.historySleep(date: day) }
        async let l = captureResult { try await api.historyLocations(date: day) }
        let (hr, sl, loc) = await (h, s, l)
        guard day == date, !Task.isCancelled else { return }

        var failures: [Error] = []
        switch hr {
        case .success(let v): heart = v
        case .failure(let e): failures.append(e); heart = []
        }
        switch sl {
        case .success(let v): sleep = v.segments ?? []; sleepSource = v.source
        case .failure(let e): failures.append(e); sleep = []
        }
        switch loc {
        case .success(let v): locations = v
        case .failure(let e): failures.append(e); locations = HistoryLocationsResponse(points: nil, visits: nil)
        }
        forbidden = failures.contains { e in
            guard let api = e as? APIError, case .forbidden = api else { return false }
            return true
        }
        let real = failures.filter { !($0 is CancellationError) }
        error = real.first?.localizedDescription
    }
}

private func captureResult<T>(_ op: () async throws -> T) async -> Result<T, Error> {
    do { return .success(try await op()) } catch { return .failure(error) }
}

// MARK: - Date picker

struct HistoryDayPicker: View {
    let initial: Date
    let onPick: (Date) -> Void
    @State private var selection: Date
    @Environment(\.dismiss) private var dismiss

    init(initial: Date, onPick: @escaping (Date) -> Void) {
        self.initial = initial
        self.onPick = onPick
        _selection = State(initialValue: initial)
    }

    var body: some View {
        NavigationStack {
            DatePicker("Day", selection: $selection, in: ...Date(), displayedComponents: .date)
                .datePickerStyle(.graphical)
                .padding(.horizontal)
                .frame(maxHeight: .infinity, alignment: .top)
                .navigationTitle("Choose day")
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) {
                        Button("Cancel") { dismiss() }
                    }
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Show") {
                            onPick(Calendar.current.startOfDay(for: selection))
                            dismiss()
                        }
                    }
                }
        }
        .presentationDetents([.medium, .large])
    }
}

// MARK: - Summary

private struct HistoryDaySummaryCard: View {
    let entry: HistoryDailyEntry

    var body: some View {
        Card(title: "Activity", systemImage: "flame.fill", tint: .orange) {
            LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 12), count: 3),
                      alignment: .leading, spacing: 16) {
                if let s = entry.steps, s > 0 {
                    MetricTile(systemImage: "figure.walk", value: Fmt.number(Int(s)), label: "Steps", tint: .orange)
                }
                if let k = entry.activeKcal, k > 0 {
                    MetricTile(systemImage: "flame.fill", value: Fmt.number(Int(k.rounded())), label: "Active kcal", tint: .red)
                }
                if let d = entry.distanceKm, d > 0 {
                    MetricTile(systemImage: "map", value: String(format: "%.1f km", d), label: "Distance", tint: .green)
                }
                if let e = entry.exerciseMin, e > 0 {
                    MetricTile(systemImage: "figure.run", value: Fmt.minutes(Int(e)), label: "Exercise", tint: .green)
                }
                if let r = entry.restingHr, r > 0 {
                    MetricTile(systemImage: "heart.text.square", value: "\(Int(r))", label: "Resting HR", tint: .pink)
                }
                if let h = entry.hrvMs, h > 0 {
                    MetricTile(systemImage: "waveform.path.ecg", value: "\(Int(h.rounded())) ms", label: "HRV", tint: .purple)
                }
            }
        }
    }
}

// MARK: - Heart rate

private struct DayHeartCard: View {
    let samples: [HistoryHeartRateSample]?
    let date: Date

    /// Consecutive samples; a gap over 30 min starts a new line.
    private struct Point: Identifiable {
        let time: Date
        let bpm: Double
        let run: Int
        var id: Date { time }
    }

    private var points: [Point] {
        var run = 0
        var last: Double?
        return (samples ?? []).map { s in
            if let last, s.t - last > 30 * 60_000 { run += 1 }
            last = s.t
            return Point(time: s.time, bpm: s.bpm, run: run)
        }
    }

    private var dayDomain: ClosedRange<Date> {
        let start = Calendar.current.startOfDay(for: date)
        let end = Calendar.current.date(byAdding: .day, value: 1, to: start) ?? start
        return start...end
    }

    var body: some View {
        Card(title: "Heart rate", systemImage: "heart.fill", tint: .pink) {
            if samples == nil {
                ProgressView().frame(maxWidth: .infinity, minHeight: 160)
            } else if let samples, !samples.isEmpty {
                let bpms = samples.map(\.bpm)
                let lo = Int(bpms.min() ?? 0), hi = Int(bpms.max() ?? 0)
                let avg = Int((bpms.reduce(0, +) / Double(bpms.count)).rounded())
                HStack(alignment: .top, spacing: 24) {
                    HistoryHeadline(caption: "Range", value: Text("\(lo) – \(hi) bpm"))
                    HistoryHeadline(caption: "Average", value: Text("\(avg) bpm"))
                }
                Chart(points) { p in
                    LineMark(x: .value("Time", p.time),
                             y: .value("BPM", p.bpm),
                             series: .value("Segment", p.run))
                        .foregroundStyle(Color.pink)
                        .interpolationMethod(.catmullRom)
                        .lineStyle(StrokeStyle(lineWidth: 2))
                }
                .chartXScale(domain: dayDomain)
                .chartYScale(domain: .automatic(includesZero: false))
                .chartXAxis {
                    AxisMarks(values: .stride(by: .hour, count: 6)) { _ in
                        AxisGridLine()
                        AxisValueLabel(format: .dateTime.hour())
                    }
                }
                .frame(height: 180)
            } else {
                Text("No heart rate data for this day")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, minHeight: 60)
            }
        }
    }
}

// MARK: - Sleep

private struct DaySleepCard: View {
    let segments: [HistorySleepSegment]?
    let source: String?

    private struct Bar: Identifiable {
        let id: Int
        let start: Date
        let end: Date
        let stage: HistorySleepStage
    }

    private var bars: [Bar] {
        (segments ?? []).enumerated().compactMap { i, s in
            guard let stage = HistorySleepStage(rawValue: s.stage), s.end > s.start else { return nil }
            return Bar(id: i, start: s.startDate, end: s.endDate, stage: stage)
        }
    }

    /// Rows top to bottom: awake, REM, core, deep (+ generic stages if present).
    private var stages: [HistorySleepStage] {
        let present = Set(bars.map(\.stage))
        return HistorySleepStage.allCases.filter { present.contains($0) }
    }

    var body: some View {
        Card(title: "Sleep", systemImage: "bed.double.fill", tint: .indigo) {
            if segments == nil {
                ProgressView().frame(maxWidth: .infinity, minHeight: 120)
            } else if bars.isEmpty {
                Text("No sleep data for this night")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, minHeight: 60)
            } else {
                let staged = bars.contains { $0.stage == .deep || $0.stage == .core || $0.stage == .rem }
                let asleep = bars.filter { staged ? ($0.stage == .deep || $0.stage == .core || $0.stage == .rem) : $0.stage.isAsleep }
                    .reduce(0.0) { $0 + $1.end.timeIntervalSince($1.start) / 60 }
                let first = bars.map(\.start).min() ?? Date()
                let last = bars.map(\.end).max() ?? Date()
                HStack(alignment: .top, spacing: 24) {
                    HistoryHeadline(caption: "Time asleep", value: Text(Fmt.minutes(Int(asleep.rounded()))))
                    HistoryHeadline(caption: "In bed", value: Text("\(first.formatted(date: .omitted, time: .shortened)) – \(last.formatted(date: .omitted, time: .shortened))"))
                }
                Chart(bars) { b in
                    BarMark(xStart: .value("Start", b.start),
                            xEnd: .value("End", b.end),
                            y: .value("Stage", b.stage.label))
                        .foregroundStyle(b.stage.color)
                }
                .chartYScale(domain: stages.map(\.label))
                .chartXAxis {
                    AxisMarks(values: .stride(by: .hour, count: 2)) { _ in
                        AxisGridLine()
                        AxisValueLabel(format: .dateTime.hour())
                    }
                }
                .frame(height: CGFloat(max(stages.count, 2)) * 36 + 30)
                if let source, !source.isEmpty {
                    Text("Source: \(source)")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
    }
}

// MARK: - Locations

private struct DayLocationsCard: View {
    let response: HistoryLocationsResponse?
    @State private var camera: MapCameraPosition = .automatic

    private var points: [HistoryLocationPoint] {
        (response?.points ?? [])
            .filter { ($0.accuracy ?? 0) <= 200 }
            .sorted { $0.t < $1.t }
    }

    private var visits: [HistoryLocationVisit] {
        (response?.visits ?? []).sorted { ($0.arrive ?? 0) < ($1.arrive ?? 0) }
    }

    var body: some View {
        Card(title: "Locations", systemImage: "map.fill", tint: .blue) {
            if response == nil {
                ProgressView().frame(maxWidth: .infinity, minHeight: 200)
            } else if points.isEmpty && visits.isEmpty {
                Text("No locations for this day")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, minHeight: 60)
            } else {
                Map(position: $camera) {
                    if points.count > 1 {
                        MapPolyline(coordinates: points.map(\.coordinate))
                            .stroke(Color.blue, lineWidth: 4)
                    }
                    ForEach(Array(visits.enumerated()), id: \.offset) { _, v in
                        Marker(visitTitle(v), systemImage: "mappin", coordinate: v.coordinate)
                            .tint(.red)
                    }
                    if visits.isEmpty, let first = points.first {
                        Marker(first.time.formatted(date: .omitted, time: .shortened),
                               systemImage: "flag.fill", coordinate: first.coordinate)
                            .tint(.green)
                    }
                }
                .frame(height: 300)
                .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
                .onAppear { fit() }
                .onChange(of: points.map(\.t)) { fit() }

                if !visits.isEmpty {
                    VStack(alignment: .leading, spacing: 8) {
                        ForEach(Array(visits.enumerated()), id: \.offset) { _, v in
                            HStack(spacing: 10) {
                                Image(systemName: "mappin.circle.fill").foregroundStyle(.red)
                                Text(visitTitle(v)).font(.subheadline.monospacedDigit())
                                Spacer()
                                if let a = v.arriveDate, let d = v.departDate, d > a {
                                    Text(Fmt.minutes(Int(d.timeIntervalSince(a) / 60)))
                                        .font(.caption.monospacedDigit())
                                        .foregroundStyle(.secondary)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    private func visitTitle(_ v: HistoryLocationVisit) -> String {
        let a = v.arriveDate?.formatted(date: .omitted, time: .shortened)
        let d = v.departDate?.formatted(date: .omitted, time: .shortened)
        switch (a, d) {
        case let (a?, d?): return "\(a) – \(d)"
        case let (a?, nil): return String(localized: "since \(a)")
        case let (nil, d?): return String(localized: "until \(d)")
        default: return String(localized: "Visit")
        }
    }

    /// Region around all points and visits.
    private func fit() {
        let coords = points.map(\.coordinate) + visits.map(\.coordinate)
        guard let first = coords.first else { camera = .automatic; return }
        var minLat = first.latitude, maxLat = first.latitude
        var minLon = first.longitude, maxLon = first.longitude
        for c in coords {
            minLat = min(minLat, c.latitude); maxLat = max(maxLat, c.latitude)
            minLon = min(minLon, c.longitude); maxLon = max(maxLon, c.longitude)
        }
        let center = CLLocationCoordinate2D(latitude: (minLat + maxLat) / 2, longitude: (minLon + maxLon) / 2)
        let span = MKCoordinateSpan(latitudeDelta: max((maxLat - minLat) * 1.4, 0.01),
                                    longitudeDelta: max((maxLon - minLon) * 1.4, 0.01))
        camera = .region(MKCoordinateRegion(center: center, span: span))
    }
}
