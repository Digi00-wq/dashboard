// Reloading the widget / Control Center controls from the app.

import Foundation
import WidgetKit

/// Kind identifiers of the widgets.
enum WidgetKinds {
    static let lights = "homecollect.lights"
}

/// Asks the system to re-read the Control Center controls (cheap; the
/// system throttles). Call after sign-in/out and after the app changed lights.
enum WidgetRefresher {
    static func reload() {
        if #available(iOS 18.0, *) {
            ControlCenter.shared.reloadControls(ofKind: WidgetKinds.lights)
        }
    }
}
