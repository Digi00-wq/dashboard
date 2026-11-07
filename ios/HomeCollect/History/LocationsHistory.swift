import CoreLocation
import MapKit
import SwiftUI

// Location history: today's route, the places you spent the most time at in
// the selected range, and the last known position. Only shown once location
// data exists.

/// Loads places + today's route for a range; shared by card and detail view.
@MainActor
final class HistoryLocationsModel: ObservableObject {
    @Published private(set) var places: [HistoryPlace] = []
    @Published private(set) var latest: HistoryLatestPosition?
    @Published private(set) var today: [HistoryLocationPoint] = []
    @Published private(set) var route: [HistoryLocationPoint] = [] // whole range
    @Published private(set) var loaded = false
    @Published private(set) var names: [String: String] = [:] // place id -> address

    private var loadedDays: Int?

    var hasData: Bool { !places.isEmpty || latest != nil || !today.isEmpty || !route.isEmpty }

    /// `api` decides whose locations (HistoryStore.api: own or another user's).
    func load(days: Int, api: APIClient?, force: Bool = false) async {
        guard force || loadedDays != days, let api else { return }
        do {
            async let placesReq = api.historyPlaces(days: days)
            async let todayReq = api.historyLocations(date: Calendar.current.startOfDay(for: Date()))
            let (p, t) = try await (placesReq, todayReq)
            places = p.places ?? []
            latest = p.latest
            route = (p.route ?? []).sorted { $0.t < $1.t }
            today = (t.points ?? []).filter { ($0.accuracy ?? 0) <= 200 }.sorted { $0.t < $1.t }
            loadedDays = days
        } catch {
            // 403 / offline: the card simply stays hidden
        }
        loaded = true
        await resolveNames()
    }

    /// Street / area names for the top places (Apple's geocoder, rate limited:
    /// only a few, one after another, cached).
    private func resolveNames() async {
        let geocoder = CLGeocoder()
        for place in places.prefix(8) where names[place.id] == nil {
            let location = CLLocation(latitude: place.lat, longitude: place.lon)
            guard let mark = try? await geocoder.reverseGeocodeLocation(location).first else { continue }
            let street = [mark.thoroughfare, mark.subThoroughfare].compactMap { $0 }.joined(separator: " ")
            let name = mark.areasOfInterest?.first ?? (street.isEmpty ? mark.name : street)
            let full = [name, mark.locality].compactMap { $0 }.filter { !$0.isEmpty }
            if !full.isEmpty { names[place.id] = Array(NSOrderedSet(array: full)).compactMap { $0 as? String }.joined(separator: ", ") }
        }
    }

    func title(for place: HistoryPlace) -> String {
        names[place.id] ?? String(format: "%.4f, %.4f", place.lat, place.lon)
    }
}

// MARK: - Overview card

/// Overview card: map preview + top places.
struct HistoryLocationsCard: View {
    let range: HistoryRange
    @EnvironmentObject private var store: HistoryStore
    @StateObject private var model = HistoryLocationsModel()

    var body: some View {
        Group {
            if model.hasData {
                NavigationLink(value: HistoryRoute.locations) {
                    Card(title: "Locations", systemImage: "map.fill", tint: .blue) {
                        HistoryLocationsMap(model: model, maxPlaces: 5)
                            .frame(height: 200)
                            .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
                            .allowsHitTesting(false) // the whole card opens the detail view
                        if let top = model.places.first {
                            HStack(spacing: 10) {
                                Image(systemName: "star.circle.fill").foregroundStyle(.orange)
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(verbatim: model.title(for: top))
                                        .font(.subheadline.weight(.semibold))
                                        .lineLimit(1)
                                    Text("Most time: \(Fmt.minutes(Int(top.totalMin ?? 0)))")
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                }
                                Spacer()
                                Text("\(model.places.count) places")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                        }
                        if let latest = model.latest {
                            Label("Last seen \(latest.time.formatted(.relative(presentation: .named)))",
                                  systemImage: "location.fill")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }
                }
                .buttonStyle(.plain)
            }
        }
        .task(id: range) { await model.load(days: range.days, api: store.api) }
    }
}

// MARK: - Detail

/// Full list of places in the range.
struct HistoryLocationsView: View {
    let range: HistoryRange
    @EnvironmentObject private var store: HistoryStore
    @StateObject private var model = HistoryLocationsModel()

