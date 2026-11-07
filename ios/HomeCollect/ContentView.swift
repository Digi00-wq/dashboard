// Root view: LoginView when signed out, otherwise the tab bar
// (Home, History*, People**, Claude**, Settings; *needs "collect", **admins only).
// Also: StatusView (upload status) and HealthTypesView (list of synced HealthKit types).

import CoreLocation
import SwiftUI

/// Tab bar or login, depending on SyncEngine.signedIn.
struct ContentView: View {
    @EnvironmentObject private var engine: SyncEngine
    @EnvironmentObject private var router: AppRouter

    var body: some View {
        if engine.signedIn {
            TabView(selection: $router.selectedTab) {
                DashboardView()
                    .tabItem { Label("Home", systemImage: "house.fill") }
                    .tag(AppRouter.Tab.home)
                if engine.canCollect {
                    HistoryView()
                        .tabItem { Label("History", systemImage: "chart.xyaxis.line") }
                        .tag(AppRouter.Tab.history)
                }
                if engine.isAdmin {
                    PeopleView()
                        .tabItem { Label("People", systemImage: "person.2.fill") }
                        .tag(AppRouter.Tab.people)
                    ClaudeView()
                        .tabItem { Label("Claude", systemImage: "sparkles") }
                        .tag(AppRouter.Tab.claude)
                }
                SettingsView()
                    .tabItem { Label("Settings", systemImage: "gearshape.fill") }
                    .tag(AppRouter.Tab.settings)
            }
            .sheet(isPresented: $router.showBriefing) { BriefingView() }
        } else {
            NavigationStack { LoginView() }
        }
    }
}

// MARK: - Login

/// Server address + username + password -> POST /api/devices/login (device key in Keychain).
struct LoginView: View {
    @EnvironmentObject private var engine: SyncEngine
    @State private var server = Settings.serverURL?.absoluteString ?? ""
    @State private var user = Settings.user ?? ""
    @State private var password = ""
    @State private var busy = false
    @State private var error: String?

    var body: some View {
        Form {
            Section {
                TextField("dashboard.example.ch", text: $server)
                    .keyboardType(.URL)
                    .textContentType(.URL)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
            } header: {
                Text("Server")
            } footer: {
                Text("The address of your dashboard.")
            }
            Section("Account") {
                TextField("User name", text: $user)
                    .textContentType(.username)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                SecureField("Password", text: $password)
                    .textContentType(.password)
            }
            if let error {
                Section { Text(error).foregroundStyle(.red) }
            }
            Section {
                Button {
                    Task { await signIn() }
                } label: {
                    HStack {
                        Text("Connect")
                        Spacer()
                        if busy { ProgressView() }
                    }
                }
                .disabled(busy || server.isEmpty || user.isEmpty || password.isEmpty)
            } footer: {
                Text("Your password is only used once to register this iPhone. The app then keeps a device key in the Keychain.")
            }
        }
        .navigationTitle("HomeCollect")
    }

    private func signIn() async {
        busy = true
        error = nil
        do {
            try await engine.signIn(server: server, user: user, password: password)
            password = ""
        } catch {
            self.error = error.localizedDescription
        }
        busy = false
    }
}

// MARK: - Status

/// Upload status of this iPhone (Settings -> Collect -> Sync status).
struct StatusView: View {
    @EnvironmentObject private var engine: SyncEngine

    var body: some View {
        List {
            Section {
                LabeledContent("Server", value: engine.serverHost)
                LabeledContent("User", value: engine.user)
                LabeledContent("Last sync") {
                    if let last = engine.lastSync {
                        Text("\(last, style: .relative) ago")
                    } else {
                        Text("never")
                    }
                }
                if let progress = engine.progress {
                    HStack {
                        ProgressView()
                        Text(progress).foregroundStyle(.secondary)
                    }
                }
                if let error = engine.lastError {
                    Text(error).foregroundStyle(.red)
                }
                Button("Sync now") { Task { await engine.sync(reason: "manual") } }
                    .disabled(engine.syncing)
            }

            Section("Stored on the server") {
                row("Health samples", engine.status?.health, icon: "heart")
                row("Locations", engine.status?.locations, icon: "location")
                row("Phone stats", engine.status?.device, icon: "iphone")
                if engine.pending > 0 {
                    LabeledContent("Waiting for upload", value: "\(engine.pending)")
                }
                if let types = engine.status?.healthTypes, !types.isEmpty {
                    NavigationLink("Health data types") { HealthTypesView(types: types) }
                }
            }

            if engine.canCollect && Settings.locationEnabled && engine.locationAuth != .authorizedAlways {
                Section {
                    Label("Location is not set to \"Always\": no history while the app is closed.",
                          systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                }
            }
        }
        .navigationTitle("Sync status")
        .refreshable { await engine.sync(reason: "manual") }
    }

    private func row(_ title: LocalizedStringKey, _ summary: ServerStatus.Summary?, icon: String) -> some View {
        LabeledContent {
            VStack(alignment: .trailing) {
                Text(summary.map { "\($0.count)" } ?? "–")
                if let newest = summary?.newest {
                    Text(Date(timeIntervalSince1970: Double(newest) / 1000), style: .date)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        } label: {
            Label(title, systemImage: icon)
        }
    }
}

/// Which HealthKit types are synced.
struct HealthTypesView: View {
    let types: [String: Int]

    var body: some View {
        List(types.sorted(by: { $0.value > $1.value }), id: \.key) { item in
            LabeledContent(prettyName(item.key), value: "\(item.value)")
        }
        .navigationTitle("Health data")
    }

    private func prettyName(_ id: String) -> String {
        var s = id
        for prefix in ["HKQuantityTypeIdentifier", "HKCategoryTypeIdentifier", "HKWorkoutTypeIdentifier"] {
            if s.hasPrefix(prefix) { s.removeFirst(prefix.count) }
        }
        if s.isEmpty { return String(localized: "Workouts") }
        // "RestingHeartRate" -> "Resting Heart Rate"
        var out = ""
        for ch in s {
            if ch.isUppercase, let last = out.last, last.isLowercase { out.append(" ") }
            out.append(ch)
        }
        return out
    }
}
