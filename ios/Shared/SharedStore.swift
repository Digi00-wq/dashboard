import Foundation
import Security

/// Server address and device token, shared between the app and the widget
/// extension through the App Group (Info.plist key `AppGroupID`, set from the
/// APP_GROUP_ID build setting in project.yml).
///
/// - The token lives in the Keychain with the App Group as access group
///   (on iOS an App Group id is a valid keychain access group).
/// - The server address lives in the group's UserDefaults suite.
///
/// Compiled into both targets: keep it Foundation/Security only.
enum SharedStore {
    /// nil when the Info.plist key is missing or was not expanded.
    static let appGroupID: String? = {
        guard let id = Bundle.main.object(forInfoDictionaryKey: "AppGroupID") as? String,
              !id.isEmpty, !id.contains("$(") else { return nil }
        return id
    }()

    /// Group suite, or the standard defaults when there is no group.
    static let defaults: UserDefaults =
        appGroupID.flatMap { UserDefaults(suiteName: $0) } ?? .standard

    private static let serverKey = "shared.serverURL"

    static var serverURL: URL? {
        get { defaults.string(forKey: serverKey).flatMap { URL(string: $0) } }
        set {
            if let newValue { defaults.set(newValue.absoluteString, forKey: serverKey) }
            else { defaults.removeObject(forKey: serverKey) }
        }
    }

    // MARK: - Token (Keychain, shared access group)

    // Different service than the app's old item ("homecollect"), so queries
    // for one never match or delete the other.
    private static let service = "homecollect.shared"
    private static let account = "deviceToken"

    private static func query() -> [String: Any]? {
        guard let group = appGroupID else { return nil }
        return [kSecClass as String: kSecClassGenericPassword,
                kSecAttrService as String: service,
                kSecAttrAccount as String: account,
                kSecAttrAccessGroup as String: group]
    }

    static func readToken() -> String? {
        guard var q = query() else { return nil }
        q[kSecReturnData as String] = true
        q[kSecMatchLimit as String] = kSecMatchLimitOne
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess,
              let data = out as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    /// Returns false if the shared item could not be written (no App Group
    /// entitlement, e.g. when signing failed to add it); callers fall back.
    @discardableResult
    static func writeToken(_ value: String?) -> Bool {
        guard let q = query() else { return false }
        let deleted = SecItemDelete(q as CFDictionary)
        guard let value else { return deleted == errSecSuccess || deleted == errSecItemNotFound }
        var add = q
        add[kSecValueData as String] = Data(value.utf8)
        // Background syncs and Control Center run while the phone is locked.
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        return SecItemAdd(add as CFDictionary, nil) == errSecSuccess
    }
}
