// Face ID / passcode app lock (AppLock), the gate view that hides content while locked,
// and the lock screen.

import LocalAuthentication
import SwiftUI

/// Optional Face ID / Touch ID / passcode lock. Turned on in Settings
/// (`@AppStorage(AppLock.enabledKey)`). The app locks when it goes to the
/// background and asks to unlock when it comes back.
@MainActor
final class AppLock: ObservableObject {
    static let shared = AppLock()
    static let enabledKey = "appLockEnabled"

    @Published var locked: Bool
    @Published private(set) var authenticating = false
    @Published private(set) var errorMessage: String?

    /// Prompt automatically on the next `.active`. Not after a cancelled
    /// prompt: the prompt itself makes the app inactive -> active, which
    /// would otherwise ask again and again.
    private var promptOnActive: Bool

    var isEnabled: Bool { UserDefaults.standard.bool(forKey: Self.enabledKey) }

    private init() {
        let enabled = UserDefaults.standard.bool(forKey: Self.enabledKey)
        locked = enabled // cold start
        promptOnActive = enabled
    }

    func lockIfNeeded() {
        guard isEnabled else { return }
        locked = true
        promptOnActive = true
        errorMessage = nil
    }

    /// Call when the scene becomes active.
    func sceneBecameActive() {
        guard locked, promptOnActive else { return }
        promptOnActive = false
        Task { await unlock() }
    }

    func unlock() async {
        guard locked, !authenticating else { return }
        guard isEnabled else {
            locked = false
            return
        }
        let context = LAContext()
        var policyError: NSError?
        guard context.canEvaluatePolicy(.deviceOwnerAuthentication, error: &policyError) else {
            // No passcode set on this iPhone: nothing to check against.
            locked = false
            return
        }
        authenticating = true
        defer { authenticating = false }
        do {
            if try await context.evaluatePolicy(.deviceOwnerAuthentication,
                                                localizedReason: String(localized: "Unlock HomeCollect")) {
                locked = false
                errorMessage = nil
            }
        } catch let error as LAError {
            switch error.code {
            case .userCancel, .appCancel, .systemCancel: errorMessage = nil
            default: errorMessage = error.localizedDescription
            }
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    /// SF Symbol for the unlock button.
    var unlockSymbol: String {
        let context = LAContext()
        _ = context.canEvaluatePolicy(.deviceOwnerAuthenticationWithBiometrics, error: nil)
        switch context.biometryType {
        case .faceID: return "faceid"
        case .touchID: return "touchid"
        default: return "lock.open.fill"
        }
    }
}

/// Puts the lock screen above the app while locked, and also while the app is
/// inactive / in the background (app switcher snapshot) when the lock is on.
struct AppLockGate<Content: View>: View {
    @ObservedObject private var lock = AppLock.shared
    @Environment(\.scenePhase) private var scenePhase
    private let content: Content

    init(@ViewBuilder content: () -> Content) {
        self.content = content()
    }

    var body: some View {
        ZStack {
            content
            if lock.locked || (lock.isEnabled && scenePhase != .active) {
                LockScreenView(lock: lock)
                    .zIndex(1)
            }
        }
        .onAppear {
            if scenePhase == .active { lock.sceneBecameActive() }
        }
    }
}

struct LockScreenView: View {
    @ObservedObject var lock: AppLock

    var body: some View {
        ZStack {
            Color(uiColor: .systemBackground).ignoresSafeArea()
            VStack(spacing: 16) {
                Image(systemName: "lock.fill")
                    .font(.system(size: 52, weight: .semibold))
                    .foregroundStyle(.tint)
                Text("HomeCollect is locked")
                    .font(.title2.weight(.semibold))
                if let message = lock.errorMessage {
                    Text(message)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                        .padding(.horizontal, 32)
                }
                if lock.locked {
                    Button {
                        Task { await lock.unlock() }
                    } label: {
                        Label("Unlock", systemImage: lock.unlockSymbol)
                            .padding(.horizontal, 12)
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.large)
                    .disabled(lock.authenticating)
                    .padding(.top, 8)
                }
            }
        }
    }
}
