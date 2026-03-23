#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <algorithm>

#include <vector>

#include "ble_keyboard.h"
#include "ble_scanner.h"
#include "config_store.h"
#include "http_bridge.h"
#include "key_log.h"
#include "web_ble_api.h"
#include "web_config_api.h"
#include "web_page.h"

#define HID_SERVICE_UUID      "1812"
#define HID_INPUT_REPORT_UUID "2A4D"

static const char* AP_SSID = "ESP32-Keyboard-Hub";
static const char* AP_PASSWORD = "12345678";

WebServer server(80);

static String gBaseUrl = "";
static std::vector<WifiCredential> gWifiNetworks;
static WiFiMulti gWifiMulti;
static std::vector<KeyMapping> gKeyMappings;
static bool gConfigMode = true;

static const uint8_t CONFIG_BUTTON_PIN = D9;
static const unsigned long CONFIG_BUTTON_HOLD_MS = 800;
static const uint8_t BLE_LED_PIN = D1;
static const uint8_t HTTP_LED_PIN = D3;
static const unsigned long CONFIG_ALT_HALF_CYCLE_MS = 500;
static const unsigned long BLE_KEY_BLINK_OFF_MS = 80;
static const unsigned long BLE_KEY_BLINK_ON_MS = 80;
static const uint8_t BLE_KEY_BLINK_COUNT = 2;
static const unsigned long HTTP_200_PULSE_MS = 180;

// Shared between main-loop/BLE callbacks (writers) and the LED task (reader).
// 32-bit aligned writes on Xtensa LX7 are atomic; volatile prevents
// the compiler from caching stale values across task contexts.
static volatile unsigned long gBleLedBlinkStartMs = 0;
static volatile unsigned long gBleLedBlinkEndMs = 0;
static volatile unsigned long gHttpLedForceOffUntilMs = 0;

bool isConfigButtonHeldOnBoot();
String mappedPathForKey(uint8_t keyCode);
void handleFactoryResetExtras();
void onBleKeyPress(uint8_t keyCode);
void onHttpGetStart();
void onHttpGetResult(int statusCode);
void updateStatusLeds();

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

void handleFactoryResetExtras() {
    NimBLEDevice::deleteAllBonds();
    BLEKeyboard::clearPreferredBondedDevice();
}

void onBleKeyPress(uint8_t keyCode) {
  // Record blink window; the LED task will drive the pin.
  unsigned long now = millis();
  gBleLedBlinkStartMs = now;
  gBleLedBlinkEndMs = now + (BLE_KEY_BLINK_COUNT * (BLE_KEY_BLINK_OFF_MS + BLE_KEY_BLINK_ON_MS));
  HttpBridge::onKeyPress(keyCode);
}

void onHttpGetStart() {
  // Intentionally no LED action on GET start.
}

void onHttpGetResult(int statusCode) {
  if (statusCode == 200) {
    unsigned long until = millis() + HTTP_200_PULSE_MS;
    if (until > gHttpLedForceOffUntilMs) {
      gHttpLedForceOffUntilMs = until;
    }
  }
}

void updateStatusLeds() {
  unsigned long now = millis();

  if (gConfigMode) {
    bool firstHalf = ((now / CONFIG_ALT_HALF_CYCLE_MS) % 2) == 0;
    digitalWrite(BLE_LED_PIN, firstHalf ? HIGH : LOW);
    digitalWrite(HTTP_LED_PIN, firstHalf ? LOW : HIGH);
    return;
  }

  bool bleConnected = BLEKeyboard::isConnected();
  bool bleLedOn = bleConnected;
  if (bleLedOn && now < gBleLedBlinkEndMs) {
    unsigned long elapsed = now - gBleLedBlinkStartMs;
    unsigned long cycle = BLE_KEY_BLINK_OFF_MS + BLE_KEY_BLINK_ON_MS;
    unsigned long phase = elapsed % cycle;
    bleLedOn = phase >= BLE_KEY_BLINK_OFF_MS;
  }
  bool wifiConnected = WiFi.status() == WL_CONNECTED;
  bool httpLedOn = wifiConnected;
  if (httpLedOn && now < gHttpLedForceOffUntilMs) {
    httpLedOn = false;
  }

  digitalWrite(BLE_LED_PIN, bleLedOn ? HIGH : LOW);
  digitalWrite(HTTP_LED_PIN, httpLedOn ? HIGH : LOW);
}

// Dedicated LED task — pinned to Core 1 (APP_CPU) so BLE/WiFi stacks on
// Core 0 are completely undisturbed.  Runs every 5 ms with vTaskDelayUntil
// for jitter-free timing regardless of main-loop blocking.
static void ledTask(void* /*pvParameters*/) {
  const TickType_t period = pdMS_TO_TICKS(5);
  TickType_t xLastWakeTime = xTaskGetTickCount();
  for (;;) {
    updateStatusLeds();
    vTaskDelayUntil(&xLastWakeTime, period);
  }
}

void setup() {
    Serial.begin(115200);

  pinMode(BLE_LED_PIN, OUTPUT);
  pinMode(HTTP_LED_PIN, OUTPUT);
  digitalWrite(BLE_LED_PIN, LOW);
  digitalWrite(HTTP_LED_PIN, LOW);

  // Start LED task before any blocking init so the config-mode alternate
  // blink is visible as soon as setup() enters its slow initialisation path.
  xTaskCreatePinnedToCore(ledTask, "led", 2048, nullptr, 1, nullptr, 1);

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
    HttpBridge::setGetCallbacks(onHttpGetStart, onHttpGetResult);
    BLEKeyboard::begin(KeyLog::add, onBleKeyPress);

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
      WebBleApi::registerRoutes(server);
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