#pragma once
// =============================================================================
// config_store.h — NVS-backed persistent configuration storage
// =============================================================================
//
// All runtime configuration that must survive reboots and power cycles is
// managed here via the Arduino Preferences library, which maps directly to
// ESP-IDF NVS (Non-Volatile Storage): a wear-levelled key-value store in the
// device's flash partition.
//
// NVS namespace: "ble_cfg"
//
// Key naming scheme (all keys are 15 chars or fewer, NVS hard limit):
//   "baseurl"       — the HTTP base URL for outgoing GET requests
//   "n_nets"        — count of stored WiFi credentials (uint8)
//   "ns<i>"         — SSID of WiFi network i        (string, i = 0..n_nets-1)
//   "np<i>"         — password of WiFi network i    (string, i = 0..n_nets-1)
//   "n_maps"        — count of stored key mappings  (uint8)
//   "k<i>"          — key code of mapping i          (uint8, i = 0..n_maps-1)
//   "p<i>"          — path of mapping i              (string, i = 0..n_maps-1)
//
// The BLEKeyboard module writes additional keys in the same namespace:
//   "bondedAddr"    — BT address of the preferred bonded keyboard
//   "bondedName"    — display name of the preferred bonded keyboard
//
// Design limits:
//   Max 8 WiFi credentials (WiFiMulti supports up to ~10; 8 is a safe budget)
//   Max 32 key mappings (well above any practical use case)
// =============================================================================

#include <Arduino.h>

#include <vector>

// Holds the SSID and WPA2 password for one known WiFi access point.
// Passwords are stored in NVS flash and never returned to HTTP clients.
struct WifiCredential {
  String ssid;      // Network name (SSID)
  String password;  // WPA2 passphrase, or empty string for an open network
};

// Maps one HID usage-page key code to a relative URL path.
// When that key is pressed in RUN mode the device fires:
//   HTTP GET <baseUrl>/<path>
struct KeyMapping {
  uint8_t keyCode;  // USB HID keyboard usage code, e.g. 0x28 = Enter
  String  path;     // Relative path appended to baseUrl, e.g. "/lights/toggle"
};

namespace ConfigStore {

// Read all stored configuration from NVS into the provided containers.
// If the old single-network format ("wifissid"/"wifipass" keys from a previous
// firmware version) is detected, the data is migrated automatically into the
// multi-network format so existing devices upgrade cleanly.
void load(std::vector<WifiCredential>& wifiNetworks,
          String&                     baseUrl,
          std::vector<KeyMapping>&    keyMappings);

// Write the current in-RAM configuration to NVS, overwriting any previous
// values.  Called by every config-modifying HTTP route handler to ensure
// data is persisted before acknowledging the request.
void save(const std::vector<WifiCredential>& wifiNetworks,
          const String&                     baseUrl,
          const std::vector<KeyMapping>&    keyMappings);

// Serialise configuration to a JSON object string:
//   { "baseUrl": "...", "networks": [{"ssid":"..."},...],
//     "mappings": [{"key":"xx","path":"..."},...] }
// Passwords are intentionally omitted from the output.
String configJson(
  const std::vector<WifiCredential>& wifiNetworks,
  const String&                     baseUrl,
  const std::vector<KeyMapping>&    keyMappings
);

// Returns true when all conditions required for RUN mode are satisfied:
//   1. wifiNetworks is non-empty (at least one network to connect to).
//   2. baseUrl is non-empty (has a destination for HTTP GETs).
//   3. keyMappings is non-empty (at least one key configured to trigger an event).
//   4. preferredBondedAddress is non-empty (a keyboard has been paired).
bool hasValidRunConfig(
  const std::vector<WifiCredential>& wifiNetworks,
  const String&                     baseUrl,
  const std::vector<KeyMapping>&    keyMappings,
  const String&                     preferredBondedAddress
);

// Call Preferences::clear() on the "ble_cfg" namespace, wiping every stored
// key in a single operation.  Triggers a clean-slate boot on the next restart.
void clearAll();

} // namespace ConfigStore
