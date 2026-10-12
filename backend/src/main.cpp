#include "PerMessageDeflate.h"
#include "WebSocketProtocol.h"
#include "dashboard.pb.h"
#include <thread>

#include <App.h>
#include <iostream>

// Leeres Struct für per-Socket-Zustand (Pflicht-Template-Parameter bei uWS)

// variabel namen für verschiedene subscriptions (eins für neue nachrichten,
// history clear etc)
using std::thread;

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
                    // ws->send(serializedData, uWS::OpCode::BINARY);
                  },

              .message =
                  [](auto *ws, std::string_view message, uWS::OpCode opCode) {
                    // TODO: make parser to send data that client wants
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
  // 3. Auf Port 8080 lauschen und Event-Loop starten
  app.listen(
         8080,
         [&app, serializedData](auto *token) {
           if (token) {
             std::cout
                 << "[Server] Erfolgreich gestartet auf http://0.0.0.0:8080\n";

             // 1. Grab the event loop pointer
             auto *loop = uWS::Loop::get();

             // 2. Offload terminal input to a background thread
             thread([&app, loop]() {
               std::string input;
               while (true) {
                 std::cout << "enter something: ";
                 std::getline(std::cin, input);
                 std::cout << input << "\n";

                 // 3. Dispatch back to the uWS thread safely
                 loop->defer([&app, input]() {
                   app.publish("dashboard", input, uWS::OpCode::TEXT);
                 });
               }
             }).detach();
           } else {
             std::cerr
                 << "[Server] FEHLER: Port 8080 konnte nicht belegt werden!\n";
           }
         })
      .run();

  return 0;
}
