// App entry. Creates the window (AppLockGate -> ContentView) and injects the shared stores:
// SyncEngine (sign-in, uploads), LiveStore (live dashboard), AppRouter (tabs, deep links), AppLock.
// AppDelegate starts background work (HealthKit observers, location, notifications) at launch.

import SwiftUI
import UIKit

@main
struct HomeCollectApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @Environment(\.scenePhase) private var scenePhase
    @StateObject private var engine = SyncEngine.shared

    var body: some Scene {
        WindowGroup {
            AppLockGate {
                ContentView()
            }
            .environmentObject(engine)
            .environmentObject(LiveStore.shared)
            .environmentObject(AppRouter.shared)
            .environmentObject(AppLock.shared)
            .onOpenURL { AppRouter.shared.handle($0) }
        }
        // initial: true -> also runs on a cold start, where iOS may not report
        // a *change* to .active (the stream would never start).
        .onChange(of: scenePhase, initial: true) { _, phase in
            switch phase {
            case .active:
                AppLock.shared.sceneBecameActive()
                guard engine.signedIn else { break }
                LiveStore.shared.reconnectIfNeeded()
                AppNotifier.shared.ensureRunning()
                KeepAlive.shared.apply()
                Task { await AppNotifier.shared.refreshAuthorization() }
                Task {
                    await engine.refreshAccount()
                    await engine.sync(reason: "foreground")
                }
            case .background:
                AppLock.shared.lockIfNeeded()
                LiveStore.shared.stop() // no dashboard stream in the background
                // AppNotifier keeps its (light) stream: with KeepAlive the
                // app stays running and notifications arrive at once.
                engine.scheduleBackgroundTasks()
                WidgetRefresher.reload() // Control Center shows the current lights
            default: break
            }
        }
    }
}

/// Launch hook: background tasks and observers must be registered before launch finishes.
final class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        // Background task handlers, HealthKit observers and location
        // monitoring must be set up before this returns.
        MainActor.assumeIsolated {
            AppNotifier.shared.setup() // before launch ends: notification taps
            SyncEngine.shared.launch()
            if SyncEngine.shared.signedIn {
                AppNotifier.shared.start()
                KeepAlive.shared.apply()
            }
        }
        return true
    }
}
