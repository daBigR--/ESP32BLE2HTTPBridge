#pragma once

#include <Arduino.h>

#include <vector>

struct WifiCredential {
  String ssid;
  String password;
};

struct KeyMapping {
  uint8_t keyCode;
  String path;
};

namespace ConfigStore {

void load(std::vector<WifiCredential>& wifiNetworks, String& baseUrl, std::vector<KeyMapping>& keyMappings);

void save(const std::vector<WifiCredential>& wifiNetworks, const String& baseUrl, const std::vector<KeyMapping>& keyMappings);

String configJson(
  const std::vector<WifiCredential>& wifiNetworks,
  const String& baseUrl,
  const std::vector<KeyMapping>& keyMappings,
  String (*escapeJson)(const String&)
);

bool hasValidRunConfig(
  const std::vector<WifiCredential>& wifiNetworks,
  const String& baseUrl,
  const std::vector<KeyMapping>& keyMappings,
  const String& preferredBondedAddress
);

void clearAll();

} // namespace ConfigStore
