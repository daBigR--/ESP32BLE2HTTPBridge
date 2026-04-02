// =============================================================================
// web_ble_api.cpp — HTTP REST endpoints for BLE keyboard management
// =============================================================================
//
// Registers HTTP GET routes that let the browser-based web UI perform all
// BLE keyboard operations: scanning, pairing, connecting, disconnecting,
// unpairing, and reading the current connection state.
//
// Route summary:
//   GET /scan        — scan for nearby BLE keyboards and return results as JSON
//   GET /pair        — pair with a keyboard by address (must be in pairing mode)
//   GET /connect     — connect to a previously bonded keyboard
//   GET /unpair      — remove bond for a keyboard address
//   GET /disconnect  — drop the current BLE connection
//   GET /state       — return current connection state + recent event log
//
// All routes use URL query parameters for arguments and respond with JSON.
// =============================================================================

#include "web_ble_api.h"

#include <Arduino.h>

#include "ble_keyboard.h"
#include "ble_scanner.h"
#include "json_util.h"
#include "key_log.h"

namespace WebBleApi {

void registerRoutes(WebServer& server) {

  // ---- GET /scan ----------------------------------------------------------
  // Triggers an active BLE scan, merges results with the bond store,
  // and returns the list as JSON.  This call blocks until the scan completes.
  server.on("/scan", HTTP_GET, [&server]() {
    // Suspend auto-connect for this config session so it does not compete
    // with the scanner.  It will resume automatically after pairing or reboot.
    BLEKeyboard::setAutoConnectEnabled(false);
    KeyLog::add("HTTP /scan: starting BLE scan (auto-connect suspended)");
    BleScanner::performScan();
    KeyLog::add(String("HTTP /scan: complete, devices=") + String(BleScanner::deviceCount()));
    String out = "{\"devices\":" + BleScanner::devicesJson() + "}";
    server.send(200, "application/json", out);
  });

  // ---- GET /pair?addr=<address>&name=<name> -------------------------------
  // Initiates BLE pairing with the specified keyboard address.
  // The device must be advertising in pairing mode or the request will be
  // rejected.  name is optional but stored for display in the UI.
  server.on("/pair", HTTP_GET, [&server]() {
    if (!server.hasArg("addr")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing addr\"}");
      return;
    }
    const String addr = server.arg("addr");
    const String name = server.hasArg("name") ? server.arg("name") : "";
    KeyLog::add(String("HTTP /pair: addr=") + addr + (name.length() ? String(" name=") + name : String("")));
    const bool   ok   = BLEKeyboard::pairKeyboard(addr, name);
    KeyLog::add(String("HTTP /pair: ") + (ok ? "ok" : "failed"));
    server.send(200, "application/json",
      ok ? "{\"ok\":true}"
         : "{\"ok\":false,\"error\":\"pair failed (device may not accept new bonding now)\"}");
  });

  // ---- GET /connect?addr=<address>&name=<name> ----------------------------
  // Connects to an already-bonded keyboard.  Will fail if the address has no
  // stored bond (pair first).  Blocks until connected or a timeout occurs.
  server.on("/connect", HTTP_GET, [&server]() {
    if (!server.hasArg("addr")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing addr\"}");
      return;
    }
    const String addr = server.arg("addr");
    const String name = server.hasArg("name") ? server.arg("name") : "";
    KeyLog::add(String("HTTP /connect: addr=") + addr + (name.length() ? String(" name=") + name : String("")));
    const bool   ok   = BLEKeyboard::connectToKeyboard(addr, name);
    KeyLog::add(String("HTTP /connect: ") + (ok ? "ok" : "failed"));
    server.send(200, "application/json",
      ok ? "{\"ok\":true}"
         : "{\"ok\":false,\"error\":\"connect failed or device not bonded\"}");
  });

  // ---- GET /unpair?addr=<address> -----------------------------------------
  // Removes the BLE bond for the specified address from NimBLE's NVS store
  // and disconnects if currently connected to that keyboard.
  server.on("/unpair", HTTP_GET, [&server]() {
    if (!server.hasArg("addr")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing addr\"}");
      return;
    }
    const String addr = server.arg("addr");
    KeyLog::add(String("HTTP /unpair: addr=") + addr);
    const bool   ok   = BLEKeyboard::unpairKeyboard(addr);
    KeyLog::add(String("HTTP /unpair: ") + (ok ? "ok" : "failed"));
    server.send(200, "application/json",
      ok ? "{\"ok\":true}"
         : "{\"ok\":false,\"error\":\"unpair failed\"}");
  });

  // ---- GET /disconnect ----------------------------------------------------
  // Drops the active BLE connection without removing the bond.
  // Auto-connect will attempt to reconnect on the next scan cycle.
  server.on("/disconnect", HTTP_GET, [&server]() {
    KeyLog::add("HTTP /disconnect");
    BLEKeyboard::disconnectKeyboard();
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- GET /state ---------------------------------------------------------
  // Returns a JSON object with the complete current state:
  //   connected      — bool: whether a keyboard is currently connected
  //   name           — display name of the connected keyboard (or "")
  //   address        — BT address of the connected keyboard (or "")
  //   bondedAddress  — preferred bonded keyboard address (used for auto-connect)
  //   bondedName     — display name of the preferred bonded keyboard
  //   lastKey        — hex string of the most recently received key code
  //   keys           — JSON array of the last 40 log lines (rolling)
  //
  // The web UI polls this endpoint periodically to update its live display.
  server.on("/state", HTTP_GET, [&server]() {
    String out = "{";
    out += "\"connected\":";
    out += BLEKeyboard::isConnected() ? "true" : "false";
    out += ",\"name\":\""       + JsonUtil::escape(BLEKeyboard::connectedName())           + "\"";
    out += ",\"address\":\""    + JsonUtil::escape(BLEKeyboard::connectedAddress())        + "\"";
    out += ",\"bondedAddress\":\"" + JsonUtil::escape(BLEKeyboard::preferredBondedAddress()) + "\"";
    out += ",\"bondedName\":\"" + JsonUtil::escape(BLEKeyboard::preferredBondedName())     + "\"";
    out += ",\"lastKey\":\"";
    if (BLEKeyboard::lastKeyCode() > 0) {
      if (BLEKeyboard::lastKeyCode() < 0x10) { out += "0"; } // zero-pad
      out += String(BLEKeyboard::lastKeyCode(), HEX);
    }
    out += "\",\"keys\":" + KeyLog::toJson();
    out += "}";
    server.send(200, "application/json", out);
  });
}

} // namespace WebBleApi