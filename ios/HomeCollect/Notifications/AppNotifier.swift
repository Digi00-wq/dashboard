import Foundation
import UIKit
import UserNotifications

/// A notification from the server (GET /api/notifications, live stream).
struct ServerNotification: Codable, Hashable {
    struct Action: Codable, Hashable {
        let action: String
        let title: String?
    }

    let id: Int64
    let t: Double?       // Unix ms
    let title: String
    let message: String
    let tag: String?     // same tag replaces the previous one ("dash-lights")
    let url: String?     // tap opens it (homecollect://briefing)
    let actions: [Action]?
    let timeSensitive: Bool?

    var date: Date? { t.map { Date(timeIntervalSince1970: $0 / 1000) } }
}

private struct NotificationsResponse: Decodable {
    let notifications: [ServerNotification]
    let latest: Int64
}

extension APIClient {
    fileprivate func notifications(after: Int64) async throws -> NotificationsResponse {
        try await getJSON("api/notifications", ["after": String(after)])
    }

    /// Test notification to yourself, or (admins) to `user`.
    func testNotification(to user: String? = nil, title: String = "", message: String = "") async throws {
        var body: [String: String] = ["title": title, "message": message]
        if let user { body["user"] = user }
        _ = try await send("api/notifications/test", method: "POST",
                           body: try JSONSerialization.data(withJSONObject: body))
    }

    fileprivate func notificationAction(_ action: String) async throws {
        let body = try JSONSerialization.data(withJSONObject: ["action": action])
        _ = try await send("api/notifications/action", method: "POST", body: body)
    }
}

/// Shows the server's notifications as this app's own notifications.
///
/// iOS gives a free-account app no server push, so delivery has three paths:
///  * a live stream (GET /api/notifications/stream) while the app runs -
///    in the background too, as long as `KeepAlive` keeps it awake;
///  * catch-up (GET /api/notifications?after=) whenever iOS wakes the app
///    (foreground, location, HealthKit, background refresh);
///  * duplicates are dropped by id, so all paths can run at once.
@MainActor
final class AppNotifier: NSObject, ObservableObject {
    static let shared = AppNotifier()

    /// Category for notifications with the "Lights off" button.
    nonisolated static let lightsOffAction = "DASH_LIGHTS_OFF"
    nonisolated private static let lightsCategory = "dash-lights-off"
    /// Older notifications found while catching up are skipped silently.
    private static let maxAge: TimeInterval = 6 * 3600

    @Published private(set) var authorization: UNAuthorizationStatus = .notDetermined
    @Published private(set) var streaming = false

    private var task: Task<Void, Never>?
    private var catchingUp = false

    private var lastSeen: Int64 {
        get { Int64(UserDefaults.standard.integer(forKey: "notifyLastSeen")) }
        set { UserDefaults.standard.set(Int(newValue), forKey: "notifyLastSeen") }
    }
    /// False until the first catch-up after sign-in: old ones are not shown.
    private var primed: Bool {
        get { UserDefaults.standard.bool(forKey: "notifyPrimed") }
        set { UserDefaults.standard.set(newValue, forKey: "notifyPrimed") }
    }

    // MARK: Setup

    /// From didFinishLaunching: the delegate must be set before launch ends
    /// so button taps that launched the app are delivered.
    func setup() {
        let center = UNUserNotificationCenter.current()
        center.delegate = self
        let lightsOff = UNNotificationAction(identifier: Self.lightsOffAction,
                                             title: String(localized: "Lights off"),
                                             options: [])
        center.setNotificationCategories([
            UNNotificationCategory(identifier: Self.lightsCategory, actions: [lightsOff], intentIdentifiers: []),
        ])
        Task { await refreshAuthorization() }
    }

    func requestPermission() async {
        _ = try? await UNUserNotificationCenter.current()
            .requestAuthorization(options: [.alert, .sound, .badge])
        await refreshAuthorization()
    }

    func refreshAuthorization() async {
        authorization = await UNUserNotificationCenter.current().notificationSettings().authorizationStatus
    }

    /// Settings button: the server sends one to this account.
    func sendTest() async -> String? {
        guard let api = SyncEngine.shared.apiClient else { return nil }
        do {
            try await api.testNotification()
            if !streaming { await catchUp() } // not live: fetch it right away
            return nil
        } catch {
            return error.localizedDescription
        }
    }

    // MARK: Live stream

    func start() {
        guard task == nil, SyncEngine.shared.signedIn else { return }
        Task {
            if authorization == .notDetermined { await requestPermission() }
        }
        task = Task { await run() }
    }

    func stop() {
        task?.cancel()
        task = nil
        streaming = false
    }

    /// Signed out: stop and forget what was seen.
    func reset() {
        stop()
        lastSeen = 0
        primed = false
        UNUserNotificationCenter.current().removeAllDeliveredNotifications()
    }

