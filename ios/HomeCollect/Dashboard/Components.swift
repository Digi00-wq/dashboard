import SwiftUI
import UIKit

/// Rounded card on the grouped background, like the Health / Home apps.
/// `title:` is a localized key; `verbatim:` shows runtime data as is
/// (player names, host names).
struct Card<Content: View>: View {
    let title: Text
    let systemImage: String
    var tint: Color = .accentColor
    var trailing: AnyView? = nil
    let content: () -> Content

    init(title: LocalizedStringKey, systemImage: String, tint: Color = .accentColor,
         trailing: AnyView? = nil, @ViewBuilder content: @escaping () -> Content) {
        self.init(titleText: Text(title), systemImage: systemImage, tint: tint, trailing: trailing, content: content)
    }

    init(verbatim title: String, systemImage: String, tint: Color = .accentColor,
         trailing: AnyView? = nil, @ViewBuilder content: @escaping () -> Content) {
        self.init(titleText: Text(verbatim: title), systemImage: systemImage, tint: tint, trailing: trailing, content: content)
    }

    private init(titleText: Text, systemImage: String, tint: Color,
                 trailing: AnyView?, content: @escaping () -> Content) {
        self.title = titleText
        self.systemImage = systemImage
        self.tint = tint
        self.trailing = trailing
        self.content = content
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Label {
                    title
                } icon: {
                    Image(systemName: systemImage)
                }
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(tint)
                Spacer()
                if let trailing { trailing }
            }
            content()
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color(.secondarySystemGroupedBackground),
                    in: RoundedRectangle(cornerRadius: 22, style: .continuous))
    }
}

/// Small value tile: icon, big number, caption.
/// `label:` is a localized key; `verbatimLabel:` shows a ready-made string.
struct MetricTile: View {
    let systemImage: String
    let value: String
    let label: Text
    var tint: Color = .accentColor

    init(systemImage: String, value: String, label: LocalizedStringKey, tint: Color = .accentColor) {
        self.init(systemImage: systemImage, value: value, labelText: Text(label), tint: tint)
    }

    init(systemImage: String, value: String, verbatimLabel: String, tint: Color = .accentColor) {
        self.init(systemImage: systemImage, value: value, labelText: Text(verbatim: verbatimLabel), tint: tint)
    }

    private init(systemImage: String, value: String, labelText: Text, tint: Color) {
        self.systemImage = systemImage
        self.value = value
        self.label = labelText
        self.tint = tint
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Image(systemName: systemImage)
                .font(.subheadline)
                .foregroundStyle(tint)
            Text(value)
                .font(.title3.weight(.semibold).monospacedDigit())
                .lineLimit(1)
                .minimumScaleFactor(0.7)
            label
                .font(.caption)
                .foregroundStyle(.secondary)
                .lineLimit(1)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// Capsule chip for the "at a glance" row.
struct GlanceChip: View {
    let systemImage: String
    let text: String
    var tint: Color = .secondary

    var body: some View {
        Label {
            Text(text).foregroundStyle(.primary)
        } icon: {
            Image(systemName: systemImage).foregroundStyle(tint)
        }
        .font(.footnote.weight(.medium))
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(Color(.secondarySystemGroupedBackground), in: Capsule())
    }
}

/// Album art from the dashboard server (needs the device token, so no AsyncImage).
struct CoverImage: View {
    let path: String?
    @State private var image: UIImage?
    private static let cache = NSCache<NSString, UIImage>()

    var body: some View {
        ZStack {
            Color(.tertiarySystemFill)
            if let image {
                Image(uiImage: image).resizable().scaledToFill()
            } else {
                Image(systemName: "music.note").font(.title).foregroundStyle(.secondary)
            }
        }
        .task(id: path) {
            guard let path, !path.isEmpty else { image = nil; return }
            if let cached = Self.cache.object(forKey: path as NSString) { image = cached; return }
            guard let api = SyncEngine.shared.apiClient,
                  let data = try? await api.data(path: path),
                  let img = UIImage(data: data) else { return }
            Self.cache.setObject(img, forKey: path as NSString)
            image = img
        }
    }
}

// MARK: - Formatting

enum Fmt {
    static func number(_ n: Int) -> String { n.formatted(.number) }

    static func minutes(_ m: Int) -> String {
        m >= 60 ? "\(m / 60) h \(m % 60) min" : "\(m) min"
    }

    static func clock(_ seconds: Double) -> String {
        let s = max(0, Int(seconds))
        return String(format: "%d:%02d", s / 60, s % 60)
    }

    static func bytes(_ b: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: b, countStyle: .binary)
    }

    static func speed(kib: Double) -> String {
        kib >= 1024 ? String(format: "%.1f MB/s", kib / 1024) : String(format: "%.0f KB/s", kib)
    }

    static func uptime(_ seconds: Int64) -> String {
        let d = seconds / 86400, h = (seconds % 86400) / 3600, m = (seconds % 3600) / 60
        if d > 0 { return String(localized: "\(Int(d)) d \(Int(h)) h") }
        if h > 0 { return "\(h) h \(m) min" }
        return "\(m) min"
    }
}

extension Color {
    init(rgb: Int) {
        self.init(red: Double((rgb >> 16) & 0xFF) / 255,
                  green: Double((rgb >> 8) & 0xFF) / 255,
                  blue: Double(rgb & 0xFF) / 255)
    }

    var rgbInt: Int {
        var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
        UIColor(self).getRed(&r, green: &g, blue: &b, alpha: &a)
        func c(_ v: CGFloat) -> Int { Int((min(max(v, 0), 1) * 255).rounded()) }
        return (c(r) << 16) | (c(g) << 8) | c(b)
    }
}
