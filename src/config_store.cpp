#include "config_store.h"

#include <Preferences.h>

#include "json_util.h"

namespace {

static Preferences gPrefs;

} // namespace

namespace ConfigStore {

void load(std::vector<WifiCredential>& wifiNetworks, String& baseUrl, std::vector<KeyMapping>& keyMappings) {
  gPrefs.begin("ble_cfg", true);
  baseUrl = gPrefs.getString("baseurl", "");

  wifiNetworks.clear();
  if (gPrefs.isKey("n_nets")) {
    uint8_t nNets = gPrefs.getUChar("n_nets", 0);
    for (uint8_t i = 0; i < nNets && i < 8; i++) {
      String sk = String("ns") + String(i);
      String pk = String("np") + String(i);
      String ssid = gPrefs.getString(sk.c_str(), "");
      String pass = gPrefs.getString(pk.c_str(), "");
      if (ssid.length() > 0) {
        wifiNetworks.push_back({ssid, pass});
      }
    }
  } else {
    // Migrate old single-SSID format
    String oldSsid = gPrefs.getString("wifissid", "");
    String oldPass = gPrefs.getString("wifipass", "");
    if (oldSsid.length() > 0) {
      wifiNetworks.push_back({oldSsid, oldPass});
    }
  }

  uint8_t n = gPrefs.getUChar("n_maps", 0);
  keyMappings.clear();
  for (uint8_t i = 0; i < n && i < 32; i++) {
    String kk = String("k") + String(i);
    String pk = String("p") + String(i);
    uint8_t code = gPrefs.getUChar(kk.c_str(), 0);
    String path = gPrefs.getString(pk.c_str(), "");
    if (code != 0 && path.length() > 0) {
      keyMappings.push_back({code, path});
    }
  }
  gPrefs.end();
}

void save(const std::vector<WifiCredential>& wifiNetworks, const String& baseUrl, const std::vector<KeyMapping>& keyMappings) {
  gPrefs.begin("ble_cfg", false);
  gPrefs.putUChar("n_nets", (uint8_t)wifiNetworks.size());
  for (size_t i = 0; i < wifiNetworks.size() && i < 8; i++) {
    String sk = String("ns") + String(i);
    String pk = String("np") + String(i);
    gPrefs.putString(sk.c_str(), wifiNetworks[i].ssid);
    gPrefs.putString(pk.c_str(), wifiNetworks[i].password);
  }
  gPrefs.putString("baseurl", baseUrl);
  gPrefs.putUChar("n_maps", (uint8_t)keyMappings.size());

  for (size_t i = 0; i < keyMappings.size() && i < 32; i++) {
    String kk = String("k") + String(i);
    String pk = String("p") + String(i);
    gPrefs.putUChar(kk.c_str(), keyMappings[i].keyCode);
    gPrefs.putString(pk.c_str(), keyMappings[i].path);
  }
  gPrefs.end();
}

String configJson(
  const std::vector<WifiCredential>& wifiNetworks,
  const String& baseUrl,
  const std::vector<KeyMapping>& keyMappings
) {
  String out = "{\"wifiNetworks\":["; 
  for (size_t i = 0; i < wifiNetworks.size(); i++) {
    if (i > 0) out += ",";
    out += "{\"ssid\":\"" + JsonUtil::escape(wifiNetworks[i].ssid) + "\"}";
  }
  out += "],\"baseUrl\":\"" + JsonUtil::escape(baseUrl) + "\",\"mappings\":[";

  for (size_t i = 0; i < keyMappings.size(); i++) {
    if (i > 0) {
      out += ",";
    }
    out += "{\"key\":\"";
    if (keyMappings[i].keyCode < 0x10) {
      out += "0";
    }
    out += String(keyMappings[i].keyCode, HEX) + "\",\"path\":\"" + JsonUtil::escape(keyMappings[i].path) + "\"}";
  }
  out += "]}";
  return out;
}

bool hasValidRunConfig(
  const std::vector<WifiCredential>& wifiNetworks,
  const String& baseUrl,
  const std::vector<KeyMapping>& keyMappings,
  const String& preferredBondedAddress
) {
  if (wifiNetworks.empty()) {
    return false;
  }
  if (baseUrl.length() == 0) {
    return false;
  }
  if (keyMappings.empty()) {
    return false;
  }
  if (preferredBondedAddress.length() == 0) {
    return false;
  }
  return true;
}

void clearAll() {
  gPrefs.begin("ble_cfg", false);
  gPrefs.clear();
  gPrefs.end();
}

} // namespace ConfigStore
