import SwiftUI

// MARK: - Weather

/// Current weather + forecast days.
struct WeatherCard: View {
    let weather: Weather

    var body: some View {
        Card(title: "Weather", systemImage: "cloud.sun.fill", tint: .cyan) {
            HStack(alignment: .center, spacing: 14) {
                Image(systemName: Self.symbol(weather.condition))
                    .symbolRenderingMode(.multicolor)
                    .font(.system(size: 44))
                VStack(alignment: .leading, spacing: 2) {
                    if let t = weather.temperature {
                        Text("\(Int(t.rounded()))°")
                            .font(.system(size: 42, weight: .semibold, design: .rounded))
                    }
                    Text(Self.name(weather.condition)).foregroundStyle(.secondary)
                }
                Spacer()
                VStack(alignment: .trailing, spacing: 6) {
                    if let h = weather.humidity { Label("\(Int(h)) %", systemImage: "humidity.fill") }
                    if let w = weather.windSpeed {
                        Label("\(Int(w.rounded())) \(weather.windSpeedUnit ?? "km/h")", systemImage: "wind")
                    }
                }
                .font(.subheadline)
                .foregroundStyle(.secondary)
            }

            if let days = weather.forecast, !days.isEmpty {
                Divider()
                HStack(spacing: 0) {
                    ForEach(days.prefix(5)) { day in
                        VStack(spacing: 6) {
                            Text(dayName(day.time?.date))
                                .font(.caption.weight(.semibold))
                                .foregroundStyle(.secondary)
                            Image(systemName: Self.symbol(day.condition))
                                .symbolRenderingMode(.multicolor)
                                .font(.title3)
                                .frame(height: 26)
                            Text(day.temperature.map { "\(Int($0.rounded()))°" } ?? "–")
                                .font(.subheadline.weight(.semibold))
                            Text(day.tempLow.map { "\(Int($0.rounded()))°" } ?? "")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        .frame(maxWidth: .infinity)
                    }
                }
            }
        }
    }

    private func dayName(_ date: Date?) -> String {
        guard let date else { return "" }
        if Calendar.current.isDateInToday(date) { return String(localized: "Today") }
        return date.formatted(.dateTime.weekday(.abbreviated))
    }

    static func symbol(_ condition: String?) -> String {
        switch condition {
        case "sunny": return "sun.max.fill"
        case "clear-night": return "moon.stars.fill"
        case "partlycloudy": return "cloud.sun.fill"
        case "cloudy": return "cloud.fill"
        case "fog": return "cloud.fog.fill"
        case "rainy": return "cloud.rain.fill"
        case "pouring": return "cloud.heavyrain.fill"
        case "snowy": return "cloud.snow.fill"
        case "snowy-rainy": return "cloud.sleet.fill"
        case "hail": return "cloud.hail.fill"
        case "lightning": return "cloud.bolt.fill"
        case "lightning-rainy": return "cloud.bolt.rain.fill"
        case "windy", "windy-variant": return "wind"
        default: return "thermometer.medium"
        }
    }

    static func name(_ condition: String?) -> String {
        switch condition {
        case "sunny": return String(localized: "Sunny")
        case "clear-night": return String(localized: "Clear")
        case "partlycloudy": return String(localized: "Partly cloudy")
        case "cloudy": return String(localized: "Cloudy")
        case "fog": return String(localized: "Fog")
        case "rainy": return String(localized: "Rain")
        case "pouring": return String(localized: "Heavy rain")
        case "snowy": return String(localized: "Snow")
        case "snowy-rainy": return String(localized: "Sleet")
        case "hail": return String(localized: "Hail")
        case "lightning": return String(localized: "Thunderstorm")
        case "lightning-rainy": return String(localized: "Thunderstorm with rain")
        case "windy", "windy-variant": return String(localized: "Windy")
        default: return condition ?? ""
        }
    }
}

// MARK: - Health

/// Steps, heart rate, energy, sleep and last workout.
struct HealthCard: View {
    let phone: Phone?
    let sleep: Sleep?
    let activity: Activity?
    @EnvironmentObject private var engine: SyncEngine
    @State private var local: LocalToday?

    /// The dashboard shows this account's data and this iPhone reads
    /// HealthKit: take the values straight from here (fresher).
    private var isThisDevice: Bool {
        engine.canCollect && Settings.healthEnabled && HealthSync.available
            && (activity?.user == nil || activity?.user == Settings.user)
    }

