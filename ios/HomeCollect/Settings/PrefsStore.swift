import Foundation
import SwiftUI

/// Server-side preferences of the signed-in user (GET/PUT /api/prefs) and
/// the learned sleep pattern. Changes are applied optimistically and only
/// the changed key is sent; on failure the old value comes back and
/// `saveError` is set.
@MainActor
final class PrefsStore: ObservableObject {
    @Published private(set) var prefs: ServerPrefs?
    @Published private(set) var loading = false
    @Published private(set) var loadError: String?
    @Published var saveError: String?

    @Published private(set) var pattern: SleepPattern?
    /// True once /api/sleep-pattern answered (pattern nil = not enough data).
    @Published private(set) var patternLoaded = false

    private var inFlight = 0
    private var idleTask: Task<Void, Never>?
    /// Value to restore if the debounced lightsIdleMin change fails
    /// (outer nil = nothing pending).
    private var idleRevert: Int?? = nil

    private var api: APIClient? { SyncEngine.shared.apiClient }

    /// Loads prefs and sleep pattern from the server.
    func load() async {
        // Don't overwrite optimistic values that are still on their way.
        guard let api, inFlight == 0, idleTask == nil, !loading else { return }
        loading = true
        defer { loading = false }
        do {
            let loaded = try await api.prefs()
            if inFlight == 0 && idleTask == nil { prefs = loaded }
            loadError = nil
        } catch APIError.unauthorized {
            SyncEngine.shared.deviceRevoked()
            return
        } catch {
            loadError = error.localizedDescription
        }
        do {
            pattern = try await api.sleepPattern()
            patternLoaded = true
        } catch {
            // keep whatever was shown before
        }
    }

    // MARK: Changes

    /// Binding for a Bool pref (saves on change).
    func bool(_ key: WritableKeyPath<ServerPrefs, Bool?>) -> Binding<Bool> {
        Binding(
            get: { self.prefs?[keyPath: key] ?? false },
            set: { self.set(key, $0) })
    }

    var language: Binding<String> {
        Binding(
            get: { self.prefs?.language ?? "de" },
            set: { self.set(\.language, $0) })
    }

    /// Stepper value; the PUT goes out once the user stops tapping.
    var lightsIdleMin: Binding<Int> {
        Binding(
            get: { self.prefs?.lightsIdleMin ?? 30 },
            set: { self.setLightsIdle($0) })
    }

    /// Sets one pref and saves it.
    func set<Value>(_ key: WritableKeyPath<ServerPrefs, Value?>, _ value: Value) {
        guard prefs != nil else { return }
        let old = prefs?[keyPath: key]
        prefs?[keyPath: key] = value
        var change = ServerPrefs()
        change[keyPath: key] = value
        Task { await push(change) { $0[keyPath: key] = old } }
    }

    private func setLightsIdle(_ minutes: Int) {
        guard prefs != nil else { return }
        let value = min(max(minutes, 5), 240)
        if idleRevert == nil { idleRevert = .some(prefs?.lightsIdleMin) }
        prefs?.lightsIdleMin = value
        idleTask?.cancel()
        idleTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 800_000_000)
            guard let self, !Task.isCancelled else { return }
            let revert: Int? = self.idleRevert ?? nil
            self.idleRevert = nil
            self.idleTask = nil
            var change = ServerPrefs()
            change.lightsIdleMin = value
            await self.push(change) { $0.lightsIdleMin = revert }
        }
    }

    /// PUT /api/prefs with only the change; reverts on failure.
    private func push(_ change: ServerPrefs, revert: @escaping (inout ServerPrefs) -> Void) async {
        guard let api else { return }
        inFlight += 1
        do {
            let full = try await api.updatePrefs(change)
            inFlight -= 1
            // Take the server's view only when nothing else is pending.
            if inFlight == 0 && idleTask == nil { prefs = full }
        } catch {
            inFlight -= 1
            if var p = prefs {
                revert(&p)
                prefs = p
            }
            if case APIError.unauthorized = error {
                SyncEngine.shared.deviceRevoked()
                return
            }
            saveError = error.localizedDescription
        }
    }
}
