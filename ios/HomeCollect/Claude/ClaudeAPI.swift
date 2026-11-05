import Foundation

// Claude tab (admins only): /api/claude/* on the dashboard, forwarded to the
// Claude Code bridge on the server (scripts/claude-bridge.py).

struct ClaudeStatus: Decodable {
    let enabled: Bool
    let version: String?
    let workdir: String?
    let permissionMode: String?
}

struct ClaudeJobStarted: Decodable {
    let id: String
}

/// One step of a running prompt, as reported by the bridge.
struct ClaudeEvent: Codable, Hashable {
    /// "text" | "tool" | "tool_error" | "denied"
    let kind: String
    let text: String?
    let name: String?
    let detail: String?
}

struct ClaudeJob: Decodable {
    let id: String
    /// "running" | "done" | "error" | "cancelled"
    let status: String
    let session: String?
    let events: [ClaudeEvent]
    let next: Int
    let result: String?
    let error: String?
    let costUsd: Double?

    var finished: Bool { status != "running" }
}

extension APIClient {
    func claudeStatus() async throws -> ClaudeStatus {
        try JSONDecoder().decode(ClaudeStatus.self, from: try await send("api/claude/status"))
    }

    /// Starts a prompt; `session` continues an earlier conversation.
    func claudeStart(prompt: String, session: String?) async throws -> String {
        var body: [String: String] = ["prompt": prompt]
        if let session { body["session"] = session }
        let data = try await send("api/claude/jobs", method: "POST",
                                  body: try JSONSerialization.data(withJSONObject: body))
        return try JSONDecoder().decode(ClaudeJobStarted.self, from: data).id
    }

    /// Progress of a job: events from index `after` on.
    func claudeJob(_ id: String, after: Int) async throws -> ClaudeJob {
        try await getJSON("api/claude/jobs/\(id)", ["after": String(after)])
    }

    func claudeCancel(_ id: String) async throws {
        _ = try await send("api/claude/jobs/\(id)/cancel", method: "POST", body: Data("{}".utf8))
    }
}