    var body: some View {
        let steps = local?.steps ?? activity?.steps ?? phone?.steps
        let heart = local?.heartRate ?? activity?.heartRate ?? phone?.heartRate
        let kcal = local?.activeKcal ?? activity?.activeKcal

        Card(title: "Health", systemImage: "heart.fill", tint: .pink,
             trailing: local != nil ? AnyView(Text("This iPhone").font(.caption).foregroundStyle(.secondary)) : nil) {
            LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 12), count: 3),
                      alignment: .leading, spacing: 16) {
                if let steps { MetricTile(systemImage: "figure.walk", value: Fmt.number(steps), label: "Steps", tint: .orange) }
                if let heart { MetricTile(systemImage: "heart.fill", value: "\(heart)", label: "BPM", tint: .pink) }
                if let kcal { MetricTile(systemImage: "flame.fill", value: Fmt.number(Int(kcal.rounded())), label: "Active kcal", tint: .red) }
                if let r = activity?.restingHr { MetricTile(systemImage: "heart.text.square", value: "\(r)", label: "Resting HR", tint: .pink) }
                if let hrv = activity?.hrvMs { MetricTile(systemImage: "waveform.path.ecg", value: "\(Int(hrv.rounded())) ms", label: "HRV", tint: .purple) }
                if let vo2 = activity?.vo2max { MetricTile(systemImage: "lungs.fill", value: String(format: "%.1f", vo2), label: "VO2 max", tint: .teal) }
                if activity == nil, let b = phone?.battery {
                    MetricTile(systemImage: "battery.75percent", value: "\(b) %", label: "Battery", tint: .green)
                }
            }

            if let s = sleep, let total = s.total, total > 0 { SleepView(sleep: s) }

            if let w = activity?.lastWorkout {
                Divider()
                HStack(spacing: 10) {
                    Image(systemName: Self.workoutSymbol(w.activity))
                        .font(.title3)
                        .foregroundStyle(.green)
                        .frame(width: 28)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(Self.workoutName(w.activity)).font(.subheadline.weight(.semibold))
                        Text(workoutDetails(w)).font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
        }
        .task(id: isThisDevice) {
            guard isThisDevice else { local = nil; return }
            while !Task.isCancelled {
                local = await engine.health.today()
                try? await Task.sleep(nanoseconds: 60_000_000_000)
            }
        }
    }

    private func workoutDetails(_ w: Workout) -> String {
        var parts: [String] = []
        if let s = w.durationS { parts.append("\(s / 60) min") }
        if let k = w.kcal { parts.append("\(Int(k.rounded())) kcal") }
        if let d = w.distanceKm, d > 0.05 { parts.append(String(format: "%.1f km", d)) }
        if let end = w.end?.date { parts.append(end.formatted(.relative(presentation: .named))) }
        return parts.joined(separator: " · ")
    }

    static func workoutName(_ a: String?) -> String {
        switch a {
        case "basketball": return String(localized: "Basketball")
        case "football": return String(localized: "Football")
        case "americanFootball": return String(localized: "American football")
        case "running": return String(localized: "Run")
        case "walking": return String(localized: "Walk")
        case "hiking": return String(localized: "Hike")
        case "cycling": return String(localized: "Ride")
        case "swimming": return String(localized: "Swim")
        case "strength", "functionalStrength": return String(localized: "Strength training")
        case "hiit": return String(localized: "HIIT")
        case "yoga": return String(localized: "Yoga")
        default: return String(localized: "Workout")
        }
    }

    static func workoutSymbol(_ a: String?) -> String {
        switch a {
        case "basketball": return "figure.basketball"
        case "football": return "figure.soccer"
        case "americanFootball": return "figure.american.football"
        case "running": return "figure.run"
        case "walking": return "figure.walk"
        case "hiking": return "figure.hiking"
        case "cycling": return "figure.outdoor.cycle"
        case "swimming": return "figure.pool.swim"
        case "strength", "functionalStrength": return "figure.strengthtraining.traditional"
        case "yoga": return "figure.yoga"
        default: return "figure.mixed.cardio"
        }
    }
}

/// Last night: duration and phase bar.
struct SleepView: View {
    let sleep: Sleep

    private struct Phase: Identifiable {
        let name: String
        let minutes: Int
        let color: Color
        var id: String { name }
    }

    private var phases: [Phase] {
        [Phase(name: HistorySleepStage.deep.label, minutes: sleep.deep ?? 0, color: .indigo),
         Phase(name: HistorySleepStage.core.label, minutes: sleep.core ?? 0, color: .blue),
         Phase(name: HistorySleepStage.rem.label, minutes: sleep.rem ?? 0, color: .cyan),
         Phase(name: HistorySleepStage.awake.label, minutes: sleep.awake ?? 0, color: .orange)]
            .filter { $0.minutes > 0 }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Label("Sleep", systemImage: "bed.double.fill")
                    .font(.subheadline)
                    .foregroundStyle(.indigo)
                Spacer()
                Text(Fmt.minutes(sleep.total ?? 0)).font(.headline.monospacedDigit())
            }
            if !phases.isEmpty {
                GeometryReader { geo in
                    let total = Double(phases.reduce(0) { $0 + $1.minutes })
                    HStack(spacing: 2) {
                        ForEach(phases) { p in
                            p.color.frame(width: max(2, (geo.size.width - 6) * Double(p.minutes) / total))
                        }
                    }
                    .clipShape(Capsule())
                }
                .frame(height: 10)
                HStack(spacing: 12) {
                    ForEach(phases) { p in
                        HStack(spacing: 4) {
                            Circle().fill(p.color).frame(width: 7, height: 7)
                            Text(p.name).foregroundStyle(.secondary)
                            Text(Fmt.minutes(p.minutes))
                        }
                        .font(.caption2)
                    }
                }
            }
        }
        .padding(.top, 4)
    }
}

