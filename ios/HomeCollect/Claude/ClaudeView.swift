import SwiftUI

/// Claude tab (admins only): prompts to Claude Code on the server.
struct ClaudeView: View {
    @ObservedObject private var store = ClaudeStore.shared
    @Environment(\.scenePhase) private var scenePhase
    @State private var draft = ""
    @State private var confirmNewChat = false
    @FocusState private var inputFocused: Bool

    var body: some View {
        NavigationStack {
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(spacing: 12) {
                        header
                        ForEach(store.messages) { m in
                            ClaudeBubble(message: m).id(m.id)
                        }
                        Color.clear.frame(height: 1).id("bottom")
                    }
                    .padding(.horizontal, 16)
                    .padding(.vertical, 12)
                }
                .scrollDismissesKeyboard(.interactively)
                .onChange(of: store.messages) { _, _ in
                    withAnimation(.easeOut(duration: 0.2)) { proxy.scrollTo("bottom", anchor: .bottom) }
                }
                .onAppear { proxy.scrollTo("bottom", anchor: .bottom) }
            }
            .background(Color(.systemGroupedBackground))
            .safeAreaInset(edge: .bottom) { inputBar }
            .navigationTitle("Claude")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button { confirmNewChat = true } label: { Image(systemName: "square.and.pencil") }
                        .disabled(store.running || store.messages.isEmpty)
                        .accessibilityLabel("New chat")
                }
            }
            .confirmationDialog("Start a new chat?", isPresented: $confirmNewChat, titleVisibility: .visible) {
                Button("New chat", role: .destructive) { store.newChat() }
            } message: {
                Text("Claude forgets this conversation.")
            }
            .task { await store.appeared() }
            .onChange(of: scenePhase) { _, phase in
                if phase == .active { Task { await store.appeared() } }
            }
        }
    }

    // MARK: Header

    @ViewBuilder private var header: some View {
        if let error = store.statusError {
            Label(error, systemImage: "exclamationmark.triangle.fill")
                .font(.footnote)
                .foregroundStyle(.orange)
                .frame(maxWidth: .infinity, alignment: .leading)
        } else if let status = store.status, !status.enabled {
            ContentUnavailableView {
                Label("Claude is not set up", systemImage: "sparkles")
            } description: {
                Text("Set CLAUDE_BRIDGE_URL and CLAUDE_BRIDGE_TOKEN in the server's .env and start the Claude bridge (see README).")
            }
        } else if store.messages.isEmpty {
            VStack(spacing: 8) {
                Image(systemName: "sparkles")
                    .font(.largeTitle)
                    .foregroundStyle(.orange)
                Text("Ask Claude on your server")
                    .font(.headline)
                if let status = store.status {
                    Text("Claude Code works in \(status.workdir ?? "–") and can read and change your projects. Mode: \(status.permissionMode ?? "–").")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                }
            }
            .padding(.top, 60)
            .frame(maxWidth: .infinity)
        }
    }

    // MARK: Input

    private var inputBar: some View {
        HStack(alignment: .bottom, spacing: 8) {
            TextField("Message", text: $draft, axis: .vertical)
                .lineLimit(1...6)
                .focused($inputFocused)
                .padding(.horizontal, 14)
                .padding(.vertical, 9)
                .background(Color(.secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 20, style: .continuous))
            if store.running {
                Button {
                    Task { await store.cancel() }
                } label: {
                    Image(systemName: "stop.circle.fill").font(.system(size: 32))
                }
                .tint(.red)
                .accessibilityLabel("Stop")
            } else {
                Button {
                    let text = draft
                    draft = ""
                    Task { await store.send(text) }
                } label: {
                    Image(systemName: "arrow.up.circle.fill").font(.system(size: 32))
                }
                .disabled(draft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || store.sending
                          || store.status?.enabled == false)
                .accessibilityLabel("Send")
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(.bar)
    }
}

// MARK: - Bubble

private struct ClaudeBubble: View {
    let message: ClaudeMessage

    var body: some View {
        if message.role == .user {
            HStack {
                Spacer(minLength: 48)
                Text(message.text)
                    .textSelection(.enabled)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 10)
                    .foregroundStyle(.white)
                    .background(Color.accentColor, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
            }
        } else {
            VStack(alignment: .leading, spacing: 8) {
                if !message.steps.isEmpty { steps }
                if !message.text.isEmpty {
                    Text(Self.markdown(message.text))
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                footer
            }
            .padding(14)
            .background(Color(.secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 18, style: .continuous))
        }
    }

    private var steps: some View {
        VStack(alignment: .leading, spacing: 4) {
            ForEach(Array(message.steps.enumerated()), id: \.offset) { _, step in
                HStack(alignment: .firstTextBaseline, spacing: 6) {
                    Image(systemName: icon(step))
                        .foregroundStyle(color(step))
                        .frame(width: 16)
                    Text(verbatim: line(step))
                        .lineLimit(2)
                        .truncationMode(.middle)
                }
                .font(.caption.monospaced())
                .foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder private var footer: some View {
        switch message.state {
        case .running:
            HStack(spacing: 8) {
                ProgressView()
                Group {
                    if message.steps.isEmpty && message.text.isEmpty { Text("Thinking…") } else { Text("Working…") }
                }
                .font(.footnote)
                .foregroundStyle(.secondary)
            }
        case .error, .cancelled:
            Label(message.error ?? String(localized: "Stopped."),
                  systemImage: message.state == .cancelled ? "stop.circle" : "exclamationmark.triangle.fill")
                .font(.footnote)
                .foregroundStyle(message.state == .cancelled ? Color.secondary : Color.orange)
        case .done:
            if let cost = message.costUsd {
                Text(cost, format: .currency(code: "USD").precision(.fractionLength(2)))
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }
        }
    }

    private func icon(_ e: ClaudeEvent) -> String {
        switch e.kind {
        case "tool_error": return "xmark.octagon"
        case "denied": return "hand.raised.fill"
        default: return "wrench.and.screwdriver"
        }
    }

    private func color(_ e: ClaudeEvent) -> Color {
        switch e.kind {
        case "tool_error", "denied": return .orange
        default: return .secondary
        }
    }

    private func line(_ e: ClaudeEvent) -> String {
        switch e.kind {
        case "tool_error": return e.text ?? ""
        case "denied": return String(localized: "Not allowed: \(e.name ?? "") \(e.detail ?? "")")
        default: return [e.name, e.detail].compactMap { $0 }.joined(separator: "  ")
        }
    }

    /// Inline Markdown (bold, code, links), line breaks kept.
    static func markdown(_ s: String) -> AttributedString {
        (try? AttributedString(markdown: s, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace)))
            ?? AttributedString(s)
    }
}
