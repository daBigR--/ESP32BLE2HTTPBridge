#include "web_ble_api.h"

#include <Arduino.h>

#include "ble_keyboard.h"
#include "ble_scanner.h"
#include "json_util.h"
#include "key_log.h"

namespace WebBleApi {

void registerRoutes(WebServer& server) {
  server.on("/scan", HTTP_GET, [&server]() {
    BleScanner::performScan();
    String out = "{\"devices\":" + BleScanner::devicesJson() + "}";
    server.send(200, "application/json", out);
  });

  server.on("/pair", HTTP_GET, [&server]() {
    if (!server.hasArg("addr")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing addr\"}");
      return;
    }
    const String addr = server.arg("addr");
    const String name = server.hasArg("name") ? server.arg("name") : "";
    const bool ok = BLEKeyboard::pairKeyboard(addr, name);
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"pair failed (device may not accept new bonding now)\"}");
  });

  server.on("/connect", HTTP_GET, [&server]() {
    if (!server.hasArg("addr")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing addr\"}");
      return;
    }
    const String addr = server.arg("addr");
    const String name = server.hasArg("name") ? server.arg("name") : "";
    const bool ok = BLEKeyboard::connectToKeyboard(addr, name);
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"connect failed or device not bonded\"}");
  });

  server.on("/unpair", HTTP_GET, [&server]() {
    if (!server.hasArg("addr")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing addr\"}");
      return;
    }
    const String addr = server.arg("addr");
    const bool ok = BLEKeyboard::unpairKeyboard(addr);
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"unpair failed\"}");
  });

  server.on("/disconnect", HTTP_GET, [&server]() {
    BLEKeyboard::disconnectKeyboard();
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/state", HTTP_GET, [&server]() {
    String out = "{";
    out += "\"connected\":";
    out += BLEKeyboard::isConnected() ? "true" : "false";
    out += ",\"name\":\"" + JsonUtil::escape(BLEKeyboard::connectedName()) + "\"";
    out += ",\"address\":\"" + JsonUtil::escape(BLEKeyboard::connectedAddress()) + "\"";
    out += ",\"bondedAddress\":\"" + JsonUtil::escape(BLEKeyboard::preferredBondedAddress()) + "\"";
    out += ",\"bondedName\":\"" + JsonUtil::escape(BLEKeyboard::preferredBondedName()) + "\"";
    out += ",\"lastKey\":\"";
    if (BLEKeyboard::lastKeyCode() > 0) {
      if (BLEKeyboard::lastKeyCode() < 0x10) {
        out += "0";
      }
      out += String(BLEKeyboard::lastKeyCode(), HEX);
    }
    out += "\"";
    out += ",\"keys\":" + KeyLog::toJson();
    out += "}";
    server.send(200, "application/json", out);
  });
}

} // namespace WebBleApi