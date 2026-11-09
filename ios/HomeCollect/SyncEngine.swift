// SyncEngine (singleton): sign-in/out, permissions of the account, starting the collectors
// (HealthSync, LocationTracker, DeviceStats, PresenceMonitor), uploads via Outbox and
// BGTaskScheduler background refresh.

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
    /// Server permissions of this account ("lights", "collect", ...).
    @Published private(set) var permissions = Settings.permissions
    var canCollect: Bool { permissions.contains("collect") }
    /// Admins see other users' history (People) and the Claude tab.
    @Published private(set) var isAdmin = Settings.isAdmin
    /// How the dashboard talks to you (set by an admin).
    @Published private(set) var displayName = Settings.displayName

    let health = HealthSync()
    let location = LocationTracker()
    let outbox = Outbox()
    /// Home zone enter/exit -> POST /api/presence (home automations).
    private(set) lazy var presence = PresenceMonitor(tracker: location)

    private init() {
        location.onRecord = { [weak self] points in self?.record(points) }
        location.onAuthorizationChange = { [weak self] s in self?.locationAuth = s }
        location.onRegionEvent = { [weak self] id, entered in self?.regionEvent(id, entered: entered) }
        locationAuth = location.authorization
    }

    var serverHost: String { Settings.serverURL?.host ?? "" }
    var user: String { Settings.user ?? "" }

    var apiClient: APIClient? { client() }

    /// APIClient for the saved server + token (nil when signed out).
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
        if canCollect { startCollectors(askPermissions: false) }
    }

    /// Re-reads the permissions (the admin may have changed them).
    func refreshAccount() async {
        guard let api = client() else { return }
        do {
            let me = try await api.me()
            let couldCollect = canCollect
            permissions = me.permissions
            Settings.permissions = me.permissions
            isAdmin = me.admin
            Settings.isAdmin = me.admin
            displayName = me.displayName
            Settings.displayName = me.displayName
            if canCollect != couldCollect {
                if canCollect { startCollectors(askPermissions: true) } else { stopCollectors() }
            } else if canCollect && Settings.locationEnabled {
                presence.start() // daily home zone refresh, re-send a failed event
            }
        } catch APIError.unauthorized {
            deviceRevoked()
        } catch {}
    }

    private func stopCollectors() {
        location.stop()
        presence.stop()
        Task { await health.stopObserving() }
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
            presence.start()
        } else {
            location.stop()
            presence.stop()
        }
    }

    /// Home / work region entered or left (also on a background relaunch by iOS).
    private func regionEvent(_ id: String, entered: Bool) {
        guard id == PresenceMonitor.regionID || id == PresenceMonitor.workRegionID else { return }
        guard signedIn, canCollect, Settings.locationEnabled else {
            presence.stop()
            return
        }
        presence.report(region: id, entered: entered)
    }

    /// Re-applies the toggles after the user changed one.
    func settingsChanged() {
        guard signedIn, canCollect else { return }
        startCollectors(askPermissions: true)
    }

    // MARK: Account

    /// Logs in, stores the device key, loads /api/me and starts collecting.
    func signIn(server: String, user: String, password: String) async throws {
        guard let url = APIClient.normalize(server) else { throw APIError.invalidURL }
        let response = try await APIClient(base: url, token: nil)
            .login(username: user, password: password, device: UIDevice.current.name)
        Settings.serverURL = url
        Settings.token = response.token
        Settings.user = response.user
        Settings.resetHealthAnchors() // new account/server: send the full history
        presence.stop(forget: true)   // home zone of the previous account
        let me = try? await APIClient(base: url, token: response.token).me()
        permissions = me?.permissions ?? []
        Settings.permissions = permissions
        isAdmin = me?.admin ?? false
        Settings.isAdmin = isAdmin
        displayName = me?.displayName
        Settings.displayName = displayName
        signedIn = true
        lastError = nil
        LiveStore.shared.start()
        AppNotifier.shared.start()
        KeepAlive.shared.apply()
        if canCollect {
            startCollectors(askPermissions: true)
            await sync(reason: "sign-in")
        }
    }

    /// Logs out on the server and wipes local credentials and state.
    func signOut() async {
        try? await client()?.logout()
        deviceRevoked()
    }

    /// Token gone (signed out here, or revoked on the server).
    func deviceRevoked() {
        guard signedIn else { return }
        Settings.token = nil
        signedIn = false
        status = nil
        LiveStore.shared.reset()
        HistoryStore.clearDiskCache()
        isAdmin = false
        Settings.isAdmin = false
        ClaudeStore.shared.reset()
        AppNotifier.shared.reset()
        KeepAlive.shared.apply() // signed out -> stops
        location.stop()
        presence.stop(forget: true)
        Task {
            await health.stopObserving()
            await outbox.clear()
            pending = 0
        }
    }

    // MARK: Sync

    /// One upload round: HealthKit changes, outbox (locations, stats). `reason` is for logging.
    func sync(reason: String) async {
        // Every wake-up is a chance to show missed notifications.
        await AppNotifier.shared.catchUp()
        guard signedIn, canCollect, !syncing, let api = client() else { return }
        syncing = true
        defer {
            syncing = false
            progress = nil
        }
        do {
            recordDeviceStat(force: reason == "manual")
            progress = String(localized: "Uploading locations")
            try await outbox.flush(api)
            pending = await outbox.pending
            if Settings.healthEnabled && HealthSync.available {
                try await health.syncAll(api: api) { n in
                    Task { @MainActor in SyncEngine.shared.progress = String(localized: "Health: \(n) new samples") }
                }
            }
            status = try await api.status()
            lastSync = Date()
            Settings.lastSync = lastSync
            lastError = nil
        } catch APIError.unauthorized {
            lastError = APIError.unauthorized.errorDescription
            deviceRevoked()
        } catch is CancellationError {
            // background time ran out; continues next time
        } catch {
            lastError = error.localizedDescription
        }
        pending = await outbox.pending
    }

    /// HealthKit observer fired (possibly in the background).
    private func healthChanged(_ type: HKSampleType) async {
        await AppNotifier.shared.catchUp()
        guard let api = client() else { return }
        do {
            try await health.sync(type, api: api)
        } catch APIError.unauthorized {
            deviceRevoked()
        } catch {
            // retried on the next wake / foreground
        }
    }

    private func record(_ points: [LocationDTO]) {
        Task {
            await AppNotifier.shared.catchUp() // location wake: missed notifications
            await outbox.add(locations: points)
            recordDeviceStat(force: false) // piggyback on the location wake
            pending = await outbox.pending
            guard await outbox.isDue(), let api = client() else { return }
            // A location wake gives only a few seconds; ask for a bit more.
            let id = UIApplication.shared.beginBackgroundTask(withName: "upload")
            do {
                try await outbox.flush(api)
            } catch APIError.unauthorized {
                deviceRevoked()
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

    /// BGTaskScheduler handlers for periodic background sync.
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

    /// Requests the next background run.
    func scheduleBackgroundTasks() {
        guard signedIn else { return }
        // Short, roughly every 30+ min (iOS decides based on usage).
        let refresh = BGAppRefreshTaskRequest(identifier: Self.refreshTask)
        refresh.earliestBeginDate = Date(timeIntervalSinceNow: 15 * 60) // also catches up on notifications
        try? BGTaskScheduler.shared.submit(refresh)
        // Long, while charging: finishes the initial Health backfill.
        let backfill = BGProcessingTaskRequest(identifier: Self.backfillTask)
        backfill.requiresNetworkConnectivity = true
        backfill.requiresExternalPower = true
        try? BGTaskScheduler.shared.submit(backfill)
    }
}
