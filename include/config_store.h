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
//   "n_urls"        — count of stored base URLs (uint8)
//   "u<i>"          — base URL i                     (string, i = 0..n_urls-1)
//   "selurl"        — index of the currently selected base URL (uint8)
//   "n_nets"        — count of stored WiFi credentials (uint8)
//   "ns<i>"         — SSID of WiFi network i        (string, i = 0..n_nets-1)
//   "np<i>"         — password of WiFi network i    (string, i = 0..n_nets-1)
//   "n_sigs"        — count of stored button mappings (uint8)
//   "si<i>"         — signature of mapping i         (string, i = 0..n_sigs-1)
//   "su<i>"         — url of mapping i               (string, i = 0..n_sigs-1)
//   "sl<i>"         — label of mapping i             (string, i = 0..n_sigs-1)
//   "slp_to"        — inactivity timeout for deep sleep (uint32, ms)
//
// Migration from old single-URL format:
//   Devices running a previous firmware stored exactly one URL under "baseurl".
//   On first load after this firmware version we detect the absence of "n_urls"
//   and silently migrate the old key to the new multi-URL format.
//
// The BLEKeyboard module writes additional keys in the same namespace:
//   "bondedAddr"    — BT address of the preferred bonded keyboard
//   "bondedName"    — display name of the preferred bonded keyboard
//
// Design limits:
//   Max 8 WiFi credentials (WiFiMulti supports up to ~10; 8 is a safe budget)
//   Max 8 base URLs
//   Max 32 button mappings (well above any practical use case)
// =============================================================================

#include <Arduino.h>

#include <vector>

// Holds the SSID and WPA2 password for one known WiFi access point.
// Passwords are stored in NVS flash and never returned to HTTP clients.
struct WifiCredential {
  String ssid;      // Network name (SSID)
  String password;  // WPA2 passphrase, or empty string for an open network
};

// Maps one HID burst signature to a URL path.
// When a button with that signature is pressed in RUN mode the device fires:
//   HTTP GET <baseUrl>/<url>
struct ButtonMapping {
  String signature; // lowercase hex of the HID payload, e.g. "037828"
  String url;       // relative path appended to baseUrl, e.g. "/lights/toggle"
  String label;     // optional human-readable name; shown in UI instead of signature
};

namespace ConfigStore {

// Default inactivity timeout before entering deep sleep in RUN mode.
// Units are milliseconds.
static const uint32_t DEFAULT_SLEEP_TIMEOUT_MS = 10UL * 60UL * 1000UL;
static const uint32_t MIN_SLEEP_TIMEOUT_MS     = 30UL * 1000UL;
static const uint32_t MAX_SLEEP_TIMEOUT_MS     = 24UL * 60UL * 60UL * 1000UL;

// Read all stored configuration from NVS into the provided containers.
// If the old single-network format ("wifissid"/"wifipass" keys from a previous
// firmware version) is detected, the data is migrated automatically into the
// multi-network format so existing devices upgrade cleanly.
// If the old single-URL format ("baseurl" key from a previous firmware version)
// is detected, it is migrated into baseUrls[0] automatically.
void load(std::vector<WifiCredential>& wifiNetworks,
          std::vector<String>&         baseUrls,
          uint8_t&                     selectedUrlIndex,
          std::vector<ButtonMapping>&  buttonMappings,
          uint32_t&                    sleepTimeoutMs);

// Write the current in-RAM configuration to NVS, overwriting any previous
// values.  Called by every config-modifying HTTP route handler to ensure
// data is persisted before acknowledging the request.
void save(const std::vector<WifiCredential>& wifiNetworks,
          const std::vector<String>&         baseUrls,
          uint8_t                            selectedUrlIndex,
          const std::vector<ButtonMapping>&  buttonMappings,
          uint32_t                           sleepTimeoutMs);

// Persist only the selected URL index — lightweight NVS write called by the
// physical button long-press handler so we avoid rewriting all config.
void saveSelectedUrlIndex(uint8_t index);

// Serialise configuration to a JSON object string:
//   { "wifiNetworks": [{"ssid":"..."},...],
//     "baseUrls": ["...", ...], "selectedUrlIndex": N,
//     "mappings": [{"sig":"...","url":"...","label":"..."},...] }
// Passwords are intentionally omitted from the output.
String configJson(
  const std::vector<WifiCredential>& wifiNetworks,
  const std::vector<String>&         baseUrls,
  uint8_t                            selectedUrlIndex,
  const std::vector<ButtonMapping>&  buttonMappings,
  uint32_t                           sleepTimeoutMs
);

// Returns true when all conditions required for RUN mode are satisfied:
//   1. wifiNetworks is non-empty (at least one network to connect to).
//   2. baseUrls is non-empty (has at least one destination for HTTP GETs).
//   3. buttonMappings is non-empty (at least one button configured to trigger an event).
//   4. preferredBondedAddress is non-empty (a keyboard has been paired).
bool hasValidRunConfig(
  const std::vector<WifiCredential>& wifiNetworks,
  const std::vector<String>&         baseUrls,
  const std::vector<ButtonMapping>&  buttonMappings,
  const String&                      preferredBondedAddress
);

// Call Preferences::clear() on the "ble_cfg" namespace, wiping every stored
// key in a single operation.  Triggers a clean-slate boot on the next restart.
void clearAll();

} // namespace ConfigStore
