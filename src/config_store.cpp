// =============================================================================
// config_store.cpp — NVS-backed persistent configuration
// =============================================================================
//
// All runtime configuration is serialised to the ESP32's Non-Volatile Storage
// (NVS) flash partition via the Arduino Preferences library.  NVS uses a
// key-value store inside a dedicated flash partition; data survives power
// cycles and OTA updates.
//
// NVS namespace: "ble_cfg"
// ------------------------
// All keys live under this single namespace to keep them isolated from any
// other firmware components that also use Preferences.
//
// Key naming scheme:
//   "n_urls"      — number of stored base URLs (uint8, 0–8)
//   "u<i>"        — base URL i (String)
//   "selurl"      — currently selected base URL index (uint8)
//   "n_nets"      — number of stored WiFi networks (uint8, 0–8)
//   "ns<i>"       — SSID of network i (String)
//   "np<i>"       — password of network i (String)
//   "n_maps"      — number of stored key mappings (uint8, 0–32)
//   "k<i>"        — HID key code for mapping i (uint8)
//   "p<i>"        — URL path for mapping i (String)
//
// Migration from the old single-SSID / single-URL format:
//   The original firmware stored exactly one WiFi network under "wifissid"
//   and "wifipass" plus one URL under "baseurl".  On first load after the
//   upgrade we detect the absence of the new keys and silently migrate the
//   old values.  The old keys are left in NVS (harmless) to avoid a save
//   cycle on every boot.
// =============================================================================

#include "config_store.h"

#include <Preferences.h>

#include "json_util.h"

namespace {

// Preferences instance is kept as a module-level object so it can be
// opened/closed without dynamic allocation.
static Preferences gPrefs;

uint32_t clampSleepTimeoutMs(uint32_t ms) {
  if (ms < ConfigStore::MIN_SLEEP_TIMEOUT_MS) return ConfigStore::MIN_SLEEP_TIMEOUT_MS;
  if (ms > ConfigStore::MAX_SLEEP_TIMEOUT_MS) return ConfigStore::MAX_SLEEP_TIMEOUT_MS;
  return ms;
}

} // namespace

