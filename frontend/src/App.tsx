import { createSignal, onMount, onCleanup, Show } from "solid-js";
// Importiere das generierte Interface und die Decode-Funktion
import { Dashboard } from "./dashboard";

export function App() {
  const [status, setStatus] = createSignal("Verbinde...");
  const [dashboardData, setDashboardData] = createSignal<Dashboard | null>(
    null,
  );
  const [testData, setTestData] = createSignal<string | null>(null);
  const ws = new WebSocket("ws://127.0.0.1:8080/ws");

  onMount(() => {
    // 1. WICHTIG: WebSocket anweisen, Binärdaten als ArrayBuffer bereitzustellen
    ws.binaryType = "arraybuffer";

    ws.onopen = () => {
      console.log("[WS] Verbunden!");
      setStatus("Verbunden");
    };

    ws.onmessage = (event: MessageEvent) => {
      // 2. Rohdaten prüfen und decodieren
      if (event.data instanceof ArrayBuffer) {
        try {
          // uWS sendet ein Binär-Frame -> mit Protobuf decode parsen
          const uint8View = new Uint8Array(event.data);
          const parsedData = Dashboard.decode(uint8View);

          console.log("[WS] Dashboard empfangen:", parsedData);
          setDashboardData(parsedData);
        } catch (err) {
          console.error("Fehler beim Dekodieren der Protobuf-Nachricht:", err);
        }
      } else if (typeof event.data === "string") {
        if (testData()) {
          setTestData(testData() + " " + String(event.data));
        } else {
          setTestData(String(event.data));
        }
      }
    };

    ws.onerror = (err) => {
      console.error("[WS] Fehler:", err);
      setStatus("Verbindungsfehler");
    };

    ws.onclose = () => {
      console.log("[WS] Getrennt");
      setStatus("Getrennt");
    };

    onCleanup(() => {
      ws.close();
    });
  });

  function senMessage() {
    ws.send("test");
  }

  return (
    <div style={{ padding: "20px", "font-family": "sans-serif" }}>
      <h1>WebSocket Status: {status()}</h1>
      <button onclick={senMessage}>Send test message</button>

      <Show when={dashboardData()}>
        {(data) => (
          <>
            <div>
              <h2>Dashboard Licht werte</h2>
              <Show when={data().light}>
                {(light) => (
                  <ul>
                    <li>Licht an: {light().isLight ? "Ja" : "Nein"}</li>
                    <li>Lux-Wert: {light().luxValue} lx</li>
                  </ul>
                )}
              </Show>
            </div>
          </>
        )}
      </Show>
      <Show when={testData()}>{(data) => <h2>{data()}</h2>}</Show>
    </div>
  );
}

export default App;
