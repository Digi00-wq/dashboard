import SwiftUI

/// App-wide navigation: selected tab and deep links
/// (homecollect://briefing from the morning notification).
@MainActor
final class AppRouter: ObservableObject {
    static let shared = AppRouter()

    enum Tab: Hashable { case home, history, people, claude, settings }

    @Published var selectedTab: Tab = .home
    @Published var showBriefing = false

    func handle(_ url: URL) {
        guard url.scheme == "homecollect" else { return }
        switch url.host {
        case "briefing":
            selectedTab = .home
            showBriefing = true
        case "history": selectedTab = .history
        case "people": selectedTab = .people
        case "claude": selectedTab = .claude
        case "settings": selectedTab = .settings
        default: break
        }
    }
}
