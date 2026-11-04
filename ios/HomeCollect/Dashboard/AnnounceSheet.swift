import SwiftUI

/// Text spoken by the Echo ("Hans"), with your name in front.
struct AnnounceSheet: View {
    @EnvironmentObject private var live: LiveStore
    @Environment(\.dismiss) private var dismiss
    @State private var text = ""
    @State private var sending = false
    @State private var error: String?
    @State private var sent = false
    @FocusState private var focused: Bool

    // Spoken in German by the Echo.
    private let phrases = ["Essen ist fertig!", "Kommst du bitte kurz?", "Ich bin gleich zuhause.", "Gute Nacht!"]

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("What should Alexa say?", text: $text, axis: .vertical)
                        .lineLimit(2...5)
                        .focused($focused)
                        .onChange(of: text) { _, t in if t.count > 200 { text = String(t.prefix(200)) } }
                } footer: {
                    HStack {
                        Text("Spoken on the Echo with your name in front.")
                        Spacer()
                        Text("\(text.count)/200").monospacedDigit()
                    }
                }
                Section("Quick") {
                    ForEach(phrases, id: \.self) { p in
                        Button(p) { Task { await send(p) } }
                    }
                }
                if let error {
                    Section { Text(error).foregroundStyle(.red) }
                }
            }
            .navigationTitle("Announcement")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    if sent {
                        Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
                    } else if sending {
                        ProgressView()
                    } else {
                        Button("Send") { Task { await send(text) } }
                            .disabled(text.trimmingCharacters(in: .whitespaces).isEmpty)
                    }
                }
            }
            .onAppear { focused = true }
        }
        .presentationDetents([.medium, .large])
    }

    private func send(_ message: String) async {
        let m = message.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !m.isEmpty, !sending else { return }
        sending = true
        error = nil
        do {
            try await live.announce(m)
            sent = true
            UINotificationFeedbackGenerator().notificationOccurred(.success)
            try? await Task.sleep(nanoseconds: 900_000_000)
            dismiss()
        } catch {
            self.error = error.localizedDescription
        }
        sending = false
    }
}
