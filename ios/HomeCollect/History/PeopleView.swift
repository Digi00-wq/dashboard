import SwiftUI

/// Toolbar menu of the People tab: which user's history is shown.
struct PeopleSwitcher {
    let users: [HistoryUser]
    let selected: String
    let select: (String) -> Void

    var menu: some View {
        Menu {
            Picker("Person", selection: Binding(get: { selected }, set: select)) {
                ForEach(users) { u in
                    Label {
                        Text(verbatim: u.name)
                    } icon: {
                        Image(systemName: u.hasData == true ? "person.fill" : "person")
                    }
                    .tag(u.name)
                }
            }
        } label: {
            Image(systemName: "person.2.fill")
        }
        .accessibilityLabel("Choose person")
    }
}

/// People tab (admins only): the History screens for another user's data.
struct PeopleView: View {
    @AppStorage("peopleSelected") private var selected = ""
    @State private var users: [HistoryUser] = []
    @State private var loaded = false
    @State private var error: String?

    var body: some View {
        Group {
            if let current = users.first(where: { $0.name == selected }) ?? defaultUser {
                HistoryView(subject: current.name,
                            switcher: PeopleSwitcher(users: users, selected: current.name) { selected = $0 })
                    .id(current.name) // fresh store and navigation per person
            } else {
                NavigationStack {
                    placeholder
                        .navigationTitle("People")
                }
            }
        }
        .task { await load() }
    }

    /// First user with data, else the first one.
    private var defaultUser: HistoryUser? {
        users.first(where: { $0.hasData == true }) ?? users.first
    }

    @ViewBuilder private var placeholder: some View {
        if let error {
            ContentUnavailableView {
                Label("Couldn't load people", systemImage: "wifi.exclamationmark")
            } description: {
                Text(error)
            } actions: {
                Button("Try again") { Task { await load() } }
                    .buttonStyle(.borderedProminent)
            }
        } else if loaded {
            ContentUnavailableView {
                Label("No other users", systemImage: "person.2.slash")
            } description: {
                Text("Add users under \"Manage users\" in the web dashboard. Their data shows up here once their iPhone uploads it.")
            }
        } else {
            ProgressView().controlSize(.large)
        }
    }

    private func load() async {
        guard let api = SyncEngine.shared.apiClient else { return }
        do {
            users = try await api.historyUsers()
            error = nil
        } catch is CancellationError {
            return
        } catch {
            if users.isEmpty { self.error = error.localizedDescription }
        }
        loaded = true
    }
}
