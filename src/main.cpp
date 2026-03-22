#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WebServer.h>
#include <algorithm>

#include <deque>
#include <vector>

#include "ble_keyboard.h"
#include "config_store.h"
#include "http_bridge.h"
#include "web_page.h"

#define HID_SERVICE_UUID      "1812"
#define HID_INPUT_REPORT_UUID "2A4D"

static const char* AP_SSID = "ESP32-Keyboard-Hub";
static const char* AP_PASSWORD = "12345678";

struct DiscoveredDevice {
    String name;
    String address;
    int rssi;
    bool bonded;
    bool seen;
    bool pairableNow;
};

WebServer server(80);

static std::vector<DiscoveredDevice> gDevices;
static std::deque<String> gKeyLog;
static const size_t MAX_KEY_LOG = 40;

static String gBaseUrl = "";
static String gWifiSsid = "";
static String gWifiPassword = "";
static std::vector<KeyMapping> gKeyMappings;
static bool gConfigMode = true;

static const uint8_t CONFIG_BUTTON_PIN = D9;
static const unsigned long CONFIG_BUTTON_HOLD_MS = 800;

void addKeyLog(const String& line);
bool isConfigButtonHeldOnBoot();
String mappedPathForKey(uint8_t keyCode);
void handleConfigGet();
void handleSetUrl();
void handleSetWifi();
void handleSetMapping();
void handleDelMapping();
void handleReboot();
void handleFactoryReset();

String jsonEscape(const String& in) {
    String out = "";
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        const char c = in[i];
        if (c == '\\' || c == '"') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            out += c;
        }
    }
    return out;
}

void addKeyLog(const String& line) {
    if (line.length() == 0) {
        return;
    }
    gKeyLog.push_back(line);
    while (gKeyLog.size() > MAX_KEY_LOG) {
        gKeyLog.pop_front();
    }
    Serial.println(line);
}

bool isConfigButtonHeldOnBoot() {
  pinMode(CONFIG_BUTTON_PIN, INPUT);
  if (digitalRead(CONFIG_BUTTON_PIN) == HIGH) {
    unsigned long started = millis();
    while (millis() - started < CONFIG_BUTTON_HOLD_MS) {
      if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
        return false; // released before hold time
      }
      delay(10);
    }
    return true; // stayed HIGH for full hold period
  }
  return false;
}

String mappedPathForKey(uint8_t keyCode) {
  for (const KeyMapping& m : gKeyMappings) {
    if (m.keyCode == keyCode) {
      return m.path;
    }
  }
  return "";
}

String currentBaseUrl() {
  return gBaseUrl;
}

void performScan() {
    gDevices.clear();

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setInterval(80);
    scan->setWindow(40);
    scan->setActiveScan(true);
    scan->clearResults();

    NimBLEScanResults results = scan->start(4, false);
    for (int i = 0; i < results.getCount(); i++) {
      NimBLEAdvertisedDevice d = results.getDevice(i);
      if (!d.haveName()) {
            continue;
        }

      String address = String(d.getAddress().toString().c_str());
      bool bonded = BLEKeyboard::isBondedAddress(address);
      bool pairableNow = !bonded && BLEKeyboard::isAdvertisedAsPairingMode(d);

      if (!bonded && !pairableNow) {
        continue;
      }

        DiscoveredDevice item;
        item.name = String(d.getName().c_str());
      item.address = address;
        item.rssi = d.getRSSI();
      item.bonded = bonded;
        item.seen = true;
      item.pairableNow = pairableNow;
        gDevices.push_back(item);
    }

      const int bondCount = NimBLEDevice::getNumBonds();
      for (int i = 0; i < bondCount; i++) {
        String bondedAddr = String(NimBLEDevice::getBondedAddress(i).toString().c_str());
        bool alreadyListed = false;
        for (DiscoveredDevice& device : gDevices) {
          if (device.address.equalsIgnoreCase(bondedAddr)) {
            device.bonded = true;
            alreadyListed = true;
            break;
          }
        }

        if (alreadyListed) {
          continue;
        }

        DiscoveredDevice item;
        item.address = bondedAddr;
        item.rssi = -127;
        item.bonded = true;
        item.seen = false;
        item.pairableNow = false;
        if (bondedAddr.equalsIgnoreCase(BLEKeyboard::preferredBondedAddress()) && BLEKeyboard::preferredBondedName().length() > 0) {
          item.name = BLEKeyboard::preferredBondedName();
        } else {
          item.name = "(bonded device)";
        }
        gDevices.push_back(item);
      }
}

