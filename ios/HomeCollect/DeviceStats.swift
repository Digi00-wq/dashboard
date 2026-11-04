import Foundation
import Network
import UIKit

/// Battery, storage and network snapshot. (iOS gives apps no access to
/// Screen Time / per-app usage, so that can't be collected.)
@MainActor
enum DeviceStats {
    private static let monitor: NWPathMonitor = {
        let m = NWPathMonitor()
        m.start(queue: DispatchQueue(label: "homecollect.network"))
        return m
    }()

    /// Call at launch so `currentPath` is known by the first snapshot.
    static func start() { _ = monitor }

    static func snapshot() -> DeviceStatDTO {
        let device = UIDevice.current
        device.isBatteryMonitoringEnabled = true
        let level = device.batteryLevel

        let state: String
        switch device.batteryState {
        case .charging: state = "charging"
        case .full: state = "full"
        case .unplugged: state = "unplugged"
        default: state = "unknown"
        }

        let values = try? URL(fileURLWithPath: NSHomeDirectory()).resourceValues(
            forKeys: [.volumeAvailableCapacityForImportantUsageKey, .volumeTotalCapacityKey])

        let path = monitor.currentPath
        let network: String
        if path.status != .satisfied { network = "offline" }
        else if path.usesInterfaceType(.wifi) { network = "wifi" }
        else if path.usesInterfaceType(.cellular) { network = "cellular" }
        else if path.usesInterfaceType(.wiredEthernet) { network = "ethernet" }
        else { network = "other" }

        return DeviceStatDTO(
            t: ms(Date()),
            device: modelIdentifier(),
            battery: level >= 0 ? Double(level) : nil,
            batteryState: state,
            lowPower: ProcessInfo.processInfo.isLowPowerModeEnabled,
            storageFree: values?.volumeAvailableCapacityForImportantUsage,
            storageTotal: values?.volumeTotalCapacity.map(Int64.init),
            network: network)
    }

    /// e.g. "iPhone16,1"
    static func modelIdentifier() -> String {
        var info = utsname()
        uname(&info)
        let id = withUnsafeBytes(of: &info.machine) { raw in
            String(decoding: raw.prefix(while: { $0 != 0 }), as: UTF8.self)
        }
        return clean(id, maxBytes: 64)
    }
}
