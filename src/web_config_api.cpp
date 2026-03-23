// =============================================================================
// web_config_api.cpp — HTTP REST endpoints for configuration management
// =============================================================================
//
// Registers a set of HTTP GET endpoints on the WebServer instance that the
// browser-based web UI uses to read and modify the device configuration.
//
// All routes accept and return JSON.  Parameters are passed as URL query
// string arguments (?key=value) because the web UI is a simple single-page
// app that avoids PUT/POST for maximum compatibility with bare WebServer.
//
// Route summary:
//   GET /config             — read current config as JSON
//   GET /config/seturl      — update the HTTP base URL
//   GET /config/addwifi     — add or update a WiFi network
//   GET /config/delwifi     — remove a WiFi network by SSID
//   GET /config/setmapping  — add or update a key→path mapping
//   GET /config/delmapping  — remove a key mapping by hex key code
//   GET /reboot             — graceful restart
//   GET /factory-reset      — wipe NVS and restart
//
// All write routes call ConfigStore::save() immediately after modifying RAM
// state so data is never lost if the device is power-cycled before a reboot.
// =============================================================================

#include "web_config_api.h"

#include <algorithm>

namespace WebConfigApi {

void registerRoutes(WebServer& server, const Context& ctx) {

  // ---- GET /config --------------------------------------------------------
  // Returns the complete current configuration as a JSON object.
  // Passwords are NOT included in the response for security — only SSIDs.
  server.on("/config", HTTP_GET, [ctx, &server]() {
    server.send(200, "application/json",
      ConfigStore::configJson(*ctx.wifiNetworks, *ctx.baseUrl, *ctx.keyMappings));
  });

  // ---- GET /config/seturl?url=<value> ------------------------------------
  // Updates the HTTP base URL used for all outgoing GET requests.
  // Example: /config/seturl?url=http%3A%2F%2F192.168.1.10%3A8080
  server.on("/config/seturl", HTTP_GET, [ctx, &server]() {
    if (!server.hasArg("url")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing url\"}");
      return;
    }
    *ctx.baseUrl = server.arg("url");
    ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrl, *ctx.keyMappings);
    if (ctx.logFn) { ctx.logFn(String("Base URL: ") + *ctx.baseUrl); }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- GET /config/addwifi?ssid=<value>&pwd=<value> ----------------------
  // Adds a new WiFi network, or updates the password of an existing one.
  // If the SSID already exists in the list its entry is updated in-place
  // rather than duplicated.
  server.on("/config/addwifi", HTTP_GET, [ctx, &server]() {
    if (!server.hasArg("ssid")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ssid\"}");
      return;
    }
    const String ssid = server.arg("ssid");
    const String pwd  = server.hasArg("pwd") ? server.arg("pwd") : "";
    // Check for existing entry with same SSID and update it.
    for (auto& net : *ctx.wifiNetworks) {
      if (net.ssid == ssid) {
        net.password = pwd;
        ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrl, *ctx.keyMappings);
        if (ctx.logFn) { ctx.logFn(String("WiFi updated: ") + ssid); }
        server.send(200, "application/json", "{\"ok\":true}");
        return;
      }
    }
    // New SSID — append it.
    ctx.wifiNetworks->push_back({ssid, pwd});
    ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrl, *ctx.keyMappings);
    if (ctx.logFn) { ctx.logFn(String("WiFi added: ") + ssid); }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- GET /config/delwifi?ssid=<value> ----------------------------------
  // Removes a WiFi network by SSID.  Uses the erase-remove idiom with a
  // lambda predicate so no manual index management is needed.
  server.on("/config/delwifi", HTTP_GET, [ctx, &server]() {
    if (!server.hasArg("ssid")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ssid\"}");
      return;
    }
    const String ssid   = server.arg("ssid");
    size_t       before = ctx.wifiNetworks->size();
    ctx.wifiNetworks->erase(
      std::remove_if(ctx.wifiNetworks->begin(), ctx.wifiNetworks->end(),
        [&ssid](const WifiCredential& n) { return n.ssid == ssid; }),
      ctx.wifiNetworks->end()
    );
    if (ctx.wifiNetworks->size() == before) {
      // Nothing was removed — SSID was not in the list.
      server.send(200, "application/json", "{\"ok\":false,\"error\":\"ssid not found\"}");
      return;
    }
    ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrl, *ctx.keyMappings);
    if (ctx.logFn) { ctx.logFn(String("WiFi removed: ") + ssid); }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- GET /config/setmapping?key=<hex>&path=<value> --------------------
  // Adds a new key→path mapping or updates the path of an existing one.
  // The key code is supplied as a hexadecimal string without the 0x prefix,
  // e.g. "key=28" for the Enter key (USB HID usage 0x28).
  server.on("/config/setmapping", HTTP_GET, [ctx, &server]() {
    if (!server.hasArg("key") || !server.hasArg("path")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing key or path\"}");
      return;
    }
    // strtol with base 16 safely parses hex strings like "28" or "2A".
    uint8_t code = (uint8_t)strtol(server.arg("key").c_str(), nullptr, 16);
    String  path = server.arg("path");
    // Update existing mapping if the key code already has an entry.
    for (auto& m : *ctx.keyMappings) {
      if (m.keyCode == code) {
        m.path = path;
        ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrl, *ctx.keyMappings);
        server.send(200, "application/json", "{\"ok\":true}");
        return;
      }
    }
    // New key code — append it.
    ctx.keyMappings->push_back({code, path});
    ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrl, *ctx.keyMappings);
    if (ctx.logFn) {
      ctx.logFn(String("Map 0x") + String(code, HEX) + String(" -> ") + path);
    }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- GET /config/delmapping?key=<hex> ----------------------------------
  // Removes a key mapping by hex key code.  Same erase-remove pattern as
  // delwifi above.
  server.on("/config/delmapping", HTTP_GET, [ctx, &server]() {
    if (!server.hasArg("key")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing key\"}");
      return;
    }
    uint8_t code   = (uint8_t)strtol(server.arg("key").c_str(), nullptr, 16);
    size_t  before = ctx.keyMappings->size();
    ctx.keyMappings->erase(
      std::remove_if(ctx.keyMappings->begin(), ctx.keyMappings->end(),
        [code](const KeyMapping& m) { return m.keyCode == code; }),
      ctx.keyMappings->end()
    );
    if (ctx.keyMappings->size() == before) {
      server.send(200, "application/json", "{\"ok\":false,\"error\":\"key not found\"}");
      return;
    }
    ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrl, *ctx.keyMappings);
    if (ctx.logFn) {
      ctx.logFn(String("Del map 0x") + String(code, HEX));
    }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- GET /reboot -------------------------------------------------------
  // Responds with 200 OK to let the browser receive the acknowledgement
  // before the device resets.  The 300 ms delay is the minimum needed for
  // the TCP ACK and HTTP response to reach the client.
  server.on("/reboot", HTTP_GET, [&server]() {
    server.send(200, "application/json", "{\"ok\":true}");
    delay(300);
    ESP.restart();
  });

  // ---- GET /factory-reset ------------------------------------------------
  // Erases all stored configuration and immediately reboots.  On the next
  // boot the device will find no valid run config and enter CONFIG mode.
  // handleFactoryResetExtras() (registered in main.cpp) also deletes all
  // BLE bonds so the next pairing starts completely fresh.
  server.on("/factory-reset", HTTP_GET, [ctx, &server]() {
    ConfigStore::clearAll();       // wipe NVS namespace
    *ctx.baseUrl = "";             // clear RAM copies too so they
    ctx.wifiNetworks->clear();     // are not accidentally re-saved
    ctx.keyMappings->clear();      // before the reboot completes
    if (ctx.factoryResetExtrasFn) {
      ctx.factoryResetExtrasFn(); // delete BLE bonds via main.cpp hook
    }
    server.send(200, "application/json", "{\"ok\":true}");
    delay(300);
    ESP.restart();
  });
}

} // namespace WebConfigApi
