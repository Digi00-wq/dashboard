import SwiftUI

// Settings -> Users (admins only): change how people are called, what they
// may see and control, disable them or set a new password. Same API as the
// web admin page (/api/users).

/// User as returned by GET /api/users.
struct AdminUser: Decodable, Identifiable, Hashable {
    let name: String          // username (login, fixed)
    let displayName: String?  // how the dashboard talks to them
    let admin: Bool
    let permissions: [String]
    let disabled: Bool
    var id: String { name }
    var title: String { (displayName?.isEmpty == false ? displayName : nil) ?? name }
}

struct AdminUsersResponse: Decodable {
    let users: [AdminUser]
    let permissions: [String]
}

extension APIClient {
    func adminUsers() async throws -> AdminUsersResponse {
        try JSONDecoder().decode(AdminUsersResponse.self, from: try await send("api/users"))
    }

    /// Only the given fields change (displayName "" = back to the default).
    func updateUser(_ name: String, _ patch: [String: Any]) async throws -> AdminUser {
        let body = try JSONSerialization.data(withJSONObject: patch)
        let data = try await send("api/users/\(name)", method: "PUT", body: body)
        return try JSONDecoder().decode(AdminUser.self, from: data)
    }
}

/// Backend error codes -> messages; permission labels.
enum UserErrors {
    /// Server error codes of /api/users -> text.
    static func describe(_ error: Error) -> String {
        if case .server(_, let code)? = error as? APIError {
            switch code {
            case "invalid_display_name": return String(localized: "Name: 1–40 characters, without < > & \"")
            case "cannot_lock_out_self": return String(localized: "You can't disable yourself or remove your own admin rights.")
            case "last_admin": return String(localized: "There must be at least one admin.")
            case "weak_password": return String(localized: "Password too short (at least 8 characters).")
            default: break
            }
        }
        return error.localizedDescription
    }

    static func permissionTitle(_ p: String) -> String {
        switch p {
        case "weather": return String(localized: "Weather")
        case "lights": return String(localized: "Lights")
        case "room": return String(localized: "Room")
        case "tasks": return String(localized: "Tasks")
        case "media": return String(localized: "Music")
        case "health": return String(localized: "Health")
        case "server": return String(localized: "Server")
        case "announce": return String(localized: "Announcements")
        case "collect": return String(localized: "iPhone data")
        default: return p
        }
    }
}

// MARK: - List

/// Admin: list of users.
struct UsersView: View {
    @EnvironmentObject private var engine: SyncEngine
    @State private var users: [AdminUser] = []
    @State private var permissions: [String] = []
    @State private var error: String?
    @State private var loaded = false

    var body: some View {
        List {
            if let error {
                Section { Text(error).foregroundStyle(.red) }
            }
            Section {
                ForEach(users) { u in
                    NavigationLink {
                        UserEditView(user: u, permissions: permissions, isSelf: u.name == engine.user) { updated in
                            if let i = users.firstIndex(where: { $0.name == updated.name }) { users[i] = updated }
                        }
                    } label: {
                        row(u)
                    }
                }
            } footer: {
                Text("New users are created in the web dashboard.")
            }
        }
        .overlay { if !loaded && error == nil { ProgressView() } }
        .navigationTitle("Users")
        .task { await load() }
        .refreshable { await load() }
    }

    private func row(_ u: AdminUser) -> some View {
        HStack(spacing: 12) {
            Text(String(u.title.prefix(1)).uppercased())
                .font(.headline)
                .frame(width: 36, height: 36)
                .background(Color.accentColor.opacity(0.15), in: Circle())
            VStack(alignment: .leading, spacing: 2) {
                Text(verbatim: u.title)
                Text(verbatim: "@\(u.name)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            if u.admin {
                Image(systemName: "shield.lefthalf.filled").foregroundStyle(.secondary)
            }
            if u.disabled {
                Text("Disabled").font(.caption).foregroundStyle(.red)
            }
        }
    }

    private func load() async {
        guard let api = SyncEngine.shared.apiClient else { return }
        do {
            let r = try await api.adminUsers()
            users = r.users
            permissions = r.permissions
            error = nil
        } catch is CancellationError {
            return
        } catch {
            self.error = UserErrors.describe(error)
        }
        loaded = true
    }
}

// MARK: - Edit

/// Admin: edit one user (name, password, admin, disabled, permissions, test notification).
struct UserEditView: View {
    @State private var user: AdminUser
    let permissions: [String]
    let isSelf: Bool
    let onChange: (AdminUser) -> Void

