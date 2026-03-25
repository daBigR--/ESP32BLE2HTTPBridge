#pragma once
// =============================================================================
// web_config_api.h — HTTP routes for configuration management
// =============================================================================
//
// Registers GET endpoints on the WebServer instance that allow the browser-
// based web UI to read and modify all device configuration at runtime.
//
// Routes registered by registerRoutes():
//   GET /config              — read full configuration as JSON
//   GET /config/addurl       — add a base URL to the list (?url=)
//   GET /config/editurl      — edit a base URL by index (?idx=&url=)
//   GET /config/delurl       — remove a base URL by index (?idx=)
//   GET /config/selecturl    — set active base URL by index (?idx=)
//   GET /config/addwifi      — add or update a WiFi network (?ssid=&pwd=)
//   GET /config/delwifi      — remove a WiFi network (?ssid=)
//   GET /config/setsleeptimeout — set inactivity deep-sleep timeout (?ms=)
//   GET /config/setmapping   — add or update a key→path mapping (?key=&path=)
//   GET /config/delmapping   — remove a mapping (?key=<hex>)
//   GET /reboot              — respond 200, then ESP.restart()
//   GET /factory-reset       — wipe NVS + BLE bonds, then restart
// =============================================================================

#include <Arduino.h>
#include <WebServer.h>

#include <vector>

#include "config_store.h"

namespace WebConfigApi {

// Dependency-injection bundle passed to registerRoutes().
// Holds raw pointers to the caller's live config objects so route handlers
// can read and write the authoritative in-RAM state directly.
// All pointers must remain valid for the lifetime of the server.
struct Context {
  // Live vector of WiFi credentials in main.cpp.
  std::vector<WifiCredential>* wifiNetworks;

  // Live base-URL list in main.cpp.
  std::vector<String>* baseUrls;

  // Currently selected URL index in main.cpp.
  uint8_t* selectedUrlIndex;

  // Live key-mapping vector in main.cpp.
  std::vector<KeyMapping>* keyMappings;

  // Inactivity timeout (ms) before deep sleep in RUN mode.
  uint32_t* sleepTimeoutMs;

  // Optional — if non-null, route handlers log significant events (add/remove
  // WiFi, change URL, etc.) by calling this function with a one-line string.
  void (*logFn)(const String&);

  // Optional hook called by the factory-reset handler to perform tasks that
  // WebConfigApi itself cannot handle (specifically: deleting all BLE bonds
  // via BLEKeyboard::clearPreferredBondedDevice() and NimBLEDevice).
  // Registered in main.cpp as handleFactoryResetExtras().
  void (*factoryResetExtrasFn)();
};

// Register all configuration API routes on the given server instance.
// Must be called during setup() before server.begin().
void registerRoutes(WebServer& server, const Context& ctx);

} // namespace WebConfigApi
