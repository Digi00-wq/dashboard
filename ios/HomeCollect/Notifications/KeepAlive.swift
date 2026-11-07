// Background keep-alive via coarse location updates (see KeepAlive).

import CoreLocation
import Foundation

/// "Stay connected in background": keeps the app running in the background
/// so the notification stream stays open (no server push without a paid
/// Apple developer account).
///
/// iOS keeps an app with running location updates alive. This uses the
/// coarsest accuracy (cell / wifi, no GPS) and a huge distance filter, so the
/// radio cost is low and nothing is recorded; the location history has its
/// own manager (LocationTracker). Needs location access "Always".
@MainActor
final class KeepAlive: NSObject, CLLocationManagerDelegate {
    static let shared = KeepAlive()

    private let manager = CLLocationManager()
    private(set) var running = false

    private override init() {
        super.init()
        manager.delegate = self
        manager.desiredAccuracy = kCLLocationAccuracyThreeKilometers
        manager.distanceFilter = CLLocationDistanceMax
        manager.pausesLocationUpdatesAutomatically = false // never let iOS pause (= suspend) us
        manager.activityType = .other
    }

    /// Applies the setting (call on launch, sign-in/out and when it changes).
    func apply() {
        let want = Settings.stayConnected && SyncEngine.shared.signedIn
        if want {
            if manager.authorizationStatus == .notDetermined || manager.authorizationStatus == .authorizedWhenInUse {
                manager.requestAlwaysAuthorization()
            }
            guard manager.authorizationStatus == .authorizedAlways, !running else { return }
            manager.allowsBackgroundLocationUpdates = true
            manager.showsBackgroundLocationIndicator = false
            manager.startUpdatingLocation()
            running = true
        } else if running {
            manager.stopUpdatingLocation()
            manager.allowsBackgroundLocationUpdates = false
            running = false
        }
    }

    // MARK: CLLocationManagerDelegate

    nonisolated func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        Task { @MainActor in KeepAlive.shared.apply() }
    }

    nonisolated func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        // Nothing is recorded; a rare update is a good moment to catch up.
        Task { @MainActor in await AppNotifier.shared.catchUp() }
    }

    nonisolated func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {}
}
