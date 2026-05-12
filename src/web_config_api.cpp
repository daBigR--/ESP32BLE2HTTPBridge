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
//   GET /config/addurl      — add a base URL
//   GET /config/editurl     — edit a base URL by index
//   GET /config/delurl      — remove a base URL by index
//   GET /config/addwifi     — add or update a WiFi network
//   GET /config/delwifi     — remove a WiFi network by SSID
//   GET /config/setmapping  — add or update a sig→url mapping
//   GET /config/delmapping  — remove a button mapping by hex signature
//   GET /reboot             — graceful restart
//   GET /factory-reset      — wipe NVS and restart
//
// All write routes call ConfigStore::save() immediately after modifying RAM
// state so data is never lost if the device is power-cycled before a reboot.
// =============================================================================

#include "web_config_api.h"

#include <WiFi.h>
#include <algorithm>

#include "apsta.h"
#include "ble_keyboard.h"
#include "json_util.h"
#include "key_log.h"
#include "net_fetch.h"

namespace WebConfigApi {

// true while the browser-side Test Mode panel is active and APSTA is held up.
static bool sTestModeActive = false;

void registerRoutes(WebServer& server, const Context& ctx) {

  // ---- GET /config --------------------------------------------------------
  // Returns the complete current configuration as a JSON object.
  // Passwords are NOT included in the response for security — only SSIDs.
  server.on("/config", HTTP_GET, [ctx, &server]() {
    server.send(200, "application/json",
      ConfigStore::configJson(*ctx.wifiNetworks, *ctx.baseUrls, *ctx.selectedUrlIndex,
                              *ctx.buttonMappings, *ctx.sleepTimeoutMs));
  });

  // ---- GET /config/addurl?url=<value> ------------------------------------
  // Appends a new base URL to the list (max 8 entries).  Existing entries
  // are preserved.
  server.on("/config/addurl", HTTP_GET, [ctx, &server]() {
    if (!server.hasArg("url")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing url\"}");
      return;
    }
    const String url = server.arg("url");
    if (url.length() == 0) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"empty url\"}");
      return;
    }
    if (ctx.baseUrls->size() >= 8) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"max 8 urls\"}");
      return;
    }
    ctx.baseUrls->push_back(url);
    ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrls, *ctx.selectedUrlIndex,
              *ctx.buttonMappings, *ctx.sleepTimeoutMs);
    if (ctx.logFn) { ctx.logFn(String("URL added: ") + url); }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- GET /config/editurl?idx=<n>&url=<value> ---------------------------
  // Replaces one existing base URL in place without changing list order or
  // the currently selected index.
  server.on("/config/editurl", HTTP_GET, [ctx, &server]() {
    if (!server.hasArg("idx") || !server.hasArg("url")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing idx or url\"}");
      return;
    }
    int idx = server.arg("idx").toInt();
    if (idx < 0 || idx >= (int)ctx.baseUrls->size()) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"idx out of range\"}");
      return;
    }
    const String url = server.arg("url");
    if (url.length() == 0) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"empty url\"}");
      return;
    }
    (*ctx.baseUrls)[idx] = url;
    ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrls, *ctx.selectedUrlIndex,
              *ctx.buttonMappings, *ctx.sleepTimeoutMs);
    if (ctx.logFn) { ctx.logFn(String("URL updated: index ") + String(idx)); }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- GET /config/delurl?idx=<n> ----------------------------------------
  // Removes a base URL by 0-based index.  If the selected entry is deleted,
  // selection falls back to index 0.
  server.on("/config/delurl", HTTP_GET, [ctx, &server]() {
    if (!server.hasArg("idx")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing idx\"}");
      return;
    }
    int idx = server.arg("idx").toInt();
    if (idx < 0 || idx >= (int)ctx.baseUrls->size()) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"idx out of range\"}");
      return;
    }
    ctx.baseUrls->erase(ctx.baseUrls->begin() + idx);
    if (!ctx.baseUrls->empty() && *ctx.selectedUrlIndex >= (uint8_t)ctx.baseUrls->size()) {
      *ctx.selectedUrlIndex = 0;
    }
    ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrls, *ctx.selectedUrlIndex,
              *ctx.buttonMappings, *ctx.sleepTimeoutMs);
    if (ctx.logFn) { ctx.logFn(String("URL removed: index ") + String(idx)); }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- GET /config/selecturl?idx=<n> ------------------------------------
  // Sets the active base URL index used in RUN mode for outgoing HTTP GETs.
  server.on("/config/selecturl", HTTP_GET, [ctx, &server]() {
    if (!server.hasArg("idx")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing idx\"}");
      return;
    }
    int idx = server.arg("idx").toInt();
    if (idx < 0 || idx >= (int)ctx.baseUrls->size()) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"idx out of range\"}");
      return;
    }
    *ctx.selectedUrlIndex = (uint8_t)idx;
    ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrls, *ctx.selectedUrlIndex,
              *ctx.buttonMappings, *ctx.sleepTimeoutMs);
    if (ctx.logFn) {
      ctx.logFn(String("URL selected: #") + String(*ctx.selectedUrlIndex + 1));
    }
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
        ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrls, *ctx.selectedUrlIndex,
              *ctx.buttonMappings, *ctx.sleepTimeoutMs);
        if (ctx.logFn) { ctx.logFn(String("WiFi updated: ") + ssid); }
        server.send(200, "application/json", "{\"ok\":true}");
        return;
      }
    }
    // New SSID — append it.
    ctx.wifiNetworks->push_back({ssid, pwd});
    ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrls, *ctx.selectedUrlIndex,
              *ctx.buttonMappings, *ctx.sleepTimeoutMs);
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
    ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrls, *ctx.selectedUrlIndex,
                      *ctx.buttonMappings, *ctx.sleepTimeoutMs);
    if (ctx.logFn) { ctx.logFn(String("WiFi removed: ") + ssid); }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- GET /config/setsleeptimeout?ms=<value> ---------------------------
  // Sets inactivity timeout for RUN-mode deep sleep in milliseconds.
  server.on("/config/setsleeptimeout", HTTP_GET, [ctx, &server]() {
    if (!server.hasArg("ms")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ms\"}");
      return;
    }
    uint32_t requestedMs = (uint32_t)strtoul(server.arg("ms").c_str(), nullptr, 10);
    if (requestedMs < ConfigStore::MIN_SLEEP_TIMEOUT_MS) {
      requestedMs = ConfigStore::MIN_SLEEP_TIMEOUT_MS;
    }
    if (requestedMs > ConfigStore::MAX_SLEEP_TIMEOUT_MS) {
      requestedMs = ConfigStore::MAX_SLEEP_TIMEOUT_MS;
    }
    *ctx.sleepTimeoutMs = requestedMs;
    ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrls, *ctx.selectedUrlIndex,
                      *ctx.buttonMappings, *ctx.sleepTimeoutMs);
    if (ctx.logFn) {
      ctx.logFn(String("Sleep timeout: ") + String(*ctx.sleepTimeoutMs) + String(" ms"));
    }
    server.send(200, "application/json", String("{\"ok\":true,\"sleepTimeoutMs\":") +
      String(*ctx.sleepTimeoutMs) + String("}"));
  });

  // ---- GET /config/setmapping?sig=<hex>&url=<value>&label=<value> --------
  // Adds a new sig→url mapping or updates an existing one.
  server.on("/config/setmapping", HTTP_GET, [ctx, &server]() {
    if (!server.hasArg("sig") || !server.hasArg("url")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing sig or url\"}");
      return;
    }
    String sig   = server.arg("sig");
    String url   = server.arg("url");
    String label = server.hasArg("label") ? server.arg("label") : "";
    sig.toLowerCase();
    // Update existing mapping if the signature already has an entry.
    for (auto& m : *ctx.buttonMappings) {
      if (m.signature == sig) {
        m.url   = url;
        m.label = label;
        ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrls, *ctx.selectedUrlIndex,
              *ctx.buttonMappings, *ctx.sleepTimeoutMs);
        server.send(200, "application/json", "{\"ok\":true}");
        return;
      }
    }
    // New signature — append it.
    ctx.buttonMappings->push_back({sig, url, label});
    ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrls, *ctx.selectedUrlIndex,
              *ctx.buttonMappings, *ctx.sleepTimeoutMs);
    if (ctx.logFn) {
      ctx.logFn(String("Map ") + sig + String(" -> ") + url);
    }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- GET /config/delmapping?sig=<hex> ----------------------------------
  // Removes a button mapping by signature.
  server.on("/config/delmapping", HTTP_GET, [ctx, &server]() {
    if (!server.hasArg("sig")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing sig\"}");
      return;
    }
    String sig = server.arg("sig");
    sig.toLowerCase();
    size_t before = ctx.buttonMappings->size();
    ctx.buttonMappings->erase(
      std::remove_if(ctx.buttonMappings->begin(), ctx.buttonMappings->end(),
        [&sig](const ButtonMapping& m) { return m.signature == sig; }),
      ctx.buttonMappings->end()
    );
    if (ctx.buttonMappings->size() == before) {
      server.send(200, "application/json", "{\"ok\":false,\"error\":\"sig not found\"}");
      return;
    }
    ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrls, *ctx.selectedUrlIndex,
              *ctx.buttonMappings, *ctx.sleepTimeoutMs);
    if (ctx.logFn) {
      ctx.logFn(String("Del map ") + sig);
    }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- GET /wifi/scan -------------------------------------------------------
  // Runs a synchronous WiFi scan (APSTA mode so the SoftAP stays up) and
  // returns visible networks as a JSON array sorted by RSSI descending.
  // Hidden networks (empty SSID) and duplicates are filtered out.
  // Each result: {"ssid":"...","rssi":-52,"secure":true}
  server.on("/wifi/scan", HTTP_GET, [ctx, &server]() {
    // withStaInterface enables APSTA for the scan duration, then restores
    // AP-only mode.  The entire scan + response is run inside the lambda so
    // the mode is always restored even on early return (e.g., scan failure).
    Apsta::withStaInterface([&]() {

    // Synchronous scan — blocks until complete (typically 2–5 s).
    int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);

    if (n < 0) {
      WiFi.scanDelete();
      server.send(500, "application/json", "{\"ok\":false,\"error\":\"scan failed\"}");
      return;
    }

    struct NetResult { String ssid; int rssi; bool secure; };
    std::vector<NetResult> results;
    results.reserve(n);

    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;  // skip hidden networks
      int  rssi   = WiFi.RSSI(i);
      bool secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      // Deduplicate: update existing entry if this reading is stronger.
      bool found = false;
      for (auto& r : results) {
        if (r.ssid == ssid) { if (rssi > r.rssi) r.rssi = rssi; found = true; break; }
      }
      if (!found) results.push_back({ssid, rssi, secure});
    }
    WiFi.scanDelete();

    // Sort descending by RSSI.
    std::sort(results.begin(), results.end(),
      [](const NetResult& a, const NetResult& b) { return a.rssi > b.rssi; });

    // Build JSON array.
    String json = "[";
    for (size_t i = 0; i < results.size(); i++) {
      if (i > 0) json += ',';
      String esc = results[i].ssid;
      esc.replace("\\", "\\\\");
      esc.replace("\"", "\\\"");
      json += String("{\"ssid\":\"") + esc
            + String("\",\"rssi\":") + String(results[i].rssi)
            + String(",\"secure\":") + (results[i].secure ? "true" : "false")
            + String("}");
    }
    json += ']';

    if (ctx.logFn) {
      ctx.logFn(String("WiFi scan: ") + String(results.size()) + String(" networks"));
    }
    server.send(200, "application/json", json);
    }); // end Apsta::withStaInterface
  });

  // ---- GET /diag/sta/connect -----------------------------------------------
  // Enters transient APSTA mode and connects the STA to the best available
  // saved WiFi network.  Blocks up to ~12 s (budget shared across networks).
  // The STA remains connected until /diag/sta/disconnect is called.
  // Response: {"ok":true,"ssid":"<name>","ip":"<addr>"}
  //           {"ok":false,"error":"<reason>"}
  server.on("/diag/sta/connect", HTTP_GET, [ctx, &server]() {
    auto res = Apsta::enterApsta(*ctx.wifiNetworks);
    if (res.success) {
      String ssidEsc = JsonUtil::escape(res.ssid);
      server.send(200, "application/json",
        String("{\"ok\":true,\"ssid\":\"") + ssidEsc
        + String("\",\"ip\":\"") + res.ip + String("\"}"));
    } else {
      String errEsc = JsonUtil::escape(res.error);
      server.send(200, "application/json",
        String("{\"ok\":false,\"error\":\"") + errEsc + String("\"}"));
    }
  });

  // ---- GET /diag/sta/disconnect -------------------------------------------
  // Disconnects the STA and returns to AP-only mode.  Safe to call at any
  // time; no-op if already in AP-only mode.
  server.on("/diag/sta/disconnect", HTTP_GET, [&server]() {
    Apsta::exitApsta();
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- GET /diag/fetch?url=<value> -----------------------------------------
  // Enters APSTA, performs an HTTP GET of the given URL, exits APSTA, and
  // returns the result.  Blocks up to ~17 s (12 s connect + 5 s fetch).
  // URL must start with http:// or https://.
  // Response body is capped at NetFetch::MAX_BODY_BYTES (200 KB) internally;
  // only 2 KB is forwarded in the JSON to keep the browser response small.
  // Response: {"ok":<bool>,"status":<int>,"body":"<esc>","truncated":<bool>,"error":"<esc>"}
  server.on("/diag/fetch", HTTP_GET, [ctx, &server]() {
    if (!server.hasArg("url")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing url\"}");
      return;
    }
    String url = server.arg("url");
    if (url.length() == 0 ||
        (!url.startsWith("http://") && !url.startsWith("https://"))) {
      server.send(400, "application/json",
        "{\"ok\":false,\"error\":\"url must start with http:// or https://\"}");
      return;
    }

    // If the test panel is already holding an APSTA connection, reuse it.
    // Otherwise enter APSTA, fetch, then exit — the normal transient flow.
    bool borrowedSta = sTestModeActive && (WiFi.status() == WL_CONNECTED);
    if (!borrowedSta) {
      auto conn = Apsta::enterApsta(*ctx.wifiNetworks);
      if (!conn.success) {
        String errEsc = JsonUtil::escape(conn.error);
        server.send(200, "application/json",
          String("{\"ok\":false,\"error\":\"STA connect: ") + errEsc + String("\"}"));
        // enterApsta already restored AP-only on failure — no exitApsta needed.
        return;
      }
    }

    // Perform the GET.
    NetFetch::HttpResponse resp = NetFetch::httpGet(url);

    // Only tear down APSTA if we were the ones who brought it up.
    if (!borrowedSta) { Apsta::exitApsta(); }

    // Truncate body to 2 KB for the JSON response (UI only shows ~500 chars).
    // The full body up to 200 KB was already read by httpGet; we just cap here
    // to keep the browser response small.
    static const size_t JSON_BODY_CAP = 2048;
    bool bodyUiTrunc = resp.truncated;
    if (resp.body.length() > JSON_BODY_CAP) {
      resp.body    = resp.body.substring(0, JSON_BODY_CAP);
      bodyUiTrunc  = true;
    }

    String escapedBody  = JsonUtil::escape(resp.body);
    String escapedError = JsonUtil::escape(resp.error);

    String json = String("{\"ok\":")          + (resp.success ? "true" : "false")
                + String(",\"status\":")      + String(resp.status)
                + String(",\"body\":\"")      + escapedBody + String("\"")
                + String(",\"truncated\":")   + (bodyUiTrunc ? "true" : "false")
                + String(",\"error\":\"")     + escapedError + String("\"}");
    server.send(200, "application/json", json);
  });

  // ---- GET /koreader/events_page -----------------------------------------
  // Proxies the KOReader events list from the active base URL so the browser
  // JS parser can extract events.  Returns the body as-is with text/html.
  // On failure returns HTTP 500 with JSON {"error":"<reason>"}.
  server.on("/koreader/events_page", HTTP_GET, [ctx, &server]() {
    if (ctx.baseUrls->empty()) {
      server.send(500, "application/json", "{\"error\":\"No base URL configured\"}");
      return;
    }
    size_t idx = *ctx.selectedUrlIndex;
    if (idx >= ctx.baseUrls->size()) idx = 0;
    String url = (*ctx.baseUrls)[idx];
    KeyLog::add(String("[EVENTS] proxy fetch start url=") + url);
    uint32_t t0 = millis();
    auto cr = Apsta::enterApsta(*ctx.wifiNetworks);
    if (!cr.success) {
      KeyLog::add(String("[EVENTS] proxy fetch failed: ") + cr.error);
      String errJson = String("{\"error\":\"") + JsonUtil::escape(cr.error) + String("\"}" );
      server.send(500, "application/json", errJson);
      return;
    }
    auto resp = NetFetch::httpGet(url, 10000);
    Apsta::exitApsta();
    if (!resp.success) {
      KeyLog::add(String("[EVENTS] proxy fetch failed: ") + resp.error);
      String errJson = String("{\"error\":\"") + JsonUtil::escape(resp.error) + String("\"}" );
      server.send(500, "application/json", errJson);
      return;
    }
    uint32_t elapsed = millis() - t0;
    KeyLog::add(String("[EVENTS] proxy fetch ok status=") + String(resp.status)
                + String(" bytes=") + String(resp.body.length())
                + String(" elapsed=") + String(elapsed) + String("ms"));
    server.send(200, "text/html", resp.body);
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
    ctx.baseUrls->clear();         // clear RAM copies too so they
    ctx.wifiNetworks->clear();     // are not accidentally re-saved
    ctx.buttonMappings->clear();      // before the reboot completes
    *ctx.selectedUrlIndex = 0;
    *ctx.sleepTimeoutMs = ConfigStore::DEFAULT_SLEEP_TIMEOUT_MS;
    if (ctx.factoryResetExtrasFn) {
      ctx.factoryResetExtrasFn(); // delete BLE bonds via main.cpp hook
    }
    server.send(200, "application/json", "{\"ok\":true}");
    delay(300);
    ESP.restart();
  });

  // ---- GET /test/enter ----------------------------------------------------
  // Validates that the config is complete enough for run mode (same 4
  // conditions as ConfigStore::hasValidRunConfig), then enters APSTA and
  // connects the STA to the best saved WiFi network.  While in test mode the
  // STA stays connected so /test/fire can reach the target HTTP server.
  // Response: {"ok":true,"ssid":"<name>","ip":"<addr>"}
  //           {"ok":false,"error":"<reason>"}
  server.on("/test/enter", HTTP_GET, [ctx, &server]() {
    // Refuse if the config isn't ready for run mode.
    if (!ConfigStore::hasValidRunConfig(
          *ctx.wifiNetworks,
          *ctx.baseUrls,
          *ctx.buttonMappings,
          BLEKeyboard::preferredBondedAddress())) {
      server.send(200, "application/json",
        "{\"ok\":false,\"error\":\"Config incomplete: need WiFi, base URL, "
        "at least one mapping, and a bonded keyboard.\"}");
      return;
    }
    auto res = Apsta::enterApsta(*ctx.wifiNetworks);
    if (!res.success) {
      String errEsc = JsonUtil::escape(res.error);
      server.send(200, "application/json",
        String("{\"ok\":false,\"error\":\"") + errEsc + String("\"}"));
      return;
    }
    sTestModeActive = true;
    KeyLog::add("[TEST] enter");
    String ssidEsc = JsonUtil::escape(res.ssid);
    server.send(200, "application/json",
      String("{\"ok\":true,\"ssid\":\"") + ssidEsc
      + String("\",\"ip\":\"") + res.ip + String("\"}"));
  });

  // ---- GET|POST /test/exit ------------------------------------------------
  // Disconnects the STA and exits test mode.  Registered as HTTP_ANY so the
  // browser can call it via navigator.sendBeacon() (POST) on beforeunload as
  // well as via a normal fetch (GET) when the user clicks Exit.
  server.on("/test/exit", HTTP_ANY, [&server]() {
    Apsta::exitApsta();
    sTestModeActive = false;
    KeyLog::add("[TEST] exit");
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- GET /test/fire?sig=<hex>&suffix=<path> -----------------------------
  // Fires one mapped URL while test mode is active.  The ESP32 performs the
  // HTTP GET over the already-connected STA and returns the result so the
  // browser can display it in the fires log.
  // Response: {"ok":<bool>,"status":<int>,"elapsed_ms":<int>,
  //            "body_excerpt":"<esc80>","url":"<full>","sig":"<hex>","error":"<esc>"}
  server.on("/test/fire", HTTP_GET, [ctx, &server]() {
    if (!server.hasArg("sig") || !server.hasArg("suffix")) {
      server.send(400, "application/json",
        "{\"ok\":false,\"error\":\"missing sig or suffix\"}");
      return;
    }
    if (!sTestModeActive) {
      server.send(200, "application/json",
        "{\"ok\":false,\"error\":\"not in test mode\"}");
      return;
    }
    String sig    = server.arg("sig");
    String suffix = server.arg("suffix");
    size_t idx    = *ctx.selectedUrlIndex;
    if (idx >= ctx.baseUrls->size()) idx = 0;
    if (ctx.baseUrls->empty()) {
      server.send(200, "application/json",
        "{\"ok\":false,\"error\":\"no base URL configured\"}");
      return;
    }
    String fullUrl = (*ctx.baseUrls)[idx] + suffix;
    uint32_t t0 = millis();
    NetFetch::HttpResponse resp = NetFetch::httpGet(fullUrl);
    uint32_t elapsed = millis() - t0;
    if (resp.success) {
      KeyLog::add(String("[TEST] fire signature=") + sig
                  + String(" url=") + fullUrl
                  + String(" status=") + String(resp.status)
                  + String(" elapsed=") + String(elapsed) + String("ms"));
    } else {
      KeyLog::add(String("[TEST] fire failed signature=") + sig
                  + String(" url=") + fullUrl
                  + String(" reason=") + resp.error);
    }
    // Truncate body to 80 chars for the browser log entry.
    String bodyExcerpt = resp.body.substring(0, 80);
    server.send(200, "application/json",
      String("{\"ok\":") + (resp.success ? "true" : "false")
      + String(",\"status\":") + String(resp.status)
      + String(",\"elapsed_ms\":") + String(elapsed)
      + String(",\"body_excerpt\":\"") + JsonUtil::escape(bodyExcerpt) + String("\"")
      + String(",\"url\":\"")          + JsonUtil::escape(fullUrl)     + String("\"")
      + String(",\"sig\":\"")          + JsonUtil::escape(sig)         + String("\"")
      + String(",\"error\":\"")        + JsonUtil::escape(resp.error)  + String("\"}"));
  });
}

} // namespace WebConfigApi
