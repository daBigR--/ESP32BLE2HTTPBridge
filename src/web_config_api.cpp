#include "web_config_api.h"

#include <algorithm>

namespace WebConfigApi {

void registerRoutes(WebServer& server, const Context& ctx) {
  server.on("/config", HTTP_GET, [ctx, &server]() {
    server.send(200, "application/json", ConfigStore::configJson(*ctx.wifiNetworks, *ctx.baseUrl, *ctx.keyMappings));
  });

  server.on("/config/seturl", HTTP_GET, [ctx, &server]() {
    if (!server.hasArg("url")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing url\"}");
      return;
    }
    *ctx.baseUrl = server.arg("url");
    ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrl, *ctx.keyMappings);
    if (ctx.logFn) {
      ctx.logFn(String("Base URL: ") + *ctx.baseUrl);
    }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/config/addwifi", HTTP_GET, [ctx, &server]() {
    if (!server.hasArg("ssid")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ssid\"}");
      return;
    }
    const String ssid = server.arg("ssid");
    const String pwd = server.hasArg("pwd") ? server.arg("pwd") : "";
    for (auto& net : *ctx.wifiNetworks) {
      if (net.ssid == ssid) {
        net.password = pwd;
        ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrl, *ctx.keyMappings);
        if (ctx.logFn) { ctx.logFn(String("WiFi updated: ") + ssid); }
        server.send(200, "application/json", "{\"ok\":true}");
        return;
      }
    }
    ctx.wifiNetworks->push_back({ssid, pwd});
    ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrl, *ctx.keyMappings);
    if (ctx.logFn) { ctx.logFn(String("WiFi added: ") + ssid); }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/config/delwifi", HTTP_GET, [ctx, &server]() {
    if (!server.hasArg("ssid")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ssid\"}");
      return;
    }
    const String ssid = server.arg("ssid");
    size_t before = ctx.wifiNetworks->size();
    ctx.wifiNetworks->erase(
      std::remove_if(ctx.wifiNetworks->begin(), ctx.wifiNetworks->end(),
        [&ssid](const WifiCredential& n) { return n.ssid == ssid; }),
      ctx.wifiNetworks->end()
    );
    if (ctx.wifiNetworks->size() == before) {
      server.send(200, "application/json", "{\"ok\":false,\"error\":\"ssid not found\"}");
      return;
    }
    ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrl, *ctx.keyMappings);
    if (ctx.logFn) { ctx.logFn(String("WiFi removed: ") + ssid); }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/config/setmapping", HTTP_GET, [ctx, &server]() {
    if (!server.hasArg("key") || !server.hasArg("path")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing key or path\"}");
      return;
    }

    uint8_t code = (uint8_t)strtol(server.arg("key").c_str(), nullptr, 16);
    String path = server.arg("path");
    for (auto& m : *ctx.keyMappings) {
      if (m.keyCode == code) {
        m.path = path;
        ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrl, *ctx.keyMappings);
        server.send(200, "application/json", "{\"ok\":true}");
        return;
      }
    }

    ctx.keyMappings->push_back({code, path});
    ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrl, *ctx.keyMappings);
    if (ctx.logFn) {
      ctx.logFn(String("Map 0x") + String(code, HEX) + String(" -> ") + path);
    }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/config/delmapping", HTTP_GET, [ctx, &server]() {
    if (!server.hasArg("key")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing key\"}");
      return;
    }

    uint8_t code = (uint8_t)strtol(server.arg("key").c_str(), nullptr, 16);
    size_t before = ctx.keyMappings->size();
    ctx.keyMappings->erase(
      std::remove_if(ctx.keyMappings->begin(), ctx.keyMappings->end(), [code](const KeyMapping& m) { return m.keyCode == code; }),
      ctx.keyMappings->end()
    );

    if (ctx.keyMappings->size() == before) {
      server.send(200, "application/json", "{\"ok\":false,\"error\":\"key not found\"}");
      return;
    }

    ConfigStore::save(*ctx.wifiNetworks, *ctx.baseUrl, *ctx.keyMappings);
    if (ctx.logFn) {
      ctx.logFn(String("Del map 0x") + String(code, HEX));
    }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/reboot", HTTP_GET, [&server]() {
    server.send(200, "application/json", "{\"ok\":true}");
    delay(300);
    ESP.restart();
  });

  server.on("/factory-reset", HTTP_GET, [ctx, &server]() {
    ConfigStore::clearAll();
    *ctx.baseUrl = "";
    ctx.wifiNetworks->clear();
    ctx.keyMappings->clear();
    if (ctx.factoryResetExtrasFn) {
      ctx.factoryResetExtrasFn();
    }
    server.send(200, "application/json", "{\"ok\":true}");
    delay(300);
    ESP.restart();
  });
}

} // namespace WebConfigApi
