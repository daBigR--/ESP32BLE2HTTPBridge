#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <vector>

#include "config_store.h"

namespace WebConfigApi {

struct Context {
  std::vector<WifiCredential>* wifiNetworks;
  String* baseUrl;
  std::vector<KeyMapping>* keyMappings;
  String (*jsonEscape)(const String&);
  void (*logFn)(const String&);
  void (*factoryResetExtrasFn)();
};

void registerRoutes(WebServer& server, const Context& ctx);

} // namespace WebConfigApi
