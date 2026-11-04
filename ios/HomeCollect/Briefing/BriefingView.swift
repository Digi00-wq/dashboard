import SwiftUI

/// Morning briefing (GET /api/briefing), shown as a sheet when the morning
/// notification is opened (homecollect://briefing) or from Settings.
struct BriefingView: View {
    @Environment(\.dismiss) private var dismiss

    private enum Phase {
        case loading
        case loaded(Briefing)
        case empty
        case failed(String)
    }

    @State private var phase: Phase = .loading

    var body: some View {
        NavigationStack {
            content
                .background(Color(.systemGroupedBackground))
                .navigationTitle(title)
                .navigationBarTitleDisplayMode(.large)
                .toolbar {
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Done") { dismiss() }
                    }
                }
        }
        .task { await load() }
    }

    private var title: Text {
        if case .loaded(let b) = phase, let g = b.greeting, !g.isEmpty { return Text(verbatim: g) }
        return Text("Briefing")
    }

    @ViewBuilder
    private var content: some View {
        switch phase {
        case .loading:
            ProgressView()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        case .empty:
            ScrollView {
                ContentUnavailableView(
                    "No briefing yet",
                    systemImage: "sun.horizon",
                    description: Text("It's created in the morning."))
                    .padding(.top, 60)
            }
            .refreshable { await load() }
        case .failed(let message):
            ContentUnavailableView {
                Label("Could not load the briefing", systemImage: "exclamationmark.triangle")
            } description: {
                Text(message)
            } actions: {
                Button("Try again") {
                    phase = .loading
                    Task { await load() }
                }
            }
        case .loaded(let b):
            ScrollView {
                BriefingContent(briefing: b)
                    .padding(.horizontal)
                    .padding(.bottom, 24)
            }
            .refreshable { await load() }
        }
    }

    @MainActor
    private func load() async {
        guard let api = SyncEngine.shared.apiClient else {
            phase = .failed(String(localized: "Not signed in."))
            return
        }
        do {
            if let b = try await api.briefing() {
                phase = .loaded(b)
            } else {
                phase = .empty
            }
        } catch APIError.unauthorized {
            SyncEngine.shared.deviceRevoked()
            dismiss()
        } catch is CancellationError {
            // sheet closed
        } catch {
            phase = .failed(error.localizedDescription)
        }
    }
}

private struct BriefingContent: View {
    let briefing: Briefing

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            if let summary = briefing.summary, !summary.isEmpty {
                Text(summary)
                    .font(.body)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            if let at = briefing.generatedAt?.date {
                Text("Created \(at.formatted(date: .omitted, time: .shortened))")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            if let s = briefing.sleep, (s.total ?? 0) > 0 {
                SleepView(sleep: s.dashboardSleep)
                    .padding(16)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(Color(.secondarySystemGroupedBackground),
                                in: RoundedRectangle(cornerRadius: 22, style: .continuous))
            }
            if let w = briefing.weather, w.temperature != nil || w.condition != nil {
                weatherCard(w)
            }
            if let alarm = briefing.alarm?.date {
                alarmCard(alarm)
            }
            if let tasks = briefing.tasks, !tasks.isEmpty {
                tasksCard(tasks)
            }
            if let server = briefing.server {
                serverCard(server)
            }
            if let a = briefing.activity, a.steps != nil || a.restingHr != nil || a.hrvMs != nil {
                activityCard(a)
            }
            if let tips = briefing.tips, !tips.isEmpty {
                tipsCard(tips)
            }
        }
    }

    // MARK: Cards

