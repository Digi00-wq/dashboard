// Widget extension: Control Center / Lock Screen toggle for all lights.
// Reads server + token from SharedStore (App Group), calls the backend via Shared/LightsAPI.swift.

import AppIntents
import SwiftUI
import WidgetKit

/// Widgets of this extension.
@main
struct HomeCollectWidgetsBundle: WidgetBundle {
    var body: some Widget {
        LightsControl()
    }
}

/// Control Center / Lock Screen / Action button control: all lights on/off.
struct LightsControl: ControlWidget {
    var body: some ControlWidgetConfiguration {
        StaticControlConfiguration(kind: WidgetKinds.lights, provider: Provider()) { isOn in
            ControlWidgetToggle("Lights", isOn: isOn, action: LightsIntent()) { on in
                Label(on ? String(localized: "On") : String(localized: "Off"), systemImage: on ? "lightbulb.fill" : "lightbulb")
            }
        }
        .displayName("Lights")
        .description("Turn all lights on or off.")
    }

    struct Provider: ControlValueProvider {
        var previewValue: Bool { false }

        func currentValue() async throws -> Bool {
            try await LightsAPI.current().anyLightOn()
        }
    }
}

/// Runs when the control is toggled: all lights on/off.
struct LightsIntent: SetValueIntent {
    static let title: LocalizedStringResource = "Set all lights"

    @Parameter(title: "Lights on")
    var value: Bool

    init() {}

    func perform() async throws -> some IntentResult {
        try await LightsAPI.current().setAllLights(value)
        return .result()
    }
}
