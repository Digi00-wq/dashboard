import SwiftUI

struct LightsCard: View {
    let lamps: [Lamp]
    @EnvironmentObject private var live: LiveStore
    @State private var expanded: String?

    private var anyOn: Bool { lamps.contains { $0.on } }

    var body: some View {
        Card(title: "Lights", systemImage: "lightbulb.2.fill", tint: .yellow,
             trailing: AnyView(
                Toggle("All lights", isOn: Binding(get: { anyOn }, set: { live.setAllLights($0) }))
                    .labelsHidden()
                    .tint(.yellow))) {
            VStack(spacing: 0) {
                ForEach(Array(lamps.enumerated()), id: \.element.id) { index, lamp in
                    if index > 0 { Divider().padding(.leading, 40) }
                    LampRow(lamp: lamp, expanded: expanded == lamp.id) {
                        withAnimation(.snappy) { expanded = expanded == lamp.id ? nil : lamp.id }
                    }
                }
            }
        }
    }
}

struct LampRow: View {
    let lamp: Lamp
    let expanded: Bool
    let onTap: () -> Void
    @EnvironmentObject private var live: LiveStore
    @State private var brightness: Double = 255
    @State private var dragging = false

    private var available: Bool { lamp.available ?? false }
    private var color: Color { lamp.rgb.map { Color(rgb: $0) } ?? .yellow }
    private var adjustable: Bool { (lamp.supportsBrightness ?? false) || (lamp.supportsColor ?? false) }

    private var status: String {
        if !available { return "Unavailable" }
        guard lamp.on else { return "Off" }
        if let b = lamp.brightness { return "On · \(Int((Double(b) / 255 * 100).rounded())) %" }
        return "On"
    }

    var body: some View {
        VStack(spacing: 12) {
            HStack(spacing: 12) {
                Image(systemName: lamp.on ? "lightbulb.fill" : "lightbulb")
                    .font(.title3)
                    .foregroundStyle(lamp.on ? color : Color.secondary)
                    .frame(width: 28)
                VStack(alignment: .leading, spacing: 2) {
                    Text(lamp.name ?? lamp.entityId)
                    Text(status).font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                if adjustable && available {
                    Image(systemName: "chevron.down")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.tertiary)
                        .rotationEffect(.degrees(expanded ? 180 : 0))
                }
                Toggle(lamp.name ?? "", isOn: Binding(get: { lamp.on },
                                                      set: { live.setLamp(lamp.entityId, on: $0) }))
                    .labelsHidden()
                    .tint(.yellow)
                    .disabled(!available)
            }
            .contentShape(Rectangle())
            .onTapGesture { if adjustable && available { onTap() } }

            if expanded {
                if lamp.supportsBrightness == true {
                    HStack(spacing: 10) {
                        Image(systemName: "sun.min").foregroundStyle(.secondary)
                        Slider(value: $brightness, in: 1...255) { editing in
                            dragging = editing
                            if !editing { live.setLamp(lamp.entityId, brightness: Int(brightness)) }
                        }
                        .tint(color)
                        Image(systemName: "sun.max.fill").foregroundStyle(.secondary)
                    }
                    .padding(.leading, 40)
                }
                if lamp.supportsColor == true {
                    ColorPicker("Color", selection: Binding(
                        get: { color },
                        set: { live.setLampColor(lamp.entityId, rgb: $0.rgbInt) }),
                                supportsOpacity: false)
                        .padding(.leading, 40)
                }
            }
        }
        .padding(.vertical, 10)
        .onAppear { brightness = Double(lamp.brightness ?? 255) }
        .onChange(of: lamp.brightness) { _, b in
            if !dragging, let b { brightness = Double(b) }
        }
    }
}
