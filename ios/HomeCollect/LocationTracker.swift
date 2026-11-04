import CoreLocation
import Foundation

/// Location history.
///  * Significant changes + visits: cheap on battery, works while the app is
///    closed (iOS relaunches it in the background).
///  * Detailed mode: continuous GPS every ~100 m, for proper trip lines.
/// Created and used on the main thread (delegate callbacks arrive there).
final class LocationTracker: NSObject, CLLocationManagerDelegate {
    private let manager = CLLocationManager()
    var onRecord: (([LocationDTO]) -> Void)?
    var onAuthorizationChange: ((CLAuthorizationStatus) -> Void)?

    var authorization: CLAuthorizationStatus { manager.authorizationStatus }

    override init() {
        super.init()
        manager.delegate = self
        manager.pausesLocationUpdatesAutomatically = true
        manager.activityType = .other
    }

    func start(detailed: Bool) {
        switch manager.authorizationStatus {
        case .notDetermined, .authorizedWhenInUse:
            manager.requestAlwaysAuthorization()
        default:
            break
        }
        manager.startMonitoringSignificantLocationChanges()
        manager.startMonitoringVisits()
        if detailed {
            manager.desiredAccuracy = kCLLocationAccuracyHundredMeters
            manager.distanceFilter = 100
            manager.allowsBackgroundLocationUpdates = true
            manager.showsBackgroundLocationIndicator = false
            manager.startUpdatingLocation()
        } else {
            manager.stopUpdatingLocation()
            manager.allowsBackgroundLocationUpdates = false
        }
    }

    func stop() {
        manager.stopUpdatingLocation()
        manager.stopMonitoringSignificantLocationChanges()
        manager.stopMonitoringVisits()
        manager.allowsBackgroundLocationUpdates = false
    }

    // MARK: CLLocationManagerDelegate

    func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        let points = locations
            .filter { $0.horizontalAccuracy >= 0 && $0.horizontalAccuracy <= 1000 }
            .map { loc in
                LocationDTO(
                    t: ms(loc.timestamp),
                    lat: loc.coordinate.latitude,
                    lon: loc.coordinate.longitude,
                    accuracy: loc.horizontalAccuracy,
                    altitude: loc.verticalAccuracy >= 0 ? loc.altitude : nil,
                    speed: loc.speed >= 0 ? loc.speed : nil,
                    course: loc.course >= 0 ? loc.course : nil)
            }
        if !points.isEmpty { onRecord?(points) }
    }

    func locationManager(_ manager: CLLocationManager, didVisit visit: CLVisit) {
        let arrive = visit.arrivalDate == .distantPast ? nil : ms(visit.arrivalDate)
        let depart = visit.departureDate == .distantFuture ? nil : ms(visit.departureDate)
        guard let t = arrive ?? depart else { return }
        onRecord?([LocationDTO(
            t: t, kind: "visit",
            lat: visit.coordinate.latitude, lon: visit.coordinate.longitude,
            accuracy: visit.horizontalAccuracy >= 0 ? visit.horizontalAccuracy : nil,
            arrive: arrive, depart: depart)])
    }

    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        onAuthorizationChange?(manager.authorizationStatus)
    }

    func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        // Transient (no fix yet); iOS keeps trying.
    }
}