    var body: some View {
        List {
            Section {
                HistoryLocationsMap(model: model, maxPlaces: 50)
                    .frame(height: 340)
                    .listRowInsets(EdgeInsets())
            } footer: {
                Text("Light blue: your route in this time range, dark blue: today. Pins: places you stayed at, bigger = more time.")
            }

            if !model.today.isEmpty {
                Section {
                    NavigationLink(value: HistoryRoute.day(Calendar.current.startOfDay(for: Date()))) {
                        Label("Today's route and visits", systemImage: "point.topleft.down.to.point.bottomright.curvepath.fill")
                    }
                }
            }

            Section("Places") {
                if model.places.isEmpty {
                    Group {
                        if model.loaded { Text("No places in this time range yet") } else { Text("Loading…") }
                    }
                    .foregroundStyle(.secondary)
                }
                ForEach(Array(model.places.enumerated()), id: \.element.id) { index, place in
                    HStack(spacing: 12) {
                        Text("\(index + 1)")
                            .font(.caption.weight(.bold).monospacedDigit())
                            .foregroundStyle(.white)
                            .frame(width: 22, height: 22)
                            .background(index == 0 ? Color.orange : Color.red, in: Circle())
                        VStack(alignment: .leading, spacing: 2) {
                            Text(verbatim: model.title(for: place))
                                .lineLimit(2)
                            Text(details(place))
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        Spacer(minLength: 0)
                        Text(Fmt.minutes(Int(place.totalMin ?? 0)))
                            .font(.subheadline.monospacedDigit())
                    }
                }
            }

            if let latest = model.latest {
                Section("Last known position") {
                    LabeledContent("Time", value: latest.time.formatted(date: .abbreviated, time: .shortened))
                    if let acc = latest.accuracy {
                        LabeledContent("Accuracy", value: "± \(Int(acc)) m")
                    }
                }
            }
        }
        .navigationTitle("Locations")
        .navigationBarTitleDisplayMode(.inline)
        .refreshable { await model.load(days: range.days, api: store.api, force: true) }
        .task { await model.load(days: range.days, api: store.api) }
    }

    private func details(_ p: HistoryPlace) -> String {
        let visits = p.visits ?? 0
        let count = visits == 1 ? String(localized: "1 visit") : String(localized: "\(visits) visits")
        guard let last = p.lastVisitDate else { return count }
        return count + " · " + last.formatted(.relative(presentation: .named))
    }
}

// MARK: - Map

/// MapKit map with route, places and last position.
struct HistoryLocationsMap: View {
    @ObservedObject var model: HistoryLocationsModel
    let maxPlaces: Int
    @State private var camera: MapCameraPosition = .automatic

    private var shownPlaces: [HistoryPlace] { Array(model.places.prefix(maxPlaces)) }

    var body: some View {
        Map(position: $camera) {
            if model.route.count > 1 {
                MapPolyline(coordinates: model.route.map(\.coordinate))
                    .stroke(Color.blue.opacity(0.45), lineWidth: 2.5)
            }
            if model.today.count > 1 {
                MapPolyline(coordinates: model.today.map(\.coordinate))
                    .stroke(Color.blue, lineWidth: 4)
            }
            ForEach(Array(shownPlaces.enumerated()), id: \.element.id) { index, place in
                Annotation(model.names[place.id] ?? "", coordinate: place.coordinate) {
                    Circle()
                        .fill(index == 0 ? Color.orange : Color.red)
                        .frame(width: pinSize(place), height: pinSize(place))
                        .overlay(Circle().stroke(.white, lineWidth: 2))
                        .shadow(radius: 2)
                }
            }
            if let latest = model.latest {
                Annotation("", coordinate: latest.coordinate) {
                    Image(systemName: "location.fill")
                        .font(.caption)
                        .foregroundStyle(.white)
                        .padding(6)
                        .background(Color.blue, in: Circle())
                        .overlay(Circle().stroke(.white, lineWidth: 2))
                }
            }
        }
        .onAppear { fit() }
        .onChange(of: model.places) { fit() }
        .onChange(of: model.today) { fit() }
        .onChange(of: model.route) { fit() }
    }

    /// Bigger pin = more time spent there (relative to the top place).
    private func pinSize(_ p: HistoryPlace) -> CGFloat {
        let top = max(model.places.first?.totalMin ?? 1, 1)
        return 12 + 16 * CGFloat(min((p.totalMin ?? 0) / top, 1))
    }

    private func fit() {
        var coords = shownPlaces.map(\.coordinate) + model.today.map(\.coordinate) + model.route.map(\.coordinate)
        if let l = model.latest { coords.append(l.coordinate) }
        guard let first = coords.first else { camera = .automatic; return }
        var minLat = first.latitude, maxLat = first.latitude
        var minLon = first.longitude, maxLon = first.longitude
        for c in coords {
            minLat = min(minLat, c.latitude); maxLat = max(maxLat, c.latitude)
            minLon = min(minLon, c.longitude); maxLon = max(maxLon, c.longitude)
        }
        camera = .region(MKCoordinateRegion(
            center: CLLocationCoordinate2D(latitude: (minLat + maxLat) / 2, longitude: (minLon + maxLon) / 2),
            span: MKCoordinateSpan(latitudeDelta: max((maxLat - minLat) * 1.4, 0.01),
                                   longitudeDelta: max((maxLon - minLon) * 1.4, 0.01))))
    }
}
