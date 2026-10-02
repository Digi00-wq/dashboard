#include "PerMessageDeflate.h"
#include "WebSocketProtocol.h"
#include "dashboard.pb.h"

#include <App.h>
#include <iostream>

// Leeres Struct für per-Socket-Zustand (Pflicht-Template-Parameter bei uWS)
struct PerSocketData {};

int main() {

  // TODO: Fill out proto, Send proto over Websocket

  dashboard_backend::Dashboard data;
  auto *lightData =
      data.mutable_light(); // Protobuf übernimmt die Speicherverwaltung
  lightData->set_is_light(true);
  lightData->set_lux_value(23);

  auto *presneceData = data.mutable_presence();

  std::string serializedData;
  if (!data.SerializeToString(&serializedData)) {
    std::cerr << "Fehler beim Serialisieren von Protobuf!\n";
    return 1;
  }

  // uWS::App instanziieren
  uWS::App app = uWS::App();

  // 1. WebSocket-Route registrieren
  app.ws<PerSocketData>(
      "/ws", {.compression = uWS::DISABLED,
              .maxPayloadLength = 64 * 1024,
              .idleTimeout = 60,

              .open =
                  [&serializedData](auto *ws) {
                    std::cout << "[WS] Client verbunden!\n";
                    ws->subscribe("dashboard");

                    ws->send(serializedData, uWS::OpCode::BINARY);
                  },

              .message =
                  [](auto *ws, std::string_view message, uWS::OpCode opCode) {
                    std::cout
                        << "[WS] Nachricht empfangen: " << message.length()
                        << " Bytes\n";
                  },

              .close =
                  [](auto *ws, int code, std::string_view message) {
                    std::cout << "[WS] Client getrennt\n";
                  }});

  // 2. HTTP-Test-Route für den Browser
  app.get("/", [](auto *res, auto *req) {
    res->writeHeader("Content-Type", "text/plain")
        ->end("uWebSockets laeuft! WebSocket ist verfuegbar auf /ws");
  });
  app.get("/test", [](auto *res, auto *req) {
    res->writeHeader("Content-Type", "application/json")
        ->end("{\"name\":\"Paul\"}");
  });

  // 3. Auf Port 8080 lauschen und Event-Loop starten
  app.listen(
         8080,
         [](auto *token) {
           if (token) {
             std::cout
                 << "[Server] Erfolgreich gestartet auf http://0.0.0.0:8080\n";
           } else {
             std::cerr
                 << "[Server] FEHLER: Port 8080 konnte nicht belegt werden!\n";
           }
         })
      .run();

  return 0;
}
