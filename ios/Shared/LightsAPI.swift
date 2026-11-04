import Foundation

/// Bare-bones server client for the widget extension (the app has the full
/// APIClient). Uses the address and token from SharedStore.
struct LightsAPI {
    enum Failure: LocalizedError {
        case notSignedIn
        case http(Int)

        var errorDescription: String? {
            switch self {
            case .notSignedIn: return String(localized: "Open HomeCollect and sign in first.")
            case .http(let code): return String(localized: "Server error \(code).")
            }
        }
    }

    let base: URL
    let token: String

    /// Throws `.notSignedIn` when the app has not shared address and token.
    static func current() throws -> LightsAPI {
        guard let base = SharedStore.serverURL, let token = SharedStore.readToken() else {
            throw Failure.notSignedIn
        }
        return LightsAPI(base: base, token: token)
    }

    private static let session: URLSession = {
        let c = URLSessionConfiguration.ephemeral
        c.timeoutIntervalForRequest = 15
        c.waitsForConnectivity = false
        return URLSession(configuration: c)
    }()

    private func request(_ path: String, query: [URLQueryItem] = []) -> URLRequest {
        var url = base.appendingPathComponent(path)
        if !query.isEmpty, var comps = URLComponents(url: url, resolvingAgainstBaseURL: false) {
            comps.queryItems = query
            if let u = comps.url { url = u }
        }
        var req = URLRequest(url: url)
        req.setValue("application/json", forHTTPHeaderField: "Accept")
        req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        return req
    }

    private func perform(_ req: URLRequest) async throws -> (Data, Int) {
        let (data, resp) = try await Self.session.data(for: req)
        let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
        guard (200..<300).contains(code) else { throw Failure.http(code) }
        return (data, code)
    }

    private struct Snapshot: Decodable {
        struct Lamp: Decodable { var isOn: Bool? }
        var lamps: [Lamp]?
    }

    /// GET /api/snapshot?format=json -> true if any lamp is on.
    func anyLightOn() async throws -> Bool {
        let (data, code) = try await perform(
            request("api/snapshot", query: [URLQueryItem(name: "format", value: "json")]))
        if code == 204 || data.isEmpty { return false } // server has no snapshot yet
        let snapshot = try JSONDecoder().decode(Snapshot.self, from: data)
        return snapshot.lamps?.contains { $0.isOn == true } ?? false
    }

    /// POST /api/command {"allLights": on}
    func setAllLights(_ on: Bool) async throws {
        var req = request("api/command")
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.httpBody = try JSONSerialization.data(withJSONObject: ["allLights": on])
        _ = try await perform(req)
    }
}
