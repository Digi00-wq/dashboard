import Foundation
import HealthKit

/// What we read from HealthKit and in which unit it is stored.
enum HealthTypes {
    static let quantities: [(HKQuantityTypeIdentifier, HKUnit)] = [
        (.stepCount, .count()),
        (.distanceWalkingRunning, .meter()),
        (.flightsClimbed, .count()),
        (.activeEnergyBurned, .kilocalorie()),
        (.basalEnergyBurned, .kilocalorie()),
        (.appleExerciseTime, .minute()),
        (.appleStandTime, .minute()),
        (.heartRate, .count().unitDivided(by: .minute())),
        (.restingHeartRate, .count().unitDivided(by: .minute())),
        (.walkingHeartRateAverage, .count().unitDivided(by: .minute())),
        (.heartRateVariabilitySDNN, .secondUnit(with: .milli)),
        (.respiratoryRate, .count().unitDivided(by: .minute())),
        (.oxygenSaturation, .percent()),
        (.vo2Max, HKUnit.literUnit(with: .milli).unitDivided(by: HKUnit.gramUnit(with: .kilo).unitMultiplied(by: .minute()))),
        (.walkingSpeed, .meter().unitDivided(by: .second())),
        (.headphoneAudioExposure, .decibelAWeightedSoundPressureLevel()),
        (.environmentalAudioExposure, .decibelAWeightedSoundPressureLevel()),
        (.appleSleepingWristTemperature, .degreeCelsius()),
        (.bodyMass, .gramUnit(with: .kilo)),
        (.bodyFatPercentage, .percent()),
    ]
    static let categories: [HKCategoryTypeIdentifier] = [.sleepAnalysis, .mindfulSession]

    static let units: [String: HKUnit] = Dictionary(
        uniqueKeysWithValues: quantities.map { ($0.0.rawValue, $0.1) })

    static var all: [HKSampleType] {
        quantities.map { HKQuantityType($0.0) }
            + categories.map { HKCategoryType($0) }
            + [HKObjectType.workoutType()]
    }

    static func dto(_ s: HKSample) -> HealthSampleDTO? {
        // The server accepts at most "now + 1 day"; skip odd future samples.
        if s.endDate > Date().addingTimeInterval(86_000) || s.endDate < s.startDate { return nil }
        var value: Double?
        var unit: String?
        var meta: [String: MetaValue] = [:]

        switch s {
        case let q as HKQuantitySample:
            guard let u = units[q.quantityType.identifier], q.quantity.is(compatibleWith: u) else { return nil }
            value = q.quantity.doubleValue(for: u)
            unit = u.unitString
        case let c as HKCategorySample:
            value = Double(c.value)
            if c.categoryType.identifier == HKCategoryTypeIdentifier.sleepAnalysis.rawValue {
                meta["stage"] = .s(sleepStage(c.value))
            }
        case let w as HKWorkout:
            value = w.duration
            unit = "s"
            meta["activity"] = .s(activityName(w.workoutActivityType))
            if let kcal = w.statistics(for: HKQuantityType(.activeEnergyBurned))?
                .sumQuantity()?.doubleValue(for: .kilocalorie()) {
                meta["energyKcal"] = .n(kcal)
            }
            if let km = w.statistics(for: HKQuantityType(.distanceWalkingRunning))?
                .sumQuantity()?.doubleValue(for: .meter()) {
                meta["distanceM"] = .n(km)
            }
            if let hr = w.statistics(for: HKQuantityType(.heartRate))?
                .averageQuantity()?.doubleValue(for: .count().unitDivided(by: .minute())) {
                meta["avgHeartRate"] = .n(hr)
            }
        default:
            return nil
        }
        if let v = value, !v.isFinite { return nil }
        if let model = s.device?.model { meta["device"] = .s(clean(model, maxBytes: 64)) }

        return HealthSampleDTO(
            uuid: s.uuid.uuidString,
            type: s.sampleType.identifier,
            start: ms(s.startDate),
            end: ms(s.endDate),
            value: value,
            unit: unit.map { clean($0, maxBytes: 32) },
            source: clean(s.sourceRevision.source.name, maxBytes: 128),
            meta: meta.isEmpty ? nil : meta)
    }

    static func sleepStage(_ raw: Int) -> String {
        switch HKCategoryValueSleepAnalysis(rawValue: raw) {
        case .inBed: return "inBed"
        case .awake: return "awake"
        case .asleepCore: return "core"
        case .asleepDeep: return "deep"
        case .asleepREM: return "rem"
        default: return "asleep"
        }
    }

    static func activityName(_ t: HKWorkoutActivityType) -> String {
        switch t {
        case .basketball: return "basketball"
        case .soccer: return "football"
        case .americanFootball: return "americanFootball"
        case .running: return "running"
        case .walking: return "walking"
        case .hiking: return "hiking"
        case .cycling: return "cycling"
        case .swimming: return "swimming"
        case .traditionalStrengthTraining: return "strength"
        case .functionalStrengthTraining: return "functionalStrength"
        case .highIntensityIntervalTraining: return "hiit"
        case .coreTraining: return "core"
        case .yoga: return "yoga"
        case .tennis: return "tennis"
        case .snowSports, .downhillSkiing, .snowboarding: return "snow"
        case .other: return "other"
        default: return "type\(t.rawValue)"
        }
    }
}

