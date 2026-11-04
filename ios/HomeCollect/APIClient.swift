import Foundation

enum APIError: LocalizedError {
    case invalidURL
    case invalidCredentials
    case unauthorized // token revoked, user disabled or "collect" taken away
    case forbidden
    case tooManyAttempts(Int)
    case server(Int, String)
    case network(String)

    var errorDescription: String? {
        switch self {
        case .invalidURL: return "Invalid server address."
        case .invalidCredentials: return "Wrong user name or password."
        case .unauthorized: return "This iPhone was signed out by the server."
        case .forbidden: return "Your account is not allowed to upload data. Ask the admin to enable \"iPhone data\"."
        case .tooManyAttempts(let s): return "Too many attempts. Try again in \(max(1, s / 60)) min."
        case .server(let code, let msg): return "Server error \(code): \(msg)"
        case .network(let msg): return msg
        }
    }
}

struct APIClient {
    let base: URL
    let token: String?

    /// "dashboard.example.ch" -> https://dashboard.example.ch
    static func normalize(_ input: String) -> URL? {
        var s = input.trimmingCharacters(in: .whitespacesAndNewlines)
        if !s.contains("://") { s = "https://" + s }
        while s.hasSuffix("/") { s.removeLast() }
        guard let url = URL(string: s), let scheme = url.scheme?.lowercased(),
              scheme == "https" || scheme == "http", url.host != nil else { return nil }
        return url
    }

    private static let session: URLSession = {
        let c = URLSessionConfiguration.default
        c.timeoutIntervalForRequest = 60
        c.waitsForConnectivity = false
        return URLSession(configuration: c)
    }()

    func send(_ path: String, method: String = "GET", body: Data? = nil) async throws -> Data {
        var req = URLRequest(url: base.appendingPathComponent(path))
        req.httpMethod = method
        req.setValue("application/json", forHTTPHeaderField: "Accept")
        if let token { req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization") }
        if let body {
            req.httpBody = body
            req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        }
        let data: Data, resp: URLResponse
        do {
            (data, resp) = try await Self.session.data(for: req)
        } catch {
            throw APIError.network(error.localizedDescription)
        }
        guard let http = resp as? HTTPURLResponse else { throw APIError.network("No response") }
        if (200..<300).contains(http.statusCode) { return data }

        let json = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
        let message = json?["error"] as? String ?? HTTPURLResponse.localizedString(forStatusCode: http.statusCode)
        switch http.statusCode {
        case 401: throw token == nil ? APIError.invalidCredentials : APIError.unauthorized
        case 403: throw APIError.forbidden
        case 429: throw APIError.tooManyAttempts(json?["retry_after"] as? Int ?? 60)
        default: throw APIError.server(http.statusCode, message)
        }
    }

    func login(username: String, password: String, device: String) async throws -> LoginResponse {
        let body = try JSONSerialization.data(withJSONObject: [
            "username": username, "password": password, "device": device,
        ])
        let data = try await send("api/devices/login", method: "POST", body: body)
        return try JSONDecoder().decode(LoginResponse.self, from: data)
    }

    func logout() async throws {
        _ = try await send("api/devices/logout", method: "POST", body: Data("{}".utf8))
    }

    @discardableResult
    func upload(_ body: UploadBody) async throws -> UploadResult {
        let data = try await send("api/collect", method: "POST", body: try JSONEncoder().encode(body))
        return try JSONDecoder().decode(UploadResult.self, from: data)
    }

    func me() async throws -> MeResponse {
        try JSONDecoder().decode(MeResponse.self, from: try await send("api/me"))
    }

    func command(_ command: DashCommand) async throws {
        _ = try await send("api/command", method: "POST", body: try JSONEncoder().encode(command))
    }

    /// Raw bytes of a server path that may carry a query, e.g. a cover URL.
    func data(path: String) async throws -> Data {
        guard let url = URL(string: base.absoluteString + (path.hasPrefix("/") ? path : "/" + path)) else {
            throw APIError.invalidURL
        }
        var req = URLRequest(url: url)
        if let token { req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization") }
        let (data, resp) = try await Self.session.data(for: req)
        guard (resp as? HTTPURLResponse)?.statusCode == 200 else { throw APIError.server(0, "cover") }
        return data
    }

    private static let streamSession: URLSession = {
        let c = URLSessionConfiguration.default
        c.timeoutIntervalForRequest = 70 // server sends a heartbeat every 25 s
        c.timeoutIntervalForResource = 24 * 3600
        return URLSession(configuration: c)
    }()

    /// Live dashboard (Server-Sent Events, JSON). Returns when the server
    /// closes the stream; throws on errors.
    func streamDashboard(_ onSnapshot: @escaping @MainActor (Dashboard) -> Void) async throws {
        var comps = URLComponents(url: base.appendingPathComponent("api/events"), resolvingAgainstBaseURL: false)
        comps?.queryItems = [URLQueryItem(name: "format", value: "json")]
        guard let url = comps?.url else { throw APIError.invalidURL }
        var req = URLRequest(url: url)
        req.setValue("text/event-stream", forHTTPHeaderField: "Accept")
        if let token { req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization") }

        let (bytes, resp) = try await Self.streamSession.bytes(for: req)
        guard let http = resp as? HTTPURLResponse else { throw APIError.network("No response") }
        switch http.statusCode {
        case 200: break
        case 401: throw APIError.unauthorized
        default: throw APIError.server(http.statusCode, "stream")
        }
        let decoder = JSONDecoder()
        for try await line in bytes.lines {
            guard line.hasPrefix("data: ") else { continue } // comments / heartbeats
            let snapshot = try decoder.decode(Dashboard.self, from: Data(line.dropFirst(6).utf8))
            await onSnapshot(snapshot)
        }
    }

    func status() async throws -> ServerStatus {
        try JSONDecoder().decode(ServerStatus.self, from: try await send("api/collect/status"))
    }
}