namespace ConfigStore {

// ---------------------------------------------------------------------------
// load — read all configuration from NVS into RAM
// ---------------------------------------------------------------------------
// Opens the NVS namespace in read-only mode ("true" = read-only) to avoid
// accidentally creating keys on every boot.  All three output parameters are
// populated in place.  Existing content is cleared first so stale data from
// a previous call cannot leak through.
void load(std::vector<WifiCredential>& wifiNetworks,
          std::vector<String>&         baseUrls,
          uint8_t&                     selectedUrlIndex,
          std::vector<ButtonMapping>&  buttonMappings,
          uint32_t&                    sleepTimeoutMs) {
  gPrefs.begin("ble_cfg", true); // open read-only

  // Load base URLs — prefer new multi-URL format; migrate old single-URL if needed.
  baseUrls.clear();
  if (gPrefs.isKey("n_urls")) {
    uint8_t nUrls = gPrefs.getUChar("n_urls", 0);
    for (uint8_t i = 0; i < nUrls && i < 8; i++) {
      String uk  = String("u") + String(i);
      String url = gPrefs.getString(uk.c_str(), "");
      if (url.length() > 0) {
        baseUrls.push_back(url);
      }
    }
  } else {
    String oldUrl = gPrefs.getString("baseurl", "");
    if (oldUrl.length() > 0) {
      baseUrls.push_back(oldUrl);
    }
  }
  selectedUrlIndex = gPrefs.getUChar("selurl", 0);
  if (!baseUrls.empty() && selectedUrlIndex >= (uint8_t)baseUrls.size()) {
    selectedUrlIndex = 0;
  }

  wifiNetworks.clear();
  if (gPrefs.isKey("n_nets")) {
    // New multi-network format.
    uint8_t nNets = gPrefs.getUChar("n_nets", 0);
    for (uint8_t i = 0; i < nNets && i < 8; i++) {   // 8 networks max
      String sk   = String("ns") + String(i); // e.g. "ns0", "ns1"
      String pk   = String("np") + String(i); // e.g. "np0", "np1"
      String ssid = gPrefs.getString(sk.c_str(), "");
      String pass = gPrefs.getString(pk.c_str(), "");
      if (ssid.length() > 0) {
        wifiNetworks.push_back({ssid, pass});
      }
    }
  } else {
    // Migrate old single-SSID format ("wifissid" / "wifipass" keys).
    // This branch runs exactly once — the next save() will write "n_nets"
    // and this branch will be skipped on all subsequent boots.
    String oldSsid = gPrefs.getString("wifissid", "");
    String oldPass = gPrefs.getString("wifipass",  "");
    if (oldSsid.length() > 0) {
      wifiNetworks.push_back({oldSsid, oldPass});
    }
  }

  // Load button mappings.  Maximum 32 entries; skip any entry with empty signature or url.
  uint8_t n = gPrefs.getUChar("n_sigs", 0);
  buttonMappings.clear();
  for (uint8_t i = 0; i < n && i < 32; i++) {
    String sik   = String("si") + String(i); // e.g. "si0"
    String suk   = String("su") + String(i); // e.g. "su0"
    String slk   = String("sl") + String(i); // e.g. "sl0"
    String sig   = gPrefs.getString(sik.c_str(), "");
    String url   = gPrefs.getString(suk.c_str(), "");
    String label = gPrefs.getString(slk.c_str(), "");
    if (sig.length() > 0 && url.length() > 0) {
      buttonMappings.push_back({sig, url, label});
    }
  }

  sleepTimeoutMs = clampSleepTimeoutMs(
    gPrefs.getUInt("slp_to", DEFAULT_SLEEP_TIMEOUT_MS)
  );
  gPrefs.end();
}

// ---------------------------------------------------------------------------
// save — write all configuration from RAM to NVS
// ---------------------------------------------------------------------------
// Opens the namespace in read-write mode ("false").  Overwrites all existing
// keys so the stored state always matches the RAM state exactly — there is
// no partial-update mechanism.
void save(const std::vector<WifiCredential>& wifiNetworks,
          const std::vector<String>&         baseUrls,
          uint8_t                            selectedUrlIndex,
          const std::vector<ButtonMapping>&  buttonMappings,
          uint32_t                           sleepTimeoutMs) {
  gPrefs.begin("ble_cfg", false); // open read-write
  gPrefs.putUChar("n_nets", (uint8_t)wifiNetworks.size());
  for (size_t i = 0; i < wifiNetworks.size() && i < 8; i++) {
    String sk = String("ns") + String(i);
    String pk = String("np") + String(i);
    gPrefs.putString(sk.c_str(), wifiNetworks[i].ssid);
    gPrefs.putString(pk.c_str(), wifiNetworks[i].password);
  }
  gPrefs.putUChar("n_urls", (uint8_t)baseUrls.size());
  for (size_t i = 0; i < baseUrls.size() && i < 8; i++) {
    String uk = String("u") + String(i);
    gPrefs.putString(uk.c_str(), baseUrls[i]);
  }
  gPrefs.putUChar("selurl", selectedUrlIndex);
  gPrefs.putUChar("n_sigs", (uint8_t)buttonMappings.size());
  for (size_t i = 0; i < buttonMappings.size() && i < 32; i++) {
    String sik = String("si") + String(i);
    String suk = String("su") + String(i);
    String slk = String("sl") + String(i);
    gPrefs.putString(sik.c_str(), buttonMappings[i].signature);
    gPrefs.putString(suk.c_str(), buttonMappings[i].url);
    gPrefs.putString(slk.c_str(), buttonMappings[i].label);
  }
  gPrefs.putUInt("slp_to", clampSleepTimeoutMs(sleepTimeoutMs));
  gPrefs.end();
}

// ---------------------------------------------------------------------------
// saveSelectedUrlIndex — persist only the selected URL index
// ---------------------------------------------------------------------------
void saveSelectedUrlIndex(uint8_t index) {
  gPrefs.begin("ble_cfg", false);
  gPrefs.putUChar("selurl", index);
  gPrefs.end();
}

// ---------------------------------------------------------------------------
// saveBondedName / loadBondedName — persist the bonded keyboard display name
// ---------------------------------------------------------------------------
void saveBondedName(const String& name) {
  gPrefs.begin("ble_cfg", false);
  gPrefs.putString("bondedName", name);
  gPrefs.end();
}

String loadBondedName() {
  gPrefs.begin("ble_cfg", true);
  String name = gPrefs.getString("bondedName", "");
  gPrefs.end();
  return name;
}

// ---------------------------------------------------------------------------
// configJson — serialise configuration to JSON for the web UI
// ---------------------------------------------------------------------------
// Hand-built JSON avoids pulling in a JSON library.  JsonUtil::escape() is
// used for all string values to safely handle SSIDs or paths that contain
// quotation marks, backslashes, or control characters.
String configJson(
  const std::vector<WifiCredential>& wifiNetworks,
  const std::vector<String>& baseUrls,
  uint8_t selectedUrlIndex,
  const std::vector<ButtonMapping>& buttonMappings,
  uint32_t sleepTimeoutMs
) {
  String out = "{\"wifiNetworks\":[";
  for (size_t i = 0; i < wifiNetworks.size(); i++) {
    if (i > 0) out += ",";
    // Only include the SSID — passwords are never sent to the browser.
    out += "{\"ssid\":\"" + JsonUtil::escape(wifiNetworks[i].ssid) + "\"}";
  }
  out += "],\"baseUrls\":[";
  for (size_t i = 0; i < baseUrls.size(); i++) {
    if (i > 0) out += ",";
    out += "\"" + JsonUtil::escape(baseUrls[i]) + "\"";
  }
    out += "],\"selectedUrlIndex\":" + String(selectedUrlIndex) +
      ",\"sleepTimeoutMs\":" + String(clampSleepTimeoutMs(sleepTimeoutMs)) +
      ",\"mappings\":[";

  for (size_t i = 0; i < buttonMappings.size(); i++) {
    if (i > 0) { out += ","; }
    out += "{\"sig\":\"" + JsonUtil::escape(buttonMappings[i].signature) +
           "\",\"url\":\"" + JsonUtil::escape(buttonMappings[i].url) +
           "\",\"label\":\"" + JsonUtil::escape(buttonMappings[i].label) + "\"}";
  }
  out += "]}";
  return out;
}

// ---------------------------------------------------------------------------
// hasValidRunConfig — checks that all four conditions for RUN mode are met
// ---------------------------------------------------------------------------
// Returns false (and stays in CONFIG mode) if ANY of these is missing:
//   • At least one WiFi credential — without this we cannot reach the HTTP server.
//   • A non-empty base URL — without this we have nowhere to send requests.
//   • At least one key mapping — without this no key press can trigger a GET.
//   • A preferred bonded keyboard address — without this auto-connect has no target.
bool hasValidRunConfig(
  const std::vector<WifiCredential>& wifiNetworks,
  const std::vector<String>& baseUrls,
  const std::vector<ButtonMapping>& buttonMappings,
  const String& preferredBondedAddress
) {
  if (wifiNetworks.empty())                 return false;
  if (baseUrls.empty())                     return false;
  if (buttonMappings.empty())               return false;
  if (preferredBondedAddress.length() == 0) return false;
  return true;
}

// ---------------------------------------------------------------------------
// clearAll — wipe the entire NVS namespace (factory reset)
// ---------------------------------------------------------------------------
// Preferences::clear() removes every key under the current namespace.
// After this, load() will return empty/default values and hasValidRunConfig()
// will return false, forcing CONFIG mode on the next boot.
void clearAll() {
  gPrefs.begin("ble_cfg", false);
  gPrefs.clear();
  gPrefs.end();
}

} // namespace ConfigStore
