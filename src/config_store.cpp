#include "config_store.h"

#include <Preferences.h>

namespace {

static Preferences gPrefs;

} // namespace

namespace ConfigStore {

void load(String& wifiSsid, String& wifiPassword, String& baseUrl, std::vector<KeyMapping>& keyMappings) {
  gPrefs.begin("ble_cfg", true);
  wifiSsid = gPrefs.getString("wifissid", "");
  wifiPassword = gPrefs.getString("wifipass", "");
  baseUrl = gPrefs.getString("baseurl", "");

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

void save(const String& wifiSsid, const String& wifiPassword, const String& baseUrl, const std::vector<KeyMapping>& keyMappings) {
  gPrefs.begin("ble_cfg", false);
  gPrefs.putString("wifissid", wifiSsid);
  gPrefs.putString("wifipass", wifiPassword);
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
  const String& wifiSsid,
  const String& wifiPassword,
  const String& baseUrl,
  const std::vector<KeyMapping>& keyMappings,
  String (*escapeJson)(const String&)
) {
  String out = "{\"wifiSsid\":\"" + escapeJson(wifiSsid) + "\",";
  out += "\"wifiPassword\":\"" + escapeJson(wifiPassword) + "\",";
  out += "\"baseUrl\":\"" + escapeJson(baseUrl) + "\",\"mappings\":[";

  for (size_t i = 0; i < keyMappings.size(); i++) {
    if (i > 0) {
      out += ",";
    }
    out += "{\"key\":\"";
    if (keyMappings[i].keyCode < 0x10) {
      out += "0";
    }
    out += String(keyMappings[i].keyCode, HEX) + "\",\"path\":\"" + escapeJson(keyMappings[i].path) + "\"}";
  }
  out += "]}";
  return out;
}

bool hasValidRunConfig(
  const String& wifiSsid,
  const String& baseUrl,
  const std::vector<KeyMapping>& keyMappings,
  const String& preferredBondedAddress
) {
  if (wifiSsid.length() == 0) {
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
