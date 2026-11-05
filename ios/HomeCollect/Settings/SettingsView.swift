import CoreLocation
import SwiftUI
import UIKit

/// Settings tab: account, app lock, language, what this iPhone collects and
/// the server-side preferences (notifications, home automation, experiments).
struct SettingsView: View {
    @EnvironmentObject private var engine: SyncEngine
    @EnvironmentObject private var router: AppRouter
    @StateObject private var store = PrefsStore()

    @AppStorage("appLockEnabled") private var appLockEnabled = false

    // Collect toggles: same keys as before, read/written through Settings
    // so they follow wherever Settings keeps its values.
    @State private var healthEnabled = Settings.healthEnabled
    @State private var locationEnabled = Settings.locationEnabled
    @State private var detailedLocation = Settings.detailedLocation
    @State private var deviceStatsEnabled = Settings.deviceStatsEnabled

    @State private var confirmSignOut = false

    var body: some View {
        NavigationStack {
            Form {
                accountSection
                securitySection
                languageSection
                if engine.canCollect { collectSection }
                if store.prefs != nil {
                    notificationsSection
                    if engine.canCollect { automationSection }
                    experimentSection
                } else {
                    prefsPlaceholder
                }
            }
            .navigationTitle("Settings")
            .task { await store.load() }
            .refreshable { await store.load() }
            .onChange(of: healthEnabled) { _, v in Settings.healthEnabled = v; engine.settingsChanged() }
            .onChange(of: locationEnabled) { _, v in Settings.locationEnabled = v; engine.settingsChanged() }
            .onChange(of: detailedLocation) { _, v in Settings.detailedLocation = v; engine.settingsChanged() }
            .onChange(of: deviceStatsEnabled) { _, v in Settings.deviceStatsEnabled = v }
            .confirmationDialog("Sign out?", isPresented: $confirmSignOut, titleVisibility: .visible) {
                Button("Sign out", role: .destructive) { Task { await engine.signOut() } }
            } message: {
                Text("Data already on the server stays there.")
            }
            .alert("Could not save", isPresented: Binding(
                get: { store.saveError != nil },
                set: { if !$0 { store.saveError = nil } })) {
                Button("OK", role: .cancel) {}
            } message: {
                Text(store.saveError ?? "")
            }
        }
    }

    // MARK: Account

    private var accountSection: some View {
        Section("Account") {
            LabeledContent("Server", value: engine.serverHost)
            LabeledContent("User", value: engine.user)
            if let name = engine.displayName, !name.isEmpty {
                LabeledContent("Name", value: name)
            }
            LabeledContent("Version", value: Self.appVersion)
            if engine.isAdmin {
                NavigationLink {
                    UsersView()
                } label: {
                    Label("Users", systemImage: "person.2.badge.gearshape")
                }
            }
            Button("Sign out this iPhone", role: .destructive) { confirmSignOut = true }
        }
    }

    private static var appVersion: String {
        let info = Bundle.main.infoDictionary
        let version = info?["CFBundleShortVersionString"] as? String ?? "?"
        let build = info?["CFBundleVersion"] as? String ?? "?"
        return "\(version) (\(build))"
    }

    // MARK: Security

    private var securitySection: some View {
        Section {
            Toggle("Face ID lock", isOn: $appLockEnabled)
        } header: {
            Text("Security")
        } footer: {
            Text("Asks for Face ID when the app is opened. If Face ID is not available or fails, you can use your iPhone passcode instead.")
        }
    }

    // MARK: Language

    private var languageSection: some View {
        Section {
            Button {
                openSystemSettings()
            } label: {
                LabeledContent("App language") {
                    Image(systemName: "arrow.up.forward.app")
                }
            }
            .foregroundStyle(.primary)
            Picker("Notifications", selection: store.language) {
                Text(verbatim: "Deutsch").tag("de")
                Text(verbatim: "English").tag("en")
            }
            .disabled(store.prefs == nil)
        } header: {
            Text("Language")
        } footer: {
            Text("The app follows your iPhone language. Notifications from the server use the language chosen here.")
        }
    }

    // MARK: Collect