    @State private var displayName: String
    @State private var busy = false
    @State private var error: String?
    @State private var showPassword = false
    @State private var newPassword = ""
    @State private var testTitle = ""
    @State private var testMessage = ""
    @State private var testResult: String?

    init(user: AdminUser, permissions: [String], isSelf: Bool, onChange: @escaping (AdminUser) -> Void) {
        _user = State(initialValue: user)
        _displayName = State(initialValue: user.displayName ?? "")
        self.permissions = permissions
        self.isSelf = isSelf
        self.onChange = onChange
    }

    var body: some View {
        Form {
            Section {
                TextField("Name", text: $displayName)
                    .textContentType(.givenName)
                    .submitLabel(.done)
                    .onSubmit { Task { await saveName() } }
                LabeledContent("Username", value: user.name)
            } footer: {
                Text("How the dashboard talks to this person: greeting, morning briefing, Alexa. The username is used to sign in and can't be changed.")
            }

            Section {
                Toggle("Active", isOn: binding(\.disabled, invert: true, key: "disabled"))
                    .disabled(isSelf)
                Toggle("Admin", isOn: binding(\.admin, key: "admin"))
                    .disabled(isSelf)
            } footer: {
                if isSelf { Text("You can't disable yourself or remove your own admin rights.") }
            }

            Section {
                ForEach(permissions, id: \.self) { p in
                    Toggle(UserErrors.permissionTitle(p), isOn: permissionBinding(p))
                        .disabled(user.admin)
                }
            } header: {
                Text("Access")
            } footer: {
                if user.admin { Text("Admins can see and control everything.") }
            }

            Section {
                TextField("Title", text: $testTitle)
                TextField("Message", text: $testMessage, axis: .vertical)
                    .lineLimit(1...4)
                Button("Send notification") { Task { await sendTest() } }
                    .disabled(busy)
                if let testResult {
                    Text(testResult).font(.footnote).foregroundStyle(.secondary)
                }
            } header: {
                Text("Notification")
            } footer: {
                Text("Empty = a short test text. Arrives in this person's HomeCollect app.")
            }

            Section {
                Button("Set new password") { newPassword = ""; showPassword = true }
            } footer: {
                Text("Signs this person out on all devices.")
            }

            if let error {
                Section { Text(error).foregroundStyle(.red) }
            }
        }
        .navigationTitle(user.title)
        .toolbar {
            if busy { ToolbarItem(placement: .topBarTrailing) { ProgressView() } }
            else if displayName.trimmingCharacters(in: .whitespaces) != (user.displayName ?? "") {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Save") { Task { await saveName() } }
                }
            }
        }
        .alert("New password", isPresented: $showPassword) {
            SecureField("At least 8 characters", text: $newPassword)
            Button("Cancel", role: .cancel) {}
            Button("Save") { Task { await save(["password": newPassword]) } }
        } message: {
            Text(verbatim: "@\(user.name)")
        }
    }

    private func sendTest() async {
        guard let api = SyncEngine.shared.apiClient else { return }
        busy = true
        defer { busy = false }
        do {
            try await api.testNotification(to: user.name, title: testTitle, message: testMessage)
            testResult = String(localized: "Sent.")
            testTitle = ""
            testMessage = ""
            error = nil
        } catch {
            testResult = nil
            self.error = UserErrors.describe(error)
        }
    }

    private func saveName() async {
        let trimmed = displayName.trimmingCharacters(in: .whitespaces)
        guard trimmed != (user.displayName ?? "") else { return }
        await save(["displayName": trimmed])
        displayName = user.displayName ?? ""
    }

    private func save(_ patch: [String: Any]) async {
        guard let api = SyncEngine.shared.apiClient else { return }
        busy = true
        defer { busy = false }
        do {
            user = try await api.updateUser(user.name, patch)
            error = nil
            onChange(user)
        } catch {
            self.error = UserErrors.describe(error)
        }
    }

    private func binding(_ path: KeyPath<AdminUser, Bool>, invert: Bool = false, key: String) -> Binding<Bool> {
        Binding(
            get: { user[keyPath: path] != invert },
            set: { v in Task { await save([key: v != invert]) } })
    }

    private func permissionBinding(_ p: String) -> Binding<Bool> {
        Binding(
            get: { user.admin || user.permissions.contains(p) },
            set: { on in
                var list = user.permissions.filter { $0 != p }
                if on { list.append(p) }
                Task { await save(["permissions": list]) }
            })
    }
}
