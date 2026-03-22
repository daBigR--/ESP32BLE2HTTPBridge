#pragma once

#include <Arduino.h>

#include <vector>

struct KeyMapping {
  uint8_t keyCode;
  String path;
};

namespace ConfigStore {

void load(String& wifiSsid, String& wifiPassword, String& baseUrl, std::vector<KeyMapping>& keyMappings);

void save(const String& wifiSsid, const String& wifiPassword, const String& baseUrl, const std::vector<KeyMapping>& keyMappings);

String configJson(
  const String& wifiSsid,
  const String& wifiPassword,
  const String& baseUrl,
  const std::vector<KeyMapping>& keyMappings,
  String (*escapeJson)(const String&)
);

bool hasValidRunConfig(
  const String& wifiSsid,
  const String& baseUrl,
  const std::vector<KeyMapping>& keyMappings,
  const String& preferredBondedAddress
);

void clearAll();

} // namespace ConfigStore