    private func weatherCard(_ w: Briefing.WeatherInfo) -> some View {
        Card(title: "Weather", systemImage: "cloud.sun.fill", tint: .cyan) {
            HStack(alignment: .center, spacing: 14) {
                Image(systemName: WeatherCard.symbol(w.condition))
                    .symbolRenderingMode(.multicolor)
                    .font(.system(size: 40))
                VStack(alignment: .leading, spacing: 2) {
                    if let t = w.temperature {
                        Text("\(Int(t.rounded()))°")
                            .font(.system(size: 38, weight: .semibold, design: .rounded))
                    }
                    Text(WeatherCard.name(w.condition)).foregroundStyle(.secondary)
                }
                Spacer()
                VStack(alignment: .trailing, spacing: 6) {
                    if w.high != nil || w.low != nil {
                        Text("H \(degrees(w.high))  L \(degrees(w.low))")
                    }
                    if let p = w.precipitationProbability {
                        Label("\(Int(p.rounded())) %", systemImage: "drop.fill")
                    }
                }
                .font(.subheadline)
                .foregroundStyle(.secondary)
            }
        }
    }

    private func degrees(_ v: Double?) -> String {
        v.map { "\(Int($0.rounded()))°" } ?? "–"
    }

    private func alarmCard(_ alarm: Date) -> some View {
        Card(title: "Alarm", systemImage: "alarm.fill", tint: .orange) {
            HStack(alignment: .firstTextBaseline) {
                Text(alarm, style: .time)
                    .font(.system(size: 34, weight: .semibold, design: .rounded))
                Spacer()
                Text(alarm.formatted(.relative(presentation: .named)))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private func tasksCard(_ tasks: [Briefing.TaskItem]) -> some View {
        Card(title: "Due today", systemImage: "checklist", tint: .red) {
            VStack(alignment: .leading, spacing: 10) {
                ForEach(Array(tasks.enumerated()), id: \.offset) { _, task in
                    HStack(alignment: .firstTextBaseline, spacing: 10) {
                        Image(systemName: "circle")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(task.summary ?? "")
                            if let list = task.list, !list.isEmpty {
                                Text(list).font(.caption).foregroundStyle(.secondary)
                            }
                        }
                        Spacer()
                        if let due = task.due, !due.isEmpty {
                            Text(due).font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }
            }
        }
    }

    private func serverCard(_ server: Briefing.ServerHealth) -> some View {
        let issues = server.issues ?? []
        let ok = server.ok != false && issues.isEmpty
        return Card(title: "Server", systemImage: "server.rack", tint: ok ? .green : .orange) {
            if ok {
                Label {
                    Text("Everything is running")
                } icon: {
                    Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
                }
            } else if issues.isEmpty {
                Label {
                    Text("There is a problem")
                } icon: {
                    Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                }
            } else {
                VStack(alignment: .leading, spacing: 8) {
                    ForEach(Array(issues.enumerated()), id: \.offset) { _, issue in
                        Label {
                            Text(issue)
                        } icon: {
                            Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                        }
                    }
                }
            }
        }
    }

    private func activityCard(_ a: Briefing.ActivityInfo) -> some View {
        Card(title: "Activity", systemImage: "figure.walk", tint: .orange) {
            HStack(spacing: 12) {
                if let steps = a.steps {
                    MetricTile(systemImage: "figure.walk", value: Fmt.number(Int(steps.rounded())),
                               label: "Steps", tint: .orange)
                }
                if let r = a.restingHr {
                    MetricTile(systemImage: "heart.text.square", value: "\(Int(r.rounded()))",
                               label: "Resting HR", tint: .pink)
                }
                if let hrv = a.hrvMs {
                    MetricTile(systemImage: "waveform.path.ecg", value: "\(Int(hrv.rounded())) ms",
                               label: "HRV", tint: .purple)
                }
            }
        }
    }

    private func tipsCard(_ tips: [String]) -> some View {
        Card(title: "Tips", systemImage: "lightbulb.fill", tint: .yellow) {
            VStack(alignment: .leading, spacing: 10) {
                ForEach(Array(tips.enumerated()), id: \.offset) { _, tip in
                    Label {
                        Text(tip)
                    } icon: {
                        Image(systemName: "lightbulb").foregroundStyle(.yellow)
                    }
                }
            }
        }
    }
}
