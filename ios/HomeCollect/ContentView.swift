import CoreLocation
import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var engine: SyncEngine

    var body: some View {
        NavigationStack {
            if engine.signedIn { StatusView() } else { LoginView() }
        }
    }
}

// MARK: - Login

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

struct StatusView: View {
    @EnvironmentObject private var engine: SyncEngine
    @AppStorage("healthEnabled") private var healthEnabled = true
    @AppStorage("locationEnabled") private var locationEnabled = true
    @AppStorage("detailedLocation") private var detailedLocation = false
    @AppStorage("deviceStatsEnabled") private var deviceStatsEnabled = true
    @State private var confirmSignOut = false

    var body: some View {
        List {
            Section {
                LabeledContent("Server", value: engine.serverHost)
                LabeledContent("User", value: engine.user)
                LabeledContent("Last sync") {
                    if let last = engine.lastSync {
                        Text(last, style: .relative) + Text(" ago")
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

            Section {
                Toggle("Health", isOn: $healthEnabled)
                Toggle("Location history", isOn: $locationEnabled)
                if locationEnabled {
                    Toggle("Detailed tracking", isOn: $detailedLocation)
                    if engine.locationAuth != .authorizedAlways {
                        Button("Allow location \"Always\" in Settings") {
                            if let url = URL(string: UIApplication.openSettingsURLString) {
                                UIApplication.shared.open(url)
                            }
                        }
                    }
                }
                Toggle("Battery, storage, network", isOn: $deviceStatsEnabled)
            } header: {
                Text("Collect")
            } footer: {
                Text("Detailed tracking records a point every ~100 m for real trip lines but uses noticeably more battery. Without it, iOS reports places you visit and larger moves.")
            }

            Section {
                Button("Sign out this iPhone", role: .destructive) { confirmSignOut = true }
            }
        }
        .navigationTitle("HomeCollect")
        .refreshable { await engine.sync(reason: "manual") }
        .onChange(of: healthEnabled) { _, _ in engine.settingsChanged() }
        .onChange(of: locationEnabled) { _, _ in engine.settingsChanged() }
        .onChange(of: detailedLocation) { _, _ in engine.settingsChanged() }
        .confirmationDialog("Sign out?", isPresented: $confirmSignOut) {
            Button("Sign out", role: .destructive) { Task { await engine.signOut() } }
        } message: {
            Text("Data already on the server stays there.")
        }
    }

    private func row(_ title: String, _ summary: ServerStatus.Summary?, icon: String) -> some View {
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
        if s.isEmpty { return "Workouts" }
        // "RestingHeartRate" -> "Resting Heart Rate"
        var out = ""
        for ch in s {
            if ch.isUppercase, let last = out.last, last.isLowercase { out.append(" ") }
            out.append(ch)
        }
        return out
    }
}
