import Foundation

/// One bubble in the Claude chat.
struct ClaudeMessage: Codable, Identifiable, Hashable {
    enum Role: String, Codable { case user, claude }
    enum State: String, Codable { case running, done, error, cancelled }

    var id = UUID()
    let role: Role
    var text: String
    /// Tool calls etc. of a Claude answer (shown small above the text).
    var steps: [ClaudeEvent] = []
    var state: State = .done
    var error: String?
    var costUsd: Double?
    let date: Date
}

/// Claude tab: the conversation with Claude Code on the server. Kept on
/// disk (chat + session id + running job), so a prompt that is still
/// running when the app goes to the background is picked up again.
@MainActor
final class ClaudeStore: ObservableObject {
    static let shared = ClaudeStore()

    @Published private(set) var messages: [ClaudeMessage] = []
    @Published private(set) var status: ClaudeStatus?
    @Published private(set) var statusError: String?
    @Published private(set) var sending = false

    /// Claude Code session to continue (nil = next prompt starts a new chat).
    private var session: String?
    /// Job still running on the server, and how many events we have of it.
    private var jobID: String?
    private var nextEvent = 0
    private var poller: Task<Void, Never>?

    var running: Bool { jobID != nil }

    private struct Saved: Codable {
        var messages: [ClaudeMessage]
        var session: String?
        var jobID: String?
        var nextEvent: Int
    }

    private static var fileURL: URL {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("claude-chat.json")
    }

    private init() {
        if let raw = try? Data(contentsOf: Self.fileURL),
           let saved = try? JSONDecoder().decode(Saved.self, from: raw) {
            messages = saved.messages
            session = saved.session
            jobID = saved.jobID
            nextEvent = saved.nextEvent
        }
    }

    private func save() {
        let saved = Saved(messages: messages, session: session, jobID: jobID, nextEvent: nextEvent)
        guard let raw = try? JSONEncoder().encode(saved) else { return }
        try? raw.write(to: Self.fileURL, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
    }

    // MARK: Lifecycle

    /// Tab shown / app back in the foreground.
    func appeared() async {
        resumePolling()
        guard let api = SyncEngine.shared.apiClient else { return }
        do {
            status = try await api.claudeStatus()
            statusError = nil
        } catch is CancellationError {
        } catch {
            statusError = Self.describe(error)
        }
    }

    /// Signed out: forget the chat.
    func reset() {
        poller?.cancel()
        poller = nil
        messages = []
        session = nil
        jobID = nil
        nextEvent = 0
        status = nil
        try? FileManager.default.removeItem(at: Self.fileURL)
    }

    /// Next prompt starts a fresh conversation.
    func newChat() {
        guard !running else { return }
        messages = []
        session = nil
        save()
    }

    // MARK: Prompts

    /// Starts a job (continues the conversation if there is one) and polls it.
    func send(_ prompt: String) async {
        let text = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty, !running, !sending, let api = SyncEngine.shared.apiClient else { return }
        sending = true
        defer { sending = false }
        messages.append(ClaudeMessage(role: .user, text: text, date: Date()))
        var answer = ClaudeMessage(role: .claude, text: "", state: .running, date: Date())
        messages.append(answer)
        save()
        do {
            jobID = try await api.claudeStart(prompt: text, session: session)
            nextEvent = 0
            save()
            resumePolling()
        } catch {
            answer.state = .error
            answer.error = Self.describe(error)
            replaceLast(answer)
        }
    }

    /// Cancels the running job.
    func cancel() async {
        guard let id = jobID, let api = SyncEngine.shared.apiClient else { return }
        try? await api.claudeCancel(id)
        // The poller picks up the final state.
    }

    // MARK: Polling

    private func resumePolling() {
        guard jobID != nil, poller == nil else { return }
        poller = Task { [weak self] in
            await self?.poll()
            self?.poller = nil
        }
    }

    /// Fetches new job events until the job ends.
    private func poll() async {
        var delay: UInt64 = 800_000_000
        var failures = 0
        let started = Date()
        while let id = jobID, !Task.isCancelled {
            guard let api = SyncEngine.shared.apiClient else { return }
            do {
                let job = try await api.claudeJob(id, after: nextEvent)
                failures = 0
                apply(job)
                if job.finished { return }
            } catch is CancellationError {
                return
            } catch APIError.server(404, _) {
                // Bridge restarted or job expired (kept 1 h after the end).
                finish(state: .error, error: String(localized: "The server no longer knows this prompt (restarted?)."))
                return
            } catch {
                failures += 1
                if failures >= 20 {
                    finish(state: .error, error: Self.describe(error))
                    return
                }
            }
            // Fast while a prompt is fresh, then calmer.
            if Date().timeIntervalSince(started) > 30 { delay = 2_000_000_000 }
            try? await Task.sleep(nanoseconds: failures > 0 ? 3_000_000_000 : delay)
        }
    }

    private func apply(_ job: ClaudeJob) {
        guard var answer = messages.last, answer.role == .claude else { return }
        for e in job.events {
            if e.kind == "text", let t = e.text {
                answer.text += (answer.text.isEmpty ? "" : "\n\n") + t
            } else {
                answer.steps.append(e)
            }
        }
        nextEvent = job.next
        if let s = job.session { session = s }
        if job.finished {
            if answer.text.isEmpty, let r = job.result, !r.isEmpty { answer.text = r }
            answer.state = ClaudeMessage.State(rawValue: job.status) ?? .done
            answer.error = job.error
            answer.costUsd = job.costUsd
            jobID = nil
        }
        replaceLast(answer)
    }

    private func finish(state: ClaudeMessage.State, error: String) {
        if var answer = messages.last, answer.role == .claude {
            answer.state = state
            answer.error = error
            replaceLast(answer)
        }
        jobID = nil
        save()
    }

    private func replaceLast(_ m: ClaudeMessage) {
        if !messages.isEmpty { messages[messages.count - 1] = m }
        save()
    }

    private static func describe(_ error: Error) -> String {
        switch error as? APIError {
        case .forbidden?:
            return String(localized: "Only admins can use Claude.")
        case .server(502, _)?:
            return String(localized: "The Claude bridge on the server is not running.")
        case .server(503, _)?:
            return String(localized: "Claude is not set up on the server.")
        case .server(409, _)?, .server(429, _)?:
            return String(localized: "Claude is still busy with another prompt.")
        default:
            return error.localizedDescription
        }
    }
}
