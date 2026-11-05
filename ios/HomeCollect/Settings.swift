import Foundation
import Security

/// App settings. The device token lives in the Keychain; everything else in
/// UserDefaults. Both stay readable in the background after the first unlock.
///
/// Server address and token are shared with the widget extension through the
/// App Group (see Shared/SharedStore.swift). Values stored by older versions
/// (standard UserDefaults, app-only Keychain item) are moved over on first
/// read. If the App Group is not available (missing entitlement) the token
/// stays in the app-only Keychain item, so sign-in keeps working.
enum Settings {
    private static let d = UserDefaults.standard

    static var serverURL: URL? {
        get {
            if let url = SharedStore.serverURL { return url }
            guard let legacy = d.url(forKey: "serverURL") else { return nil }
            SharedStore.serverURL = legacy // migrate
            d.removeObject(forKey: "serverURL")
            return legacy
        }
        set {
            SharedStore.serverURL = newValue
            d.removeObject(forKey: "serverURL")
        }
    }

    private static let tokenKey = "deviceToken"
    private static var cachedToken: String?
    static var token: String? {
        get {
            if let cachedToken { return cachedToken }
            // Not cached while nil: before the first unlock after a reboot
            // the Keychain is unreadable, and a later read must still work.
            var t = SharedStore.readToken()
            if t == nil, let legacy = Keychain.read(tokenKey) {
                t = legacy
                if SharedStore.writeToken(legacy) { Keychain.write(tokenKey, nil) } // migrate
            }
            cachedToken = t
            return t
        }
        set {
            if let newValue, SharedStore.writeToken(newValue) {
                Keychain.write(tokenKey, nil)
            } else {
                if newValue == nil { SharedStore.writeToken(nil) }
                Keychain.write(tokenKey, newValue) // nil deletes; fallback without App Group
            }
            cachedToken = newValue
            WidgetRefresher.reload()
        }
    }

    static var user: String? {
        get { d.string(forKey: "user") }
        set { d.set(newValue, forKey: "user") }
    }

    static var permissions: [String] {
        get { d.stringArray(forKey: "permissions") ?? [] }
        set { d.set(newValue, forKey: "permissions") }
    }

    /// Admin account: People and Claude tabs.
    static var isAdmin: Bool {
        get { d.bool(forKey: "isAdmin") }
        set { d.set(newValue, forKey: "isAdmin") }
    }

    static var healthEnabled: Bool {
        get { d.object(forKey: "healthEnabled") as? Bool ?? true }
        set { d.set(newValue, forKey: "healthEnabled") }
    }

    static var locationEnabled: Bool {
        get { d.object(forKey: "locationEnabled") as? Bool ?? true }
        set { d.set(newValue, forKey: "locationEnabled") }
    }

    /// Continuous GPS (~100 m steps) instead of only significant changes.
    /// Much better trips, noticeably more battery.
    static var detailedLocation: Bool {
        get { d.bool(forKey: "detailedLocation") }
        set { d.set(newValue, forKey: "detailedLocation") }
    }

    static var deviceStatsEnabled: Bool {
        get { d.object(forKey: "deviceStatsEnabled") as? Bool ?? true }
        set { d.set(newValue, forKey: "deviceStatsEnabled") }
    }

    static var lastSync: Date? {
        get { d.object(forKey: "lastSync") as? Date }
        set { d.set(newValue, forKey: "lastSync") }
    }

    static var lastDeviceStat: Date? {
        get { d.object(forKey: "lastDeviceStat") as? Date }
        set { d.set(newValue, forKey: "lastDeviceStat") }
    }

    /// Forget all HealthKit anchors -> next sync re-sends everything
    /// (the server ignores duplicates).
    static func resetHealthAnchors() {
        for key in d.dictionaryRepresentation().keys where key.hasPrefix("anchor.") {
            d.removeObject(forKey: key)
        }
    }
}

/// App-only Keychain items (pre-App-Group token; fallback).
enum Keychain {
    private static let service = "homecollect"

    private static func query(_ key: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service,
         kSecAttrAccount as String: key]
    }

    static func read(_ key: String) -> String? {
        var q = query(key)
        q[kSecReturnData as String] = true
        q[kSecMatchLimit as String] = kSecMatchLimitOne
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess,
              let data = out as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    static func write(_ key: String, _ value: String?) {
        SecItemDelete(query(key) as CFDictionary)
        guard let value else { return }
        var add = query(key)
        add[kSecValueData as String] = Data(value.utf8)
        // Background syncs run while the phone is locked.
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        SecItemAdd(add as CFDictionary, nil)
    }
}
