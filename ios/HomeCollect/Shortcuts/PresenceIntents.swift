// Shortcuts actions for presence: "I'm Home", "I'm Leaving" and "Report Presence"
// (+ HomeCollectShortcuts, which registers them with iOS at install).
// Meant for a Shortcuts automation "Wi-Fi -> when joining Sunrise_1413853 / _5G ->
// Run Immediately -> HomeCollect: I'm Home". They run in the background (the app
// does not open) and post to /api/presence via PresenceMonitor.reportFromShortcut.
// The server then runs the home automations and updates Home Assistant
// (sensor.homecollect_<user>_presence + event "homecollect_presence").

import AppIntents

/// The four presence events the server understands (raw value = API event name).
enum PresenceEventOption: String, AppEnum {
    case arriveHome = "arrive"
    case leaveHome = "leave"
    case arriveWork = "arrive_work"
    case leaveWork = "leave_work"

    static let typeDisplayRepresentation: TypeDisplayRepresentation = "Presence"
    static let caseDisplayRepresentations: [PresenceEventOption: DisplayRepresentation] = [
        .arriveHome: "Arrived home",
        .leaveHome: "Left home",
        .arriveWork: "Arrived at work",
        .leaveWork: "Left work",
    ]
}

/// Shared by all actions: report the event or fail with a readable message.
@MainActor
private func reportPresence(_ event: PresenceEventOption) async throws {
    guard SyncEngine.shared.signedIn else { throw PresenceIntentError.notSignedIn }
    try await SyncEngine.shared.presence.reportFromShortcut(event: event.rawValue)
}

enum PresenceIntentError: Error, CustomLocalizedStringResourceConvertible {
    case notSignedIn

    var localizedStringResource: LocalizedStringResource {
        switch self {
        case .notSignedIn: return "Sign in to HomeCollect first."
        }
    }
}

/// "I'm Home": e.g. when the iPhone joins the home Wi-Fi.
struct ArriveHomeIntent: AppIntent {
    static let title: LocalizedStringResource = "I'm Home"
    static let description = IntentDescription(
        "Tells your dashboard that you arrived home (lights on after sunset, Home Assistant trigger). Use it in a \"When joining Wi-Fi\" automation.")
    static let openAppWhenRun = false

    @MainActor
    func perform() async throws -> some IntentResult {
        try await reportPresence(.arriveHome)
        return .result()
    }
}

/// "I'm Leaving": leaving home (lights off, music paused after 2 min).
struct LeaveHomeIntent: AppIntent {
    static let title: LocalizedStringResource = "I'm Leaving"
    static let description = IntentDescription(
        "Tells your dashboard that you left home (lights off and music paused after 2 minutes, Home Assistant trigger).")
    static let openAppWhenRun = false

    @MainActor
    func perform() async throws -> some IntentResult {
        try await reportPresence(.leaveHome)
        return .result()
    }
}

/// Any of the four events, e.g. "Left work" from your own automation.
struct ReportPresenceIntent: AppIntent {
    static let title: LocalizedStringResource = "Report Presence"
    static let description = IntentDescription(
        "Reports arriving at or leaving home or work to your dashboard and Home Assistant.")
    static let openAppWhenRun = false

    @Parameter(title: "Event", default: .arriveHome)
    var event: PresenceEventOption

    static var parameterSummary: some ParameterSummary {
        Summary("Report \(\.$event)")
    }

    @MainActor
    func perform() async throws -> some IntentResult {
        try await reportPresence(event)
        return .result()
    }
}

/// Registers the actions with iOS right at install (Shortcuts, Siri, Spotlight),
/// so they show up under HomeCollect without the app having to run first.
struct HomeCollectShortcuts: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(
            intent: ArriveHomeIntent(),
            phrases: ["I'm home in \(.applicationName)", "\(.applicationName) I'm home"],
            shortTitle: "I'm Home",
            systemImageName: "house.fill")
        AppShortcut(
            intent: LeaveHomeIntent(),
            phrases: ["I'm leaving in \(.applicationName)", "\(.applicationName) I'm leaving"],
            shortTitle: "I'm Leaving",
            systemImageName: "figure.walk.departure")
    }
}
