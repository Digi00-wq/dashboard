import BackgroundTasks
import CoreLocation
import HealthKit
import SwiftUI

/// Ties everything together: sign-in, collectors, uploads, background tasks.
@MainActor
final class SyncEngine: ObservableObject {
    static let shared = SyncEngine()

    nonisolated static let refreshTask = "homecollect.refresh"
    nonisolated static let backfillTask = "homecollect.backfill"

    @Published private(set) var signedIn = Settings.token != nil
    @Published private(set) var syncing = false
    @Published private(set) var progress: String?
    @Published private(set) var lastSync = Settings.lastSync
    @Published private(set) var lastError: String?
    @Published private(set) var status: ServerStatus?
    @Published private(set) var pending = 0
    @Published private(set) var locationAuth: CLAuthorizationStatus = .notDetermined

    let health = HealthSync()
    let location = LocationTracker()
    let outbox = Outbox()

    private init() {
        location.onRecord = { [weak self] points in self?.record(points) }
        location.onAuthorizationChange = { [weak self] s in self?.locationAuth = s }
        locationAuth = location.authorization
    }

    var serverHost: String { Settings.serverURL?.host ?? "" }
    var user: String { Settings.user ?? "" }

    private func client() -> APIClient? {
        guard let url = Settings.serverURL, let token = Settings.token else { return nil }
        return APIClient(base: url, token: token)
    }

    // MARK: Launch

    /// Called from didFinishLaunching (also on background relaunches by
    /// iOS for location / HealthKit events).
    func launch() {
        registerBackgroundTasks()
        DeviceStats.start()
        guard signedIn else { return }
        startCollectors(askPermissions: false)
    }

    private func startCollectors(askPermissions: Bool) {
        if Settings.healthEnabled && HealthSync.available {
            Task {
                if askPermissions { try? await health.requestAuthorization() }
                await health.startObserving { type in
                    await SyncEngine.shared.healthChanged(type)
                }
            }
        } else {
            Task { await health.stopObserving() }
        }
        if Settings.locationEnabled {
            location.start(detailed: Settings.detailedLocation)
        } else {
            location.stop()
        }
    }

    /// Re-applies the toggles after the user changed one.
    func settingsChanged() {
        guard signedIn else { return }
        startCollectors(askPermissions: true)
    }

    // MARK: Account

    func signIn(server: String, user: String, password: String) async throws {
        guard let url = APIClient.normalize(server) else { throw APIError.invalidURL }
        let response = try await APIClient(base: url, token: nil)
            .login(username: user, password: password, device: UIDevice.current.name)
        Settings.serverURL = url
        Settings.token = response.token
        Settings.user = response.user
        Settings.resetHealthAnchors() // new account/server: send the full history
        signedIn = true
        lastError = nil
        startCollectors(askPermissions: true)
        await sync(reason: "sign-in")
    }

    func signOut() async {
        try? await client()?.logout()
        forgetDevice()
    }

    private func forgetDevice() {
        Settings.token = nil
        signedIn = false
        status = nil
        location.stop()
        Task {
            await health.stopObserving()
            await outbox.clear()
            pending = 0
        }
    }

    // MARK: Sync

    func sync(reason: String) async {
        guard signedIn, !syncing, let api = client() else { return }
        syncing = true
        defer {
            syncing = false
            progress = nil
        }
        do {
            recordDeviceStat(force: reason == "manual")
            progress = "Uploading locations"
            try await outbox.flush(api)
            pending = await outbox.pending
            if Settings.healthEnabled && HealthSync.available {
                try await health.syncAll(api: api) { n in
                    Task { @MainActor in SyncEngine.shared.progress = "Health: \(n) new samples" }
                }
            }
            status = try await api.status()
            lastSync = Date()
            Settings.lastSync = lastSync
            lastError = nil
        } catch APIError.unauthorized {
            lastError = APIError.unauthorized.errorDescription
            forgetDevice()
        } catch is CancellationError {
            // background time ran out; continues next time
        } catch {
            lastError = error.localizedDescription
        }
        pending = await outbox.pending
    }

    /// HealthKit observer fired (possibly in the background).
    private func healthChanged(_ type: HKSampleType) async {
        guard let api = client() else { return }
        do {
            try await health.sync(type, api: api)
        } catch APIError.unauthorized {
            forgetDevice()
        } catch {
            // retried on the next wake / foreground
        }
    }

    private func record(_ points: [LocationDTO]) {
        Task {
            await outbox.add(locations: points)
            recordDeviceStat(force: false) // piggyback on the location wake
            pending = await outbox.pending
            guard await outbox.isDue(), let api = client() else { return }
            // A location wake gives only a few seconds; ask for a bit more.
            let id = UIApplication.shared.beginBackgroundTask(withName: "upload")
            do {
                try await outbox.flush(api)
            } catch APIError.unauthorized {
                forgetDevice()
            } catch {}
            pending = await outbox.pending
            UIApplication.shared.endBackgroundTask(id)
        }
    }

    private func recordDeviceStat(force: Bool) {
        guard Settings.deviceStatsEnabled else { return }
        if !force, let last = Settings.lastDeviceStat, Date().timeIntervalSince(last) < 10 * 60 { return }
        Settings.lastDeviceStat = Date()
        let stat = DeviceStats.snapshot()
        Task { await outbox.add(stat: stat) }
    }

    // MARK: Background tasks

    private func registerBackgroundTasks() {
        for id in [Self.refreshTask, Self.backfillTask] {
            BGTaskScheduler.shared.register(forTaskWithIdentifier: id, using: nil) { task in
                let work = Task { @MainActor in
                    SyncEngine.shared.scheduleBackgroundTasks()
                    await SyncEngine.shared.sync(reason: "background")
                }
                task.expirationHandler = { work.cancel() }
                Task {
                    await work.value
                    task.setTaskCompleted(success: !work.isCancelled)
                }
            }
        }
    }

    func scheduleBackgroundTasks() {
        guard signedIn else { return }
        // Short, roughly every 30+ min (iOS decides based on usage).
        let refresh = BGAppRefreshTaskRequest(identifier: Self.refreshTask)
        refresh.earliestBeginDate = Date(timeIntervalSinceNow: 30 * 60)
        try? BGTaskScheduler.shared.submit(refresh)
        // Long, while charging: finishes the initial Health backfill.
        let backfill = BGProcessingTaskRequest(identifier: Self.backfillTask)
        backfill.requiresNetworkConnectivity = true
        backfill.requiresExternalPower = true
        try? BGTaskScheduler.shared.submit(backfill)
    }
}