String devicesJson() {
    String out = "[";
    for (size_t i = 0; i < gDevices.size(); i++) {
        if (i > 0) {
            out += ",";
        }
        out += "{\"name\":\"" + jsonEscape(gDevices[i].name) + "\",";
        out += "\"address\":\"" + jsonEscape(gDevices[i].address) + "\",";
        out += "\"rssi\":" + String(gDevices[i].rssi) + ",";
        out += "\"bonded\":";
        out += gDevices[i].bonded ? "true" : "false";
        out += ",\"seen\":";
        out += gDevices[i].seen ? "true" : "false";
        out += ",\"pairableNow\":";
        out += gDevices[i].pairableNow ? "true" : "false";
        out += "}";
    }
    out += "]";
    return out;
}

String keyLogJson() {
    String out = "[";
    size_t index = 0;
    for (const String& line : gKeyLog) {
        if (index++ > 0) {
            out += ",";
        }
        out += "\"" + jsonEscape(line) + "\"";
    }
    out += "]";
    return out;
}

void handleState() {
    String out = "{";
    out += "\"connected\":";
  out += BLEKeyboard::isConnected() ? "true" : "false";
  out += ",\"name\":\"" + jsonEscape(BLEKeyboard::connectedName()) + "\"";
  out += ",\"address\":\"" + jsonEscape(BLEKeyboard::connectedAddress()) + "\"";
  out += ",\"bondedAddress\":\"" + jsonEscape(BLEKeyboard::preferredBondedAddress()) + "\"";
  out += ",\"bondedName\":\"" + jsonEscape(BLEKeyboard::preferredBondedName()) + "\"";
    out += ",\"lastKey\":\"";
  if (BLEKeyboard::lastKeyCode() > 0) {
    if (BLEKeyboard::lastKeyCode() < 0x10) out += "0";
    out += String(BLEKeyboard::lastKeyCode(), HEX);
    }
    out += "\"";
    out += ",\"keys\":" + keyLogJson();
    out += "}";
    server.send(200, "application/json", out);
}

void handleConfigGet() {
  server.send(200, "application/json", ConfigStore::configJson(gWifiSsid, gWifiPassword, gBaseUrl, gKeyMappings, jsonEscape));
}

void handleSetUrl() {
    if (!server.hasArg("url")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing url\"}");
        return;
    }
    gBaseUrl = server.arg("url");
    ConfigStore::save(gWifiSsid, gWifiPassword, gBaseUrl, gKeyMappings);
    addKeyLog(String("Base URL: ") + gBaseUrl);
    server.send(200, "application/json", "{\"ok\":true}");
}

  void handleSetWifi() {
    if (!server.hasArg("ssid")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ssid\"}");
      return;
    }
    gWifiSsid = server.arg("ssid");
    gWifiPassword = server.hasArg("pwd") ? server.arg("pwd") : "";
    ConfigStore::save(gWifiSsid, gWifiPassword, gBaseUrl, gKeyMappings);
    addKeyLog(String("WiFi SSID: ") + gWifiSsid);
    server.send(200, "application/json", "{\"ok\":true}");
  }

void handleSetMapping() {
    if (!server.hasArg("key") || !server.hasArg("path")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing key or path\"}");
        return;
    }
    uint8_t code = (uint8_t)strtol(server.arg("key").c_str(), nullptr, 16);
    String path = server.arg("path");
    for (auto& m : gKeyMappings) {
        if (m.keyCode == code) {
            m.path = path;
        ConfigStore::save(gWifiSsid, gWifiPassword, gBaseUrl, gKeyMappings);
            server.send(200, "application/json", "{\"ok\":true}");
            return;
        }
    }
    gKeyMappings.push_back({code, path});
    ConfigStore::save(gWifiSsid, gWifiPassword, gBaseUrl, gKeyMappings);
    addKeyLog(String("Map 0x") + String(code, HEX) + String(" -> ") + path);
    server.send(200, "application/json", "{\"ok\":true}");
}

void handleDelMapping() {
    if (!server.hasArg("key")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing key\"}");
        return;
    }
    uint8_t code = (uint8_t)strtol(server.arg("key").c_str(), nullptr, 16);
    size_t before = gKeyMappings.size();
    gKeyMappings.erase(
        std::remove_if(gKeyMappings.begin(), gKeyMappings.end(),
            [code](const KeyMapping& m){ return m.keyCode == code; }),
        gKeyMappings.end()
    );
    if (gKeyMappings.size() == before) {
        server.send(200, "application/json", "{\"ok\":false,\"error\":\"key not found\"}");
        return;
    }
    ConfigStore::save(gWifiSsid, gWifiPassword, gBaseUrl, gKeyMappings);
    addKeyLog(String("Del map 0x") + String(code, HEX));
    server.send(200, "application/json", "{\"ok\":true}");
}

void handleReboot() {
    server.send(200, "application/json", "{\"ok\":true}");
    delay(300);
    ESP.restart();
}

