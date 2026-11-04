import SwiftUI

struct DashboardView: View {
    @EnvironmentObject private var live: LiveStore
    @State private var showAnnounce = false

    var body: some View {
        NavigationStack {
            ScrollView {
                if let d = live.dashboard {
                    VStack(spacing: 16) {
                        GlanceRow(dashboard: d)
                        if let errors = d.errors, !errors.isEmpty { ErrorBox(errors: errors) }
                        if let players = d.glance?.media, !players.isEmpty { MediaCard(players: players) }
                        if let lamps = d.lamps, !lamps.isEmpty { LightsCard(lamps: lamps) }
                        if let w = d.weather { WeatherCard(weather: w) }
                        if d.phone != nil || d.sleep != nil || d.activity != nil {
                            HealthCard(phone: d.phone, sleep: d.sleep, activity: d.activity)
                        }
                        if let lists = d.todoist?.lists, !lists.isEmpty { TasksCard(lists: lists) }
                        if d.presence != nil || d.light != nil {
                            RoomCard(presence: d.presence, light: d.light, location: d.activity?.location)
                        }
                        if let s = d.server { ServerCard(server: s, network: d.glance?.network) }
                    }
                    .padding(.horizontal)
                    .padding(.bottom, 24)
                    .animation(.default, value: d.updatedAt)
                }
            }
            .background(Color(.systemGroupedBackground))
            .overlay {
                if live.dashboard == nil {
                    VStack(spacing: 12) {
                        ProgressView()
                        Text(live.lastError ?? "Connecting…")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                    }
                    .padding()
                }
            }
            .navigationTitle(greeting)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    if live.dashboard != nil && !live.connected {
                        Image(systemName: "wifi.exclamationmark")
                            .foregroundStyle(.orange)
                            .accessibilityLabel("Reconnecting")
                    }
                }
                ToolbarItem(placement: .topBarTrailing) {
                    if live.dashboard?.voiceAvailable == true {
                        Button {
                            showAnnounce = true
                        } label: {
                            Image(systemName: "megaphone.fill")
                        }
                        .accessibilityLabel("Announcement")
                    }
                }
            }
            .refreshable { await live.refresh() }
            .sheet(isPresented: $showAnnounce) { AnnounceSheet() }
        }
    }

    private var greeting: String {
        let hour = Calendar.current.component(.hour, from: Date())
        let part = hour < 5 ? "Good night" : hour < 12 ? "Good morning" : hour < 18 ? "Good afternoon" : "Good evening"
        let name = (Settings.user ?? "").prefix(1).uppercased() + (Settings.user ?? "").dropFirst()
        return name.isEmpty ? part : "\(part), \(name)"
    }
}

// MARK: - At a glance

struct GlanceRow: View {
    let dashboard: Dashboard

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                if let alarm = dashboard.glance?.nextAlarm?.date, alarm > Date() {
                    GlanceChip(systemImage: "alarm.fill", text: alarmText(alarm), tint: .orange)
                }
                if let p = dashboard.presence, let isNow = p.isNow {
                    if isNow {
                        GlanceChip(systemImage: "house.fill", text: "Home", tint: .green)
                    } else if let since = p.lastSeen?.date {
                        GlanceChip(systemImage: "figure.walk", text: "Away \(since.formatted(.relative(presentation: .named)))")
                    }
                }
                if let net = dashboard.glance?.network {
                    if net.online == false {
                        GlanceChip(systemImage: "wifi.slash", text: "Internet down", tint: .red)
                    } else {
                        GlanceChip(systemImage: "arrow.down.circle.fill",
                                   text: Fmt.speed(kib: net.downloadKib ?? 0), tint: .blue)
                    }
                }
                if let battery = dashboard.activity?.battery {
                    GlanceChip(systemImage: battery < 0.2 ? "battery.25percent" : "battery.75percent",
                               text: "\(Int((battery * 100).rounded())) %",
                               tint: battery < 0.2 ? .red : .green)
                }
            }
            .padding(.vertical, 2)
        }
    }

    private func alarmText(_ date: Date) -> String {
        let time = date.formatted(date: .omitted, time: .shortened)
        if Calendar.current.isDateInToday(date) { return time }
        if Calendar.current.isDateInTomorrow(date) { return "Tomorrow \(time)" }
        return date.formatted(.dateTime.weekday(.abbreviated).hour().minute())
    }
}

struct ErrorBox: View {
    let errors: [String]

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Label("Problems", systemImage: "exclamationmark.triangle.fill")
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(.orange)
            ForEach(errors, id: \.self) { e in
                Text(e).font(.caption).foregroundStyle(.secondary)
            }
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.orange.opacity(0.12), in: RoundedRectangle(cornerRadius: 22, style: .continuous))
    }
}
