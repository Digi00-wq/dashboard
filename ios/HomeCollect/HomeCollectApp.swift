import SwiftUI
import UIKit

@main
struct HomeCollectApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @Environment(\.scenePhase) private var scenePhase
    @StateObject private var engine = SyncEngine.shared

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(engine)
                .environmentObject(LiveStore.shared)
        }
        .onChange(of: scenePhase) { _, phase in
            switch phase {
            case .active:
                guard engine.signedIn else { break }
                LiveStore.shared.start()
                Task {
                    await engine.refreshAccount()
                    await engine.sync(reason: "foreground")
                }
            case .background:
                LiveStore.shared.stop() // no open stream in the background
                engine.scheduleBackgroundTasks()
            default: break
            }
        }
    }
}

final class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        // Background task handlers, HealthKit observers and location
        // monitoring must be set up before this returns.
        MainActor.assumeIsolated { SyncEngine.shared.launch() }
        return true
    }
}
