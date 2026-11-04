import Foundation

/// Locations and phone stats waiting for upload. Persisted to disk so
/// nothing is lost when the server is unreachable or iOS kills the app.
actor Outbox {
    private struct Stored: Codable {
        var locations: [LocationDTO] = []
        var stats: [DeviceStatDTO] = []
    }

    private var data = Stored()
    private var flushing = false
    private var lastFlush = Date.distantPast
    private let file: URL
    private let maxLocations = 100_000 // ~ months offline; oldest dropped first

    init() {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        file = dir.appendingPathComponent("outbox.json")
        if let raw = try? Data(contentsOf: file), let stored = try? JSONDecoder().decode(Stored.self, from: raw) {
            data = stored
        }
    }

    var pending: Int { data.locations.count + data.stats.count }

    func add(locations: [LocationDTO]) {
        data.locations.append(contentsOf: locations)
        if data.locations.count > maxLocations {
            data.locations.removeFirst(data.locations.count - maxLocations)
        }
        save()
    }

    func add(stat: DeviceStatDTO) {
        data.stats.append(stat)
        if data.stats.count > 20_000 { data.stats.removeFirst(data.stats.count - 20_000) }
        save()
    }

    /// Detailed tracking produces many points: upload in bunches.
    func isDue() -> Bool {
        pending >= 30 || Date().timeIntervalSince(lastFlush) > 10 * 60
    }

    func flush(_ api: APIClient) async throws {
        guard !flushing else { return }
        flushing = true
        defer { flushing = false }
        while !data.locations.isEmpty || !data.stats.isEmpty {
            try Task.checkCancellation()
            // New items may be appended during the await; they go to the end,
            // so removing the first `count` items afterwards stays correct.
            let locations = Array(data.locations.prefix(5000))
            let stats = Array(data.stats.prefix(1000))
            try await api.upload(UploadBody(locations: locations, device: stats))
            data.locations.removeFirst(locations.count)
            data.stats.removeFirst(stats.count)
            save()
        }
        lastFlush = Date()
    }

    func clear() {
        data = Stored()
        save()
    }

    private func save() {
        guard let raw = try? JSONEncoder().encode(data) else { return }
        // Readable after the first unlock: background wakes happen while locked.
        try? raw.write(to: file, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
    }
}
