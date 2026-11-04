import CoreLocation
import UIKit

/// Tells the server when this iPhone arrives home or leaves (for the home
/// automations: lights on/off, pause music). Monitors a circular region
/// around the home zone from Home Assistant (GET /api/home). Region events
/// also arrive while the app is closed: iOS relaunches it in the background,
/// SyncEngine.launch() starts the monitor again and the event is posted
/// right away (POST /api/presence) inside a background task.
///
/// Needs "Always" location authorization (requested by LocationTracker).
/// Driven by SyncEngine: start() while signed in with "collect" and
/// location enabled, stop() otherwise.
@MainActor
final class PresenceMonitor {
    static let regionID = "home"

    private static let zoneKey = "presence.homeZone"
    private static let fetchedKey = "presence.homeZoneFetchedAt"
    private static let pendingKey = "presence.pendingEvent"
    private static let refetchInterval: TimeInterval = 24 * 3600
    /// An event that couldn't be sent is dropped after this long: switching
    /// lights for an arrival from an hour ago would be wrong.
    private static let maxEventAge: Int64 = 15 * 60 * 1000

    private struct PendingEvent: Codable, Equatable {
        let event: String // "arrive" | "leave"
        let t: Int64
    }

    private let tracker: LocationTracker
    private var active = false
    private var refreshing = false

    init(tracker: LocationTracker) {
        self.tracker = tracker
    }

    /// Idempotent: (re)applies the cached zone, refreshes it at most once a
    /// day and re-sends an event that failed earlier.
    func start() {
        active = true
        if let zone = Self.cachedZone { apply(zone) }
        Task {
            await refreshZone()
            await flushPending()
        }
    }

    /// Stops monitoring; `forget` also drops the cached zone and any
    /// unsent event (sign-out, account change).
    func stop(forget: Bool = false) {
        active = false
        tracker.stopMonitoring(identifier: Self.regionID)
        if forget {
            let d = UserDefaults.standard
            d.removeObject(forKey: Self.zoneKey)
            d.removeObject(forKey: Self.fetchedKey)
            d.removeObject(forKey: Self.pendingKey)
        }
    }

    /// The home zone currently used (cached from the server).
    var homeZone: HomeZone? { Self.cachedZone }

    // MARK: Events

    /// Called by SyncEngine for enter / exit of the "home" region.
    func report(entered: Bool) {
        let event = PendingEvent(event: entered ? "arrive" : "leave", t: ms(Date()))
        Self.pending = event
        // Relaunched in the background for this event: only a few seconds
        // unless we ask for more.
        let background = ExtendedRun(name: "presence")
        Task {
            await send(event)
            background.end()
        }
    }

    private func flushPending() async {
        guard let event = Self.pending else { return }
        if ms(Date()) - event.t > Self.maxEventAge {
            Self.pending = nil
            return
        }
        await send(event)
    }

    private func send(_ event: PendingEvent) async {
        for attempt in 0..<3 {
            guard let api = SyncEngine.shared.apiClient else { return }
            do {
                try await api.sendPresence(event: event.event, t: event.t)
                if Self.pending == event { Self.pending = nil }
                return
            } catch APIError.unauthorized {
                SyncEngine.shared.deviceRevoked()
                return
            } catch APIError.forbidden {
                if Self.pending == event { Self.pending = nil }
                return
            } catch {
                // Network flakiness right at the door is common: retry a bit.
                if attempt < 2 { try? await Task.sleep(nanoseconds: 3_000_000_000) }
            }
        }
    }

    // MARK: Home zone

    /// Fetches the home zone if the last fetch is older than a day.
    func refreshZone(force: Bool = false) async {
        guard active, !refreshing, let api = SyncEngine.shared.apiClient else { return }
        let d = UserDefaults.standard
        if !force, let last = d.object(forKey: Self.fetchedKey) as? Date,
           Date().timeIntervalSince(last) < Self.refetchInterval {
            return
        }
        refreshing = true
        defer { refreshing = false }
        do {
            let zone = try await api.homeZone()
            d.set(Date(), forKey: Self.fetchedKey)
            if let zone, zone.lat != nil, zone.lon != nil {
                Self.cachedZone = zone
                if active { apply(zone) }
            } else {
                Self.cachedZone = nil
                tracker.stopMonitoring(identifier: Self.regionID)
            }
        } catch APIError.unauthorized {
            SyncEngine.shared.deviceRevoked()
        } catch APIError.forbidden {
            stop(forget: true)
        } catch {
            // Keep the cached zone; try again next time.
        }
    }

    private func apply(_ zone: HomeZone) {
        guard active, let lat = zone.lat, let lon = zone.lon,
              CLLocationManager.isMonitoringAvailable(for: CLCircularRegion.self) else { return }
        let center = CLLocationCoordinate2D(latitude: lat, longitude: lon)
        guard CLLocationCoordinate2DIsValid(center) else { return }
        var radius = max(zone.radius ?? 0, 100)
        let maxRadius = tracker.maximumRegionMonitoringDistance
        if maxRadius > 0 { radius = min(radius, maxRadius) }

        // Re-registering resets the region state; skip if unchanged.
        if let current = tracker.monitoredRegion(identifier: Self.regionID) as? CLCircularRegion,
           abs(current.center.latitude - lat) < 0.000001,
           abs(current.center.longitude - lon) < 0.000001,
           abs(current.radius - radius) < 1 {
            return
        }
        let region = CLCircularRegion(center: center, radius: radius, identifier: Self.regionID)
        region.notifyOnEntry = true
        region.notifyOnExit = true
        tracker.startMonitoring(region: region)
    }

    // MARK: Storage

    private static var cachedZone: HomeZone? {
        get {
            guard let data = UserDefaults.standard.data(forKey: zoneKey) else { return nil }
            return try? JSONDecoder().decode(HomeZone.self, from: data)
        }
        set {
            if let newValue, let data = try? JSONEncoder().encode(newValue) {
                UserDefaults.standard.set(data, forKey: zoneKey)
            } else {
                UserDefaults.standard.removeObject(forKey: zoneKey)
            }
        }
    }

    private static var pending: PendingEvent? {
        get {
            guard let data = UserDefaults.standard.data(forKey: pendingKey) else { return nil }
            return try? JSONDecoder().decode(PendingEvent.self, from: data)
        }
        set {
            if let newValue, let data = try? JSONEncoder().encode(newValue) {
                UserDefaults.standard.set(data, forKey: pendingKey)
            } else {
                UserDefaults.standard.removeObject(forKey: pendingKey)
            }
        }
    }
}

/// UIApplication background task that is always ended, also on expiry.
@MainActor
private final class ExtendedRun {
    private var id: UIBackgroundTaskIdentifier = .invalid

    init(name: String) {
        id = UIApplication.shared.beginBackgroundTask(withName: name) { [weak self] in
            MainActor.assumeIsolated { self?.end() }
        }
    }

    func end() {
        guard id != .invalid else { return }
        UIApplication.shared.endBackgroundTask(id)
        id = .invalid
    }
}