    private var collectSection: some View {
        Section {
            NavigationLink {
                StatusView()
            } label: {
                LabeledContent {
                    if engine.syncing {
                        ProgressView()
                    } else if engine.lastError != nil {
                        Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                    } else if let last = engine.lastSync {
                        Text("\(last, style: .relative) ago")
                    } else {
                        Text("never")
                    }
                } label: {
                    Label("Sync status", systemImage: "arrow.up.heart.fill")
                }
            }
            Toggle("Health", isOn: $healthEnabled)
            Toggle("Location history", isOn: $locationEnabled)
            if locationEnabled {
                Toggle("Detailed tracking", isOn: $detailedLocation)
                if engine.locationAuth != .authorizedAlways { allowAlwaysButton }
            }
            Toggle("Battery, storage, network", isOn: $deviceStatsEnabled)
        } header: {
            Text("Collect")
        } footer: {
            Text("Detailed tracking records a point every ~100 m for real trip lines but uses noticeably more battery. Without it, iOS reports places you visit and larger moves.")
        }
    }

    private var allowAlwaysButton: some View {
        Button("Allow location \"Always\" in Settings") { openSystemSettings() }
    }

    // MARK: Server preferences

    @ViewBuilder
    private var prefsPlaceholder: some View {
        Section {
            if let error = store.loadError {
                Text(error).foregroundStyle(.red)
                Button("Try again") { Task { await store.load() } }
            } else {
                HStack {
                    ProgressView()
                    Text("Loading preferences").foregroundStyle(.secondary)
                }
            }
        } header: {
            Text("Notifications and automations")
        }
    }

    private var notificationsSection: some View {
        Section {
            Toggle("Lights left on", isOn: store.bool(\.notifyLightsLeftOn))
            if store.prefs?.notifyLightsLeftOn == true {
                Stepper(value: store.lightsIdleMin, in: 5...240, step: 5) {
                    Text("After \(store.lightsIdleMin.wrappedValue) min")
                        .monospacedDigit()
                }
            }
            Toggle("Server problems", isOn: store.bool(\.notifyServer))
            Toggle("Morning briefing", isOn: store.bool(\.morningBriefing))
            Button("Show last briefing") { router.showBriefing = true }
        } header: {
            Text("Notifications")
        } footer: {
            Text("Notifications come via the Home Assistant app. The server needs HA_NOTIFY_TARGET to be set.")
        }
    }

    private var automationSection: some View {
        Section {
            Toggle("Lights on when I arrive after sunset", isOn: store.bool(\.arriveLights))
            Toggle("Lights off when I leave", isOn: store.bool(\.leaveLightsOff))
            Toggle("Pause music when I leave", isOn: store.bool(\.leavePauseMedia))
            if !locationEnabled {
                Text("Location history is off, so arriving and leaving are not detected.")
                    .font(.footnote)
                    .foregroundStyle(.orange)
            } else if engine.locationAuth != .authorizedAlways {
                allowAlwaysButton
            }
        } header: {
            Text("Home automation")
        } footer: {
            Text("Uses the home zone from Home Assistant. This iPhone reports when it arrives or leaves, which needs location access \"Always\".")
        }
    }

    private var experimentSection: some View {
        Section {
            Toggle("Adaptive wake light", isOn: store.bool(\.adaptiveWakeLight))
            Toggle("Bedtime reminder", isOn: store.bool(\.bedtimeReminder))
            sleepPatternRows
        } header: {
            Text("Experiment")
        } footer: {
            Text("The wake light slowly brightens before your usual wake time; the reminder comes before your usual bedtime. Both use the sleep pattern learned from your Health data.")
        }
    }

    @ViewBuilder
    private var sleepPatternRows: some View {
        if let p = store.pattern, p.weekday != nil || p.weekend != nil {
            if let w = p.weekday { patternRow("Weekdays", w) }
            if let w = p.weekend { patternRow("Weekends", w) }
        } else if store.patternLoaded {
            Text("Not enough sleep data yet")
                .foregroundStyle(.secondary)
        }
    }

    private func patternRow(_ title: LocalizedStringKey, _ w: SleepPattern.Window) -> some View {
        LabeledContent {
            VStack(alignment: .trailing, spacing: 2) {
                Text("\(w.bedtime ?? "–") – \(w.wake ?? "–")")
                    .monospacedDigit()
                if let n = w.nights {
                    Group {
                        if n == 1 { Text("1 night") } else { Text("\(n) nights") }
                    }
                    .font(.caption)
                    .foregroundStyle(.secondary)
                }
            }
        } label: {
            Label(title, systemImage: "bed.double")
        }
    }

    private func openSystemSettings() {
        if let url = URL(string: UIApplication.openSettingsURLString) {
            UIApplication.shared.open(url)
        }
    }
}
