#include "PerMessageDeflate.h"
#include "WebSocketProtocol.h"
#include "dashboard.pb.h"
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

#include <App.h>
#include <iostream>

// Leeres Struct für per-Socket-Zustand (Pflicht-Template-Parameter bei uWS)

// variabel namen für verschiedene subscriptions (eins für neue nachrichten,
// history clear etc)
using std::thread;

std::string getSerializedData(dashboard_backend::Dashboard &data) {

  std::string serializedData;
  if (!data.SerializeToString(&serializedData)) {
    std::cerr << "Fehler beim Serialisieren von Protobuf!\n";
  }
  return serializedData;
}

struct PerSocketData {};

int main() {

  // TODO: Fill out proto, Send proto over Websocket

  dashboard_backend::Dashboard data;
  auto *lightData =
      data.mutable_light(); // Protobuf übernimmt die Speicherverwaltung
  lightData->set_is_light(true);
  lightData->set_lux_value(23);

  std::mutex dataMutex;

  auto *presneceData = data.mutable_presence();

  // uWS::App instanziieren
  uWS::App app = uWS::App();

  // 1. WebSocket-Route registrieren
  app.ws<PerSocketData>(
      "/ws", {.compression = uWS::DISABLED,
              .maxPayloadLength = 64 * 1024,
              .idleTimeout = 60,

              .open =
                  [&app, &data, &dataMutex](auto *ws) {
                    std::cout << "[WS] Client verbunden!\n";
                    ws->subscribe("dashboard");

                    std::string payload;
                    {
                      std::lock_guard<std::mutex> lock(dataMutex);
                      payload = getSerializedData(data);
                    }
                    ws->send(payload, uWS::OpCode::BINARY);
                  },

              .message =
                  [](auto *ws, std::string_view message, uWS::OpCode opCode) {
                    // TODO: make parser to send data that client wants
                    std::cout
                        << "[WS] Nachricht empfangen: " << message.length()
                        << " Bytes\n";
                    // TODO: resend data
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
         [&app, &data, &dataMutex](auto *token) {
           if (!token) {
             std::cerr
                 << "[Server] FEHLER: Port 8080 konnte nicht belegt werden!\n";
             return;
           }

           std::cout
               << "[Server] Erfolgreich gestartet auf http://0.0.0.0:8080\n";
           auto *loop = uWS::Loop::get();

           std::thread([&app, loop, &data, &dataMutex]() {
             std::string test_value;
             int value;
             bool is_valid_value = false;
           loop:
             while (true) {
               is_valid_value = false;
               while (!is_valid_value) {
                 std::cout << "enter new lux value: ";
                 std::cin >> test_value;
                 is_valid_value = true;
                 for (char diget : test_value) {
                   if (!std::isdigit(diget)) {
                     is_valid_value = false;
                   }
                 }
               }

               value = std::stoi(test_value);

               std::string payload;
               {
                 std::lock_guard<std::mutex> lock(dataMutex);
                 data.mutable_light()->set_lux_value(value);
                 payload = getSerializedData(data);
               }

               loop->defer([&app, payload = std::move(payload)]() {
                 app.publish("dashboard", payload, uWS::OpCode::BINARY);
                 std::cout << "send data\n";
               });
             }
           }).detach();
         })
      .run();

  return 0;
}