void handleFactoryReset() {
    NimBLEDevice::deleteAllBonds();
  ConfigStore::clearAll();
    gBaseUrl = "";
    gWifiSsid = "";
    gWifiPassword = "";
    gKeyMappings.clear();
    BLEKeyboard::clearPreferredBondedDevice();
    server.send(200, "application/json", "{\"ok\":true}");
    delay(300);
    ESP.restart();
}

void handleScan() {
    performScan();
    String out = "{\"devices\":" + devicesJson() + "}";
    server.send(200, "application/json", out);
}

void handleConnect() {
    if (!server.hasArg("addr")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing addr\"}");
        return;
    }
    const String addr = server.arg("addr");
    const String name = server.hasArg("name") ? server.arg("name") : "";
    const bool ok = BLEKeyboard::connectToKeyboard(addr, name);
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"connect failed or device not bonded\"}");
  }

  void handlePair() {
    if (!server.hasArg("addr")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing addr\"}");
      return;
    }
    const String addr = server.arg("addr");
    const String name = server.hasArg("name") ? server.arg("name") : "";
    const bool ok = BLEKeyboard::pairKeyboard(addr, name);
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"pair failed (device may not accept new bonding now)\"}");
}

  void handleUnpair() {
    if (!server.hasArg("addr")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing addr\"}");
      return;
    }
    const String addr = server.arg("addr");
    const bool ok = BLEKeyboard::unpairKeyboard(addr);
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"unpair failed\"}");
  }

void setup() {
    Serial.begin(115200);

    // Check boot button FIRST, before any slow init, so the user doesn't
    // have to hold it for longer than CONFIG_BUTTON_HOLD_MS.
    bool forceConfigMode = isConfigButtonHeldOnBoot();

    delay(800);

    NimBLEDevice::init("ESP32-KB-Receiver");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_DEFAULT);
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    HttpBridge::begin(addKeyLog, currentBaseUrl, mappedPathForKey);
    BLEKeyboard::begin(addKeyLog, HttpBridge::onKeyPress);

    ConfigStore::load(gWifiSsid, gWifiPassword, gBaseUrl, gKeyMappings);
    addKeyLog(
      String("Config: wifi=") + (gWifiSsid.length() ? gWifiSsid : "(none)") +
      String(" url=") + (gBaseUrl.length() ? gBaseUrl : "(none)") +
      String(" maps=") + String(gKeyMappings.size())
    );
    BLEKeyboard::refreshPreferredBondedDevice();

    bool runConfigReady = ConfigStore::hasValidRunConfig(gWifiSsid, gBaseUrl, gKeyMappings, BLEKeyboard::preferredBondedAddress());
    gConfigMode = forceConfigMode || !runConfigReady;

    if (gConfigMode) {
      WiFi.mode(WIFI_AP);
      WiFi.softAP(AP_SSID, AP_PASSWORD);

      server.on("/", HTTP_GET, []() { server.send(200, "text/html", PAGE); });
      server.on("/scan", HTTP_GET, handleScan);
      server.on("/pair", HTTP_GET, handlePair);
      server.on("/connect", HTTP_GET, handleConnect);
      server.on("/unpair", HTTP_GET, handleUnpair);
      server.on("/disconnect", HTTP_GET, []() {
          BLEKeyboard::disconnectKeyboard();
          server.send(200, "application/json", "{\"ok\":true}");
      });
      server.on("/state", HTTP_GET, handleState);
      server.on("/config", HTTP_GET, handleConfigGet);
      server.on("/config/seturl", HTTP_GET, handleSetUrl);
      server.on("/config/setwifi", HTTP_GET, handleSetWifi);
      server.on("/config/setmapping", HTTP_GET, handleSetMapping);
      server.on("/config/delmapping", HTTP_GET, handleDelMapping);
      server.on("/reboot", HTTP_GET, handleReboot);
      server.on("/factory-reset", HTTP_GET, handleFactoryReset);
      server.begin();

      Serial.println("\nESP32 BLE Keyboard Hub - CONFIG mode");
      Serial.print("Open GUI at: http://");
      Serial.println(WiFi.softAPIP());
      addKeyLog("GUI ready");
    } else {
      WiFi.mode(WIFI_STA);
      WiFi.begin(gWifiSsid.c_str(), gWifiPassword.c_str());
      Serial.println("\nESP32 BLE Keyboard Hub - RUN mode");
      addKeyLog(String("RUN mode WiFi SSID: ") + gWifiSsid);
      addKeyLog("RUN mode: waiting for keyboard and mapped keypresses");
    }
}

void loop() {
    if (gConfigMode) {
      server.handleClient();
    }
    BLEKeyboard::syncConnectionState();
    BLEKeyboard::maybeAutoConnectBondedKeyboard();
    if (!gConfigMode) {
      HttpBridge::processPendingKeys();
    }
    delay(10);
}