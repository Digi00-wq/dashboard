import Foundation
import SwiftUI

/// Live dashboard state: one Server-Sent-Events stream from the server
/// (same data the web dashboard gets), reconnecting on its own.
@MainActor
final class LiveStore: ObservableObject {
    static let shared = LiveStore()

    @Published var dashboard: Dashboard?
    @Published private(set) var connected = false
    @Published private(set) var lastError: String?

    private var task: Task<Void, Never>?
    private var generation = 0

    func start() {
        guard task == nil else { return }
        generation += 1
        let g = generation
        task = Task {
            await run()
            if generation == g { task = nil } // ended on its own (signed out)
        }
    }

    func stop() {
        task?.cancel()
        task = nil
        generation += 1
        connected = false
    }

    private func run() async {
        var delay: Double = 1
        while !Task.isCancelled {
            guard let api = SyncEngine.shared.apiClient else { break }
            do {
                try await api.streamDashboard { [weak self] d in
                    self?.dashboard = d
                    self?.connected = true
                    self?.lastError = nil
                }
                delay = 1 // server closed the stream normally (e.g. permissions changed)
            } catch APIError.unauthorized {
                SyncEngine.shared.deviceRevoked()
                break
            } catch {
                if Task.isCancelled { break }
                lastError = error.localizedDescription
            }
            connected = false
            try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
            delay = min(delay * 2, 30)
        }
    }

    // MARK: Commands

    func send(_ command: DashCommand) {
        guard let api = SyncEngine.shared.apiClient else { return }
        Task {
            do {
                try await api.command(command)
            } catch APIError.unauthorized {
                SyncEngine.shared.deviceRevoked()
            } catch {
                lastError = error.localizedDescription
            }
        }
    }

    func refresh() async {
        guard let api = SyncEngine.shared.apiClient else { return }
        try? await api.command(DashCommand(refresh: true))
        try? await Task.sleep(nanoseconds: 700_000_000) // let the new snapshot arrive
    }

    // Optimistic updates: the UI changes at once, the next snapshot from the
    // server (usually < 1 s later) confirms or corrects it.

    func setLamp(_ id: String, on: Bool? = nil, brightness: Int? = nil, rgb: Int? = nil) {
        if let i = dashboard?.lamps?.firstIndex(where: { $0.entityId == id }) {
            if let on { dashboard?.lamps?[i].isOn = on }
            if let brightness {
                dashboard?.lamps?[i].brightness = brightness
                dashboard?.lamps?[i].isOn = true
            }
            if let rgb { dashboard?.lamps?[i].rgb = rgb }
        }
        send(DashCommand(light: LightCommand(entityId: id, on: on, brightness: brightness, rgb: rgb)))
    }

    private var colorDebounce: [String: Task<Void, Never>] = [:]

    /// The color picker fires continuously while dragging: send only the
    /// color the finger rests on.
    func setLampColor(_ id: String, rgb: Int) {
        if let i = dashboard?.lamps?.firstIndex(where: { $0.entityId == id }) {
            dashboard?.lamps?[i].rgb = rgb
            dashboard?.lamps?[i].isOn = true
        }
        colorDebounce[id]?.cancel()
        colorDebounce[id] = Task {
            try? await Task.sleep(nanoseconds: 250_000_000)
            guard !Task.isCancelled else { return }
            send(DashCommand(light: LightCommand(entityId: id, rgb: rgb)))
        }
    }

    func setAllLights(_ on: Bool) {
        if let n = dashboard?.lamps?.count {
            for i in 0..<n where dashboard?.lamps?[i].available ?? false { dashboard?.lamps?[i].isOn = on }
        }
        send(DashCommand(allLights: on))
    }

    func completeTask(_ task: TodoTask, in list: TodoList) {
        if let li = dashboard?.todoist?.lists?.firstIndex(where: { $0.entityId == list.entityId }) {
            dashboard?.todoist?.lists?[li].task?.removeAll { $0.uid == task.uid }
        }
        send(DashCommand(completeTask: CompleteTaskCommand(uid: task.uid, entityId: list.entityId)))
    }

    func media(_ player: MediaPlayer, _ action: MediaCommand.Action,
               volume: Double? = nil, source: String? = nil, shuffle: Bool? = nil) {
        if let i = dashboard?.glance?.media?.firstIndex(where: { $0.entityId == player.entityId }) {
            switch action {
            case .playPause: dashboard?.glance?.media?[i].state = player.isPlaying ? "paused" : "playing"
            case .setVolume: dashboard?.glance?.media?[i].volume = volume
            case .setShuffle: dashboard?.glance?.media?[i].shuffle = shuffle
            case .selectSource: dashboard?.glance?.media?[i].source = source
            default: break
            }
        }
        send(DashCommand(media: MediaCommand(entityId: player.entityId, action: action,
                                             volume: volume, source: source, shuffle: shuffle)))
    }

    func announce(_ text: String) async throws {
        guard let api = SyncEngine.shared.apiClient else { return }
        try await api.command(DashCommand(announce: AnnounceCommand(text: text)))
    }
}
