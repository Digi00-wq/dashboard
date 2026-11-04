import SwiftUI

struct TasksCard: View {
    let lists: [TodoList]
    @EnvironmentObject private var live: LiveStore
    @AppStorage("selectedTodoList") private var selectedId = ""
    @State private var completing: Set<String> = []
    @State private var showAll = false

    private var list: TodoList { lists.first { $0.id == selectedId } ?? lists[0] }
    private var visible: [TodoTask] { showAll ? list.tasks : Array(list.tasks.prefix(6)) }

    var body: some View {
        Card(title: "Tasks", systemImage: "checklist", tint: .red, trailing: AnyView(listPicker)) {
            if list.tasks.isEmpty {
                Label("All done", systemImage: "checkmark.seal.fill")
                    .foregroundStyle(.secondary)
            } else {
                VStack(spacing: 0) {
                    ForEach(Array(visible.enumerated()), id: \.element.id) { index, task in
                        if index > 0 { Divider().padding(.leading, 36) }
                        row(task)
                    }
                }
                if list.tasks.count > 6 {
                    Button {
                        withAnimation { showAll.toggle() }
                    } label: {
                        if showAll { Text("Show less") } else { Text("Show all \(list.tasks.count)") }
                    }
                    .font(.subheadline)
                }
            }
        }
        .onAppear {
            if !lists.contains(where: { $0.id == selectedId }) { selectedId = lists[0].id }
        }
    }

    private var listPicker: some View {
        Menu {
            Picker("List", selection: $selectedId) {
                ForEach(lists) { l in
                    Text("\(l.name ?? l.id) (\(l.tasks.count))").tag(l.id)
                }
            }
        } label: {
            HStack(spacing: 4) {
                Text(list.name ?? list.id)
                Image(systemName: "chevron.up.chevron.down").font(.caption2)
            }
            .font(.subheadline)
        }
    }

    private func row(_ task: TodoTask) -> some View {
        let done = completing.contains(task.uid)
        return HStack(alignment: .top, spacing: 12) {
            Button {
                complete(task)
            } label: {
                Image(systemName: done ? "checkmark.circle.fill" : "circle")
                    .font(.title3)
                    .foregroundStyle(done ? Color.green : Color.secondary)
                    .contentTransition(.symbolEffect(.replace))
            }
            .buttonStyle(.plain)
            .disabled(done)
            VStack(alignment: .leading, spacing: 2) {
                Text(task.summary ?? "")
                    .strikethrough(done)
                    .foregroundStyle(done ? Color.secondary : Color.primary)
                if let due = Due(task.due) {
                    Label(due.text, systemImage: "calendar")
                        .font(.caption)
                        .foregroundStyle(due.overdue ? Color.red : Color.secondary)
                }
            }
            Spacer(minLength: 0)
        }
        .padding(.vertical, 10)
    }

    private func complete(_ task: TodoTask) {
        let current = list
        withAnimation { _ = completing.insert(task.uid) }
        UIImpactFeedbackGenerator(style: .light).impactOccurred()
        // Short delay so you see the check mark before the row disappears.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) {
            withAnimation { live.completeTask(task, in: current) }
            completing.remove(task.uid)
        }
    }
}

/// "2026-11-04" or an ISO date-time -> "Today", "Tomorrow", "Mon 10 Nov".
struct Due {
    let text: String
    let overdue: Bool

    init?(_ raw: String?) {
        guard let raw, !raw.isEmpty else { return nil }
        let date: Date
        var hasTime = false
        let day = DateFormatter()
        day.calendar = Calendar(identifier: .gregorian)
        day.dateFormat = "yyyy-MM-dd"
        if raw.count == 10, let d = day.date(from: raw) {
            date = d
        } else if let d = ISO8601DateFormatter().date(from: raw) {
            date = d
            hasTime = true
        } else {
            return nil
        }
        let cal = Calendar.current
        let today = cal.startOfDay(for: Date())
        overdue = hasTime ? date < Date() : date < today
        let time = hasTime ? " " + date.formatted(date: .omitted, time: .shortened) : ""
        if cal.isDateInToday(date) { text = String(localized: "Today") + time }
        else if cal.isDateInTomorrow(date) { text = String(localized: "Tomorrow") + time }
        else if cal.isDateInYesterday(date) { text = String(localized: "Yesterday") + time }
        else { text = date.formatted(.dateTime.weekday(.abbreviated).day().month(.abbreviated)) + time }
    }
}