    /// Back in the foreground: reconnect at once if the stream died while
    /// the app was suspended.
    func ensureRunning() {
        if task == nil || !streaming {
            stop()
            start()
        }
    }

    private func run() async {
        var delay: Double = 1
        while !Task.isCancelled {
            guard let api = SyncEngine.shared.apiClient else { break }
            await catchUp() // anything missed while not connected
            do {
                try await stream(api)
                delay = 1 // server closed it (session change): reconnect quickly
            } catch is CancellationError {
                break
            } catch APIError.unauthorized {
                break
            } catch {
                if (error as? URLError)?.code == .cancelled { break }
            }
            streaming = false
            try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
            delay = min(delay * 2, 60)
        }
        streaming = false
    }

    private static let session: URLSession = {
        let c = URLSessionConfiguration.default
        // Heartbeat every 25 s: 40 s of silence = dead connection.
        c.timeoutIntervalForRequest = 40
        c.timeoutIntervalForResource = 7 * 24 * 3600
        return URLSession(configuration: c)
    }()

    private func stream(_ api: APIClient) async throws {
        var req = URLRequest(url: api.base.appendingPathComponent("api/notifications/stream"))
        req.setValue("text/event-stream", forHTTPHeaderField: "Accept")
        if let token = api.token { req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization") }
        let (bytes, resp) = try await Self.session.bytes(for: req)
        switch (resp as? HTTPURLResponse)?.statusCode ?? 0 {
        case 200: break
        case 401: throw APIError.unauthorized
        case let code: throw APIError.server(code, "stream")
        }
        streaming = true
        var event = ""
        let decoder = JSONDecoder()
        for try await line in bytes.lines {
            if line.hasPrefix("event: ") {
                event = String(line.dropFirst(7))
            } else if line.hasPrefix("data: ") {
                if event == "notification",
                   let n = try? decoder.decode(ServerNotification.self, from: Data(line.dropFirst(6).utf8)) {
                    await deliver([n])
                }
                event = ""
            }
        }
    }

    // MARK: Catch-up

    /// Fetches and shows everything newer than the last one seen. Called on
    /// every wake-up (foreground, location, HealthKit, background refresh).
    func catchUp() async {
        guard !catchingUp, SyncEngine.shared.signedIn, let api = SyncEngine.shared.apiClient else { return }
        catchingUp = true
        defer { catchingUp = false }
        do {
            let r = try await api.notifications(after: lastSeen)
            if !primed {
                // First run after sign-in: don't replay the history.
                lastSeen = max(lastSeen, r.latest)
                primed = true
                return
            }
            await deliver(r.notifications)
        } catch {}
    }

    private func deliver(_ list: [ServerNotification]) async {
        for n in list.sorted(by: { $0.id < $1.id }) where n.id > lastSeen {
            lastSeen = n.id
            if let d = n.date, Date().timeIntervalSince(d) > Self.maxAge { continue }
            await show(n)
        }
    }

    private func show(_ n: ServerNotification) async {
        let content = UNMutableNotificationContent()
        content.title = n.title
        content.body = n.message
        content.sound = .default
        content.threadIdentifier = "dashboard"
        if n.timeSensitive == true { content.interruptionLevel = .timeSensitive }
        var info: [String: Any] = ["id": n.id]
        if let url = n.url { info["url"] = url }
        content.userInfo = info
        if n.actions?.contains(where: { $0.action == Self.lightsOffAction }) == true {
            content.categoryIdentifier = Self.lightsCategory
        }
        // Same tag replaces the earlier notification, like the HA app does.
        let id = n.tag ?? "dash-\(n.id)"
        try? await UNUserNotificationCenter.current()
            .add(UNNotificationRequest(identifier: id, content: content, trigger: nil))
    }
}

// MARK: - UNUserNotificationCenterDelegate

extension AppNotifier: UNUserNotificationCenterDelegate {
    /// App in the foreground: show the banner anyway.
    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter,
                                            willPresent notification: UNNotification) async
        -> UNNotificationPresentationOptions {
        [.banner, .list, .sound]
    }

    /// Tap or button.
    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter,
                                            didReceive response: UNNotificationResponse) async {
        let action = response.actionIdentifier
        let url = response.notification.request.content.userInfo["url"] as? String
        await MainActor.run {
            AppNotifier.shared.handle(action: action, url: url)
        }
        if action == AppNotifier.lightsOffAction {
            // Runs in the background without opening the app; iOS waits
            // for this method to return.
            guard let api = await MainActor.run(body: { SyncEngine.shared.apiClient }) else { return }
            try? await api.notificationAction(action)
        }
    }

    private func handle(action: String, url: String?) {
        guard action == UNNotificationDefaultActionIdentifier,
              let url, let u = URL(string: url) else { return }
        AppRouter.shared.handle(u)
    }
}
