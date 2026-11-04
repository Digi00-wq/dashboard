import AppIntents
import SwiftUI
import WidgetKit

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
                Label(on ? "On" : "Off", systemImage: on ? "lightbulb.fill" : "lightbulb")
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