/// Today's numbers read straight from this iPhone's HealthKit.
struct LocalToday {
    var steps: Int?
    var activeKcal: Double?
    var heartRate: Int?
}

/// Copies HealthKit to the server, type by type, with anchored queries:
/// the first run sends your whole history, later runs only what changed
/// (including deletions). An anchor is only saved after the server stored
/// the batch, so nothing is lost when an upload fails.
actor HealthSync {
    let store = HKHealthStore()
    private var inFlight = Set<String>()
    private var observing = false
    private let pageSize = 2000

    nonisolated static var available: Bool { HKHealthStore.isHealthDataAvailable() }

    func requestAuthorization() async throws {
        guard Self.available else { return }
        try await store.requestAuthorization(toShare: [], read: Set(HealthTypes.all.map { $0 as HKObjectType }))
    }

    /// HealthKit wakes the app (at most hourly) when new data arrives.
    /// Must be set up on every launch, early.
    func startObserving(onChange: @escaping (HKSampleType) async -> Void) {
        guard Self.available, !observing else { return }
        observing = true
        for type in HealthTypes.all {
            let query = HKObserverQuery(sampleType: type, predicate: nil) { _, completion, error in
                guard error == nil else { completion(); return }
                Task {
                    await onChange(type)
                    completion()
                }
            }
            store.execute(query)
            store.enableBackgroundDelivery(for: type, frequency: .hourly) { _, _ in }
        }
    }

    /// Same numbers as the Health app (HealthKit merges iPhone + Watch).
    func today() async -> LocalToday? {
        guard Self.available else { return nil }
        let start = Calendar.current.startOfDay(for: Date())
        let today = HKQuery.predicateForSamples(withStart: start, end: nil)

        func sum(_ id: HKQuantityTypeIdentifier, _ unit: HKUnit) async -> Double? {
            let q = HKStatisticsQueryDescriptor(
                predicate: .quantitySample(type: HKQuantityType(id), predicate: today),
                options: .cumulativeSum)
            return try? await q.result(for: store)?.sumQuantity()?.doubleValue(for: unit)
        }

        var out = LocalToday()
        out.steps = await sum(.stepCount, .count()).map { Int($0.rounded()) }
        out.activeKcal = await sum(.activeEnergyBurned, .kilocalorie())

        let recent = HKQuery.predicateForSamples(withStart: Date().addingTimeInterval(-6 * 3600), end: nil)
        let hr = HKSampleQueryDescriptor(
            predicates: [.quantitySample(type: HKQuantityType(.heartRate), predicate: recent)],
            sortDescriptors: [SortDescriptor(\.endDate, order: .reverse)],
            limit: 1)
        if let sample = try? await hr.result(for: store).first {
            out.heartRate = Int(sample.quantity.doubleValue(for: .count().unitDivided(by: .minute())).rounded())
        }
        // Nothing readable (no permission / locked): fall back to server data.
        if out.steps == nil && out.activeKcal == nil && out.heartRate == nil { return nil }
        return out
    }

    func stopObserving() {
        store.disableAllBackgroundDelivery { _, _ in }
        observing = false
    }

    /// Syncs every type. `progress` gets the number of samples sent so far.
    func syncAll(api: APIClient, progress: @escaping (Int) -> Void) async throws {
        var sent = 0
        for type in HealthTypes.all {
            try Task.checkCancellation()
            sent += try await sync(type, api: api) { n in progress(sent + n) }
        }
    }

    @discardableResult
    func sync(_ type: HKSampleType, api: APIClient, progress: ((Int) -> Void)? = nil) async throws -> Int {
        let key = type.identifier
        guard !inFlight.contains(key) else { return 0 }
        inFlight.insert(key)
        defer { inFlight.remove(key) }

        var anchor = loadAnchor(key)
        var total = 0
        while true {
            try Task.checkCancellation()
            let descriptor = HKAnchoredObjectQueryDescriptor(
                predicates: [.sample(type: type)], anchor: anchor, limit: pageSize)
            let result = try await descriptor.result(for: store)
            let samples = result.addedSamples.compactMap(HealthTypes.dto)
            let deleted = result.deletedObjects.map { $0.uuid.uuidString }
            if !samples.isEmpty || !deleted.isEmpty {
                try await api.upload(UploadBody(health: samples, deleted: deleted))
            }
            anchor = result.newAnchor
            saveAnchor(anchor, key)
            total += samples.count
            progress?(total)
            if result.addedSamples.count + result.deletedObjects.count < pageSize { break }
        }
        return total
    }

    private func loadAnchor(_ key: String) -> HKQueryAnchor? {
        guard let data = UserDefaults.standard.data(forKey: "anchor." + key) else { return nil }
        return try? NSKeyedUnarchiver.unarchivedObject(ofClass: HKQueryAnchor.self, from: data)
    }

    private func saveAnchor(_ anchor: HKQueryAnchor?, _ key: String) {
        guard let anchor,
              let data = try? NSKeyedArchiver.archivedData(withRootObject: anchor, requiringSecureCoding: true)
        else { return }
        UserDefaults.standard.set(data, forKey: "anchor." + key)
    }
}