// MARK: - Room

/// Presence (here / last seen) and room light level.
struct RoomCard: View {
    let presence: Presence?
    let light: RoomLight?
    let location: GeoPoint?

    var body: some View {
        Card(title: "Room", systemImage: "sensor.fill", tint: .mint) {
            HStack(spacing: 12) {
                if let p = presence {
                    MetricTile(systemImage: p.isNow == true ? "person.fill" : "person",
                               value: p.isNow == true ? String(localized: "Present") : String(localized: "Empty"),
                               verbatimLabel: presenceDetail(p), tint: .mint)
                }
                if let l = light {
                    MetricTile(systemImage: l.isLight == true ? "sun.max.fill" : "moon.fill",
                               value: "\(l.luxValue ?? 0) lx",
                               verbatimLabel: l.isLight == true ? String(localized: "Bright") : String(localized: "Dark"), tint: .yellow)
                }
                if let loc = location, let home = loc.home {
                    MetricTile(systemImage: home ? "house.fill" : "location.fill",
                               value: home ? String(localized: "Home") : String(localized: "Away"),
                               verbatimLabel: loc.time.map { "iPhone · " + $0.date.formatted(.relative(presentation: .named)) } ?? "iPhone",
                               tint: .blue)
                }
            }
        }
    }

    private func presenceDetail(_ p: Presence) -> String {
        if p.isNow == true { return String(localized: "right now") }
        guard let last = p.lastSeen?.date else { return "" }
        return String(localized: "last \(last.formatted(.relative(presentation: .named)))")
    }
}

// MARK: - Server

/// Server CPU, RAM, disk, temperature, uptime and internet speed.
struct ServerCard: View {
    let server: ServerInfo
    let network: NetworkInfo?

    var body: some View {
        Card(verbatim: server.hostname ?? String(localized: "Server"), systemImage: "server.rack", tint: .indigo,
             trailing: server.uptimeSeconds.map {
                AnyView(Text("up \(Fmt.uptime($0.value))").font(.caption).foregroundStyle(.secondary))
             }) {
            VStack(spacing: 12) {
                if let cpu = server.cpuPercent {
                    UsageRow(label: "CPU", value: cpu / 100,
                             detail: String(format: "%.0f %%", cpu)
                                 + (server.load1.map { " · " + String(format: String(localized: "load %.2f"), $0) } ?? ""))
                }
                if let used = server.memUsed?.value, let total = server.memTotal?.value, total > 0 {
                    UsageRow(label: "Memory", value: Double(used) / Double(total),
                             detail: String(localized: "\(Fmt.bytes(used)) of \(Fmt.bytes(total))"))
                }
                if let used = server.diskUsed?.value, let total = server.diskTotal?.value, total > 0 {
                    UsageRow(label: "Disk", value: Double(used) / Double(total),
                             detail: String(localized: "\(Fmt.bytes(used)) of \(Fmt.bytes(total))"))
                }
            }
            HStack(spacing: 12) {
                if let t = server.temperature {
                    MetricTile(systemImage: "thermometer.medium", value: "\(Int(t.rounded()))°C", label: "CPU temp",
                               tint: t > 75 ? .red : .orange)
                }
                if let net = network {
                    MetricTile(systemImage: "arrow.down", value: Fmt.speed(kib: net.downloadKib ?? 0), label: "Download", tint: .blue)
                    MetricTile(systemImage: "arrow.up", value: Fmt.speed(kib: net.uploadKib ?? 0), label: "Upload", tint: .green)
                }
            }
        }
    }
}

/// Label + progress bar + value.
struct UsageRow: View {
    let label: LocalizedStringKey
    let value: Double // 0...1
    let detail: String

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack {
                Text(label).font(.subheadline)
                Spacer()
                Text(detail).font(.caption.monospacedDigit()).foregroundStyle(.secondary)
            }
            ProgressView(value: min(max(value, 0), 1))
                .tint(value > 0.9 ? .red : value > 0.75 ? .orange : .indigo)
        }
    }
}
