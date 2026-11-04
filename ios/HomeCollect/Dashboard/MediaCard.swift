import SwiftUI

struct MediaCard: View {
    let players: [MediaPlayer]
    @EnvironmentObject private var live: LiveStore
    @State private var selected: String?
    @State private var volume: Double = 0.5
    @State private var dragging = false

    private var player: MediaPlayer {
        players.first { $0.id == selected }
            ?? players.first(where: \.isPlaying)
            ?? players.first(where: \.isActive)
            ?? players[0]
    }

    var body: some View {
        let p = player
        Card(verbatim: p.name ?? String(localized: "Music"), systemImage: "music.note", tint: .green,
             trailing: players.count > 1 ? AnyView(playerMenu) : nil) {
            HStack(spacing: 14) {
                CoverImage(path: p.cover)
                    .frame(width: 88, height: 88)
                    .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
                VStack(alignment: .leading, spacing: 4) {
                    Text((p.title ?? "").isEmpty ? String(localized: "Nothing playing") : p.title!)
                        .font(.headline)
                        .lineLimit(2)
                    if let artist = p.artist, !artist.isEmpty {
                        Text(artist).foregroundStyle(.secondary).lineLimit(1)
                    }
                    if let source = p.source, !source.isEmpty {
                        Label(source, systemImage: "hifispeaker.fill")
                            .font(.caption)
                            .foregroundStyle(.green)
                    }
                }
                Spacer(minLength: 0)
            }

            if let duration = p.duration, duration > 0 {
                TimelineView(.periodic(from: .now, by: 1)) { ctx in
                    let pos = min(p.position(at: ctx.date) ?? 0, Double(duration))
                    VStack(spacing: 4) {
                        ProgressView(value: pos, total: Double(duration)).tint(.primary)
                        HStack {
                            Text(Fmt.clock(pos))
                            Spacer()
                            Text("-" + Fmt.clock(Double(duration) - pos))
                        }
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(.secondary)
                    }
                }
            }

            HStack {
                Button {
                    live.media(p, .setShuffle, shuffle: !(p.shuffle ?? false))
                } label: {
                    Image(systemName: "shuffle")
                        .foregroundStyle((p.shuffle ?? false) ? Color.green : Color.secondary)
                }
                Spacer()
                Button { live.media(p, .previous) } label: { Image(systemName: "backward.fill") }
                Spacer()
                Button { live.media(p, .playPause) } label: {
                    Image(systemName: p.isPlaying ? "pause.circle.fill" : "play.circle.fill")
                        .font(.system(size: 48))
                        .contentTransition(.symbolEffect(.replace))
                }
                Spacer()
                Button { live.media(p, .next) } label: { Image(systemName: "forward.fill") }
                Spacer()
                sourceMenu(p)
            }
            .font(.title3)
            .buttonStyle(.plain)

            if p.volume != nil {
                HStack(spacing: 10) {
                    Image(systemName: "speaker.fill").foregroundStyle(.secondary)
                    Slider(value: $volume, in: 0...1) { editing in
                        dragging = editing
                        if !editing { live.media(p, .setVolume, volume: volume) }
                    }
                    .tint(.primary)
                    Image(systemName: "speaker.wave.3.fill").foregroundStyle(.secondary)
                }
                .font(.caption)
            }
        }
        .onAppear { volume = player.volume ?? 0.5 }
        .onChange(of: player.volume) { _, v in
            if !dragging, let v { volume = v }
        }
    }

    private var playerMenu: some View {
        Menu {
            ForEach(players) { pl in
                Button {
                    selected = pl.id
                } label: {
                    if pl.id == player.id { Label(pl.name ?? pl.id, systemImage: "checkmark") }
                    else { Text(pl.name ?? pl.id) }
                }
            }
        } label: {
            Image(systemName: "chevron.up.chevron.down").font(.subheadline)
        }
    }

    @ViewBuilder
    private func sourceMenu(_ p: MediaPlayer) -> some View {
        if let sources = p.sources, !sources.isEmpty {
            Menu {
                ForEach(sources, id: \.self) { s in
                    Button {
                        live.media(p, .selectSource, source: s)
                    } label: {
                        if s == p.source { Label(s, systemImage: "checkmark") } else { Text(s) }
                    }
                }
            } label: {
                Image(systemName: "hifispeaker.2").foregroundStyle(.secondary)
            }
        } else {
            Image(systemName: "hifispeaker.2").foregroundStyle(.quaternary)
        }
    }
}
