#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <algorithm>

#include <vector>

#include "ble_keyboard.h"
#include "config_store.h"
#include "http_bridge.h"
#include "json_util.h"
#include "key_log.h"
#include "web_config_api.h"
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

static String gBaseUrl = "";
static std::vector<WifiCredential> gWifiNetworks;
static WiFiMulti gWifiMulti;
static std::vector<KeyMapping> gKeyMappings;
static bool gConfigMode = true;

static const uint8_t CONFIG_BUTTON_PIN = D9;
static const unsigned long CONFIG_BUTTON_HOLD_MS = 800;

bool isConfigButtonHeldOnBoot();
String mappedPathForKey(uint8_t keyCode);
void handleFactoryResetExtras();

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
    out += "{\"name\":\"" + JsonUtil::escape(gDevices[i].name) + "\",";
    out += "\"address\":\"" + JsonUtil::escape(gDevices[i].address) + "\",";
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

void handleState() {
    String out = "{";
    out += "\"connected\":";
  out += BLEKeyboard::isConnected() ? "true" : "false";
  out += ",\"name\":\"" + JsonUtil::escape(BLEKeyboard::connectedName()) + "\"";
  out += ",\"address\":\"" + JsonUtil::escape(BLEKeyboard::connectedAddress()) + "\"";
  out += ",\"bondedAddress\":\"" + JsonUtil::escape(BLEKeyboard::preferredBondedAddress()) + "\"";
  out += ",\"bondedName\":\"" + JsonUtil::escape(BLEKeyboard::preferredBondedName()) + "\"";
    out += ",\"lastKey\":\"";
  if (BLEKeyboard::lastKeyCode() > 0) {
    if (BLEKeyboard::lastKeyCode() < 0x10) out += "0";
    out += String(BLEKeyboard::lastKeyCode(), HEX);
    }
    out += "\"";
  out += ",\"keys\":" + KeyLog::toJson();
    out += "}";
    server.send(200, "application/json", out);
}

void handleFactoryResetExtras() {
    NimBLEDevice::deleteAllBonds();
    BLEKeyboard::clearPreferredBondedDevice();
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
    HttpBridge::begin(KeyLog::add, currentBaseUrl, mappedPathForKey);
    BLEKeyboard::begin(KeyLog::add, HttpBridge::onKeyPress);

    ConfigStore::load(gWifiNetworks, gBaseUrl, gKeyMappings);
    KeyLog::add(
      String("Config: wifi=") + String(gWifiNetworks.size()) + String(" net(s)") +
      String(" url=") + (gBaseUrl.length() ? gBaseUrl : "(none)") +
      String(" maps=") + String(gKeyMappings.size())
    );
    BLEKeyboard::refreshPreferredBondedDevice();

    bool runConfigReady = ConfigStore::hasValidRunConfig(gWifiNetworks, gBaseUrl, gKeyMappings, BLEKeyboard::preferredBondedAddress());
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
      WebConfigApi::Context cfgCtx = {
        &gWifiNetworks,
        &gBaseUrl,
        &gKeyMappings,
        KeyLog::add,
        handleFactoryResetExtras
      };
      WebConfigApi::registerRoutes(server, cfgCtx);
      server.begin();

      Serial.println("\nESP32 BLE Keyboard Hub - CONFIG mode");
      Serial.print("Open GUI at: http://");
      Serial.println(WiFi.softAPIP());
      KeyLog::add("GUI ready");
    } else {
      WiFi.mode(WIFI_STA);
      for (const auto& net : gWifiNetworks) {
        gWifiMulti.addAP(net.ssid.c_str(), net.password.c_str());
      }
      gWifiMulti.run(10000);
      Serial.println("\nESP32 BLE Keyboard Hub - RUN mode");
      KeyLog::add(String("RUN mode: ") + String(gWifiNetworks.size()) + " WiFi network(s) configured");
      KeyLog::add("RUN mode: waiting for keyboard and mapped keypresses");
    }
}

void loop() {
    if (gConfigMode) {
      server.handleClient();
    }
    BLEKeyboard::syncConnectionState();
    BLEKeyboard::maybeAutoConnectBondedKeyboard();
    if (!gConfigMode) {
      static unsigned long lastWifiCheck = 0;
      if (WiFi.status() != WL_CONNECTED && millis() - lastWifiCheck > 5000) {
        gWifiMulti.run(500);
        lastWifiCheck = millis();
      }
      HttpBridge::processPendingKeys();
    }
    delay(10);
}