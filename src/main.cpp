// =============================================================================
// main.cpp — Application entry-point and top-level orchestration
// =============================================================================
//
// What this firmware does
// -----------------------
// This code turns a Seeed XIAO ESP32-S3 into a BLE-to-HTTP bridge:
//
//   BLE side:  The device operates as a BLE *central* (client, not peripheral).
//              It scans for, pairs with, and connects to a BLE HID keyboard.
//              Once connected it subscribes to the keyboard's HID input-report
//              characteristic so every key press arrives as a BLE notification
//              carrying a raw HID usage-page key code.
//
//   HTTP side: A configurable mapping table translates HID key codes into
//              relative URL paths.  When a mapped key is pressed the device
//              fires an HTTP GET to <baseUrl>/<path> — suitable for triggering
//              home-automation webhooks, REST endpoints, or any HTTP consumer.
//
//   Config:    All settings (WiFi credentials, base URL, key mappings, and the
//              preferred bonded keyboard address) are persisted in ESP32 NVS
//              flash via the Arduino Preferences library.  They survive reboots
//              and power cycles and are managed through a browser-based UI.
//
// Operating modes
// ---------------
//   CONFIG MODE — entered when:
//       • No fully-valid run configuration has been stored yet, OR
//       • The user holds the boot button (D9) LOW for ≥ 800 ms on power-on
//         (allows forced reconfiguration even when a valid config is present).
//
//     The device brings up a WiFi SoftAP (SSID "ESP32-Keyboard-Hub") and runs
//     a small HTTP server on port 80, serving a single-page web app that lets
//     the user:
//       - Scan for BLE keyboards advertising in pairing mode and bond them.
//       - Add / remove known WiFi networks (up to 8; WiFiMulti picks the
//         strongest available one when entering RUN mode).
//       - Set the target base URL for HTTP forwarding.
//       - Create, edit, or delete key→path mappings.
//     After saving a valid configuration the user reboots and the device enters
//     RUN mode automatically.
//
//   RUN MODE — entered when ALL of the following conditions are met:
//       • At least one WiFi credential stored
//       • A non-empty base URL stored
//       • At least one key mapping stored
//       • A preferred bonded keyboard address stored
//
//     The device joins a known WiFi network, then auto-connects to the bonded
//     BLE keyboard and begins forwarding mapped key presses as HTTP GETs.
//     WiFi reconnect is retried every 5 s; BLE reconnect every 8 s.
//
// Status LEDs — driven by a dedicated FreeRTOS task on Core 1
// -----------------------------------------------------------
//   D1 (keyboard indicator):
//       OFF                  — keyboard not connected
//       Steady ON            — keyboard connected and ready
//       Double blink-off     — key press received (80 ms off / 80 ms on × 2)
//
//   D3 (network indicator):
//       OFF                  — not connected to a WiFi AP
//       Steady ON            — connected to a WiFi AP
//       Single blink-off     — HTTP GET returned 200 OK (180 ms)
//
//   CONFIG MODE (both LEDs):
//       Anti-phase 1 Hz alternation — indicates the device needs configuration.
// =============================================================================

#include <Arduino.h>        // Core Arduino types: String, millis, pinMode, delay…
#include <NimBLEDevice.h>   // NimBLE stack — lighter than the bundled ESP32 BLE
                            // library; faster scan times and lower memory usage.
#include <WiFi.h>           // ESP32 WiFi driver (SoftAP + STA modes)
#include <WiFiMulti.h>      // Multi-SSID helper: stores several networks and
                            // automatically selects the strongest available one.
#include <WebServer.h>      // Synchronous HTTP server used for the config-mode UI
#include <algorithm>        // std::remove_if — used in web config route handlers

#include <vector>           // std::vector for WiFi credentials and key mappings

// Project modules — each is an isolated compilation unit with its own namespace.
#include "ble_keyboard.h"   // BLE central: pairing, connection, HID subscription
#include "ble_scanner.h"    // One-shot BLE scan → JSON list of visible/bonded devices
#include "config_store.h"   // NVS-backed persistence for all runtime configuration
#include "http_bridge.h"    // Key-press queue and non-blocking HTTP GET dispatch
#include "key_log.h"        // Rolling in-memory event log with Serial echo
#include "web_ble_api.h"    // HTTP routes that expose BLE operations to the web UI
#include "web_config_api.h" // HTTP routes that expose configuration to the web UI
#include "web_page.h"       // Const C-string containing the single-page web UI HTML

// Standard Bluetooth SIG UUIDs for the HID profile.
// 0x1812 = Human Interface Device service.
// 0x2A4D = HID Report characteristic (carries boot-protocol + report-protocol
//          input reports; the keyboard sends key codes here as notifications).
#define HID_SERVICE_UUID      "1812"
#define HID_INPUT_REPORT_UUID "2A4D"

// ---------------------------------------------------------------------------
// SoftAP credentials — used in CONFIG mode only.
// The device creates this network so a phone/laptop can connect to it and
// access the configuration web UI at http://192.168.4.1.
// ---------------------------------------------------------------------------
static const char* AP_SSID     = "ESP32-Keyboard-Hub";
static const char* AP_PASSWORD = "12345678";

// The HTTP server instance.  In CONFIG mode it serves the web UI and all
// REST API routes on port 80.  Not used at all in RUN mode.
WebServer server(80);

// ---------------------------------------------------------------------------
// Runtime configuration — loaded from NVS on boot, updated via the web UI.
// ---------------------------------------------------------------------------

// Target base URLs for HTTP forwarding.  HTTP GETs are always sent to
// gBaseUrls[gSelectedUrlIndex].  The list is managed via the web UI;
// the active selection is cycled with the physical button at runtime.
static std::vector<String> gBaseUrls;
static uint8_t             gSelectedUrlIndex = 0;

// List of known WiFi networks (SSID + password pairs).  WiFiMulti stores all
// of them and picks the one with the strongest signal at connect time.
static std::vector<WifiCredential> gWifiNetworks;
static WiFiMulti gWifiMulti;

// Table mapping HID key codes to relative URL paths.
// e.g. keyCode=0x28 (Enter) → "/door/open"
static std::vector<KeyMapping> gKeyMappings;

// Whether the device is currently in CONFIG mode (true) or RUN mode (false).
// Starts as true; set to false in setup() once a valid config is confirmed.
static bool gConfigMode = true;

// ---------------------------------------------------------------------------
// Hardware pin assignments
// ---------------------------------------------------------------------------
// D9 is the XIAO's built-in BOOT button, conveniently doubles as a config
// entry trigger.  D1 and D3 are onboard LEDs (active HIGH).
static const uint8_t CONFIG_BUTTON_PIN = D9;

// ---------------------------------------------------------------------------
// Config-mode entry: button hold threshold
// ---------------------------------------------------------------------------
// The user must hold D9 LOW for this many ms at boot to force CONFIG mode.
// 800 ms is long enough to be intentional but short enough to be comfortable.
static const unsigned long CONFIG_BUTTON_HOLD_MS = 800;

// ---------------------------------------------------------------------------
// LED pin assignments
// ---------------------------------------------------------------------------
static const uint8_t BLE_LED_PIN  = D1; // Keyboard / BLE connection indicator
static const uint8_t HTTP_LED_PIN = D3; // Network / HTTP indicator

// ---------------------------------------------------------------------------
// LED timing constants
// ---------------------------------------------------------------------------
// CONFIG mode: both LEDs alternate at 1 Hz (500 ms per half-cycle).
static const unsigned long CONFIG_ALT_HALF_CYCLE_MS = 500;

// D1 key-press blink: the LED goes off for BLE_KEY_BLINK_OFF_MS, then on
// for BLE_KEY_BLINK_ON_MS, repeated BLE_KEY_BLINK_COUNT times.
// 80/80 × 2 gives a soft, clearly visible double-dip while the keyboard stays
// mostly lit so the steady-on state is not confused with "off".
static const unsigned long BLE_KEY_BLINK_OFF_MS = 80;
static const unsigned long BLE_KEY_BLINK_ON_MS  = 80;
static const uint8_t       BLE_KEY_BLINK_COUNT  = 2;

// D3 HTTP-200 blink: the LED goes off for this many ms on a successful
// response, then returns to steady-on.  Long enough to be conspicuous without
// feeling like a failure.
static const unsigned long HTTP_200_PULSE_MS = 180;

// URL-selection burst blink: N blinks on D3 where N = selectedUrlIndex + 1.
// Each blink is URL_BLINK_OFF_MS off then URL_BLINK_ON_MS on.
static const unsigned long URL_BLINK_OFF_MS  = 150;
static const unsigned long URL_BLINK_ON_MS   = 150;
static const unsigned long URL_BLINK_PERIOD  = URL_BLINK_OFF_MS + URL_BLINK_ON_MS;
static const unsigned long URL_SAVE_PULSE_MS = 600;

// Button timing for URL cycling: anything shorter than DEBOUNCE is noise;
// anything >= LONG_PRESS triggers a save.
static const unsigned long URL_BTN_DEBOUNCE_MS   = 50;
static const unsigned long URL_BTN_LONG_PRESS_MS = 800;

// ---------------------------------------------------------------------------
// LED state shared between event callbacks and the LED task
// ---------------------------------------------------------------------------
// These variables are written by code running on Core 0 (BLE / main loop)
// and read by the LED task on Core 1.  On the Xtensa LX7 all 32-bit aligned
// writes are atomic at the instruction level, so no mutex is required.
// The `volatile` qualifier prevents the compiler from caching these values in
// a register across a context switch, ensuring the LED task always sees the
// most recent value written by any other task or callback.

// D1 blink window: the LED task performs the multi-blink pattern between
// gBleLedBlinkStartMs and gBleLedBlinkEndMs; outside that window D1 simply
// reflects the BLE connection state (on = connected).
static volatile unsigned long gBleLedBlinkStartMs    = 0;
static volatile unsigned long gBleLedBlinkEndMs      = 0;

// D3 forced-off window: the LED task keeps D3 LOW until this timestamp,
// after which it returns to the normal WiFi-connected steady-on state.
static volatile unsigned long gHttpLedForceOffUntilMs = 0;

// D3 URL-select burst blink: gD3BurstCount blinks starting at gD3BurstStartMs.
static volatile uint8_t       gD3BurstCount   = 0;
static volatile unsigned long gD3BurstStartMs = 0;

// Button state for runtime URL cycling (RUN mode only, accessed from loop only).
static bool          gBtnHeld    = false;
static unsigned long gBtnPressMs = 0;

// ---------------------------------------------------------------------------
// Forward declarations (implementations follow in this file)
// ---------------------------------------------------------------------------
bool   isConfigButtonHeldOnBoot();
String mappedPathForKey(uint8_t keyCode);
void   handleFactoryResetExtras();
void   onBleKeyPress(uint8_t keyCode);
void   onHttpGetStart();
void   onHttpGetResult(int statusCode);
void   updateStatusLeds();
void   cycleUrl();
void   saveSelectedUrl();
void   handleButton();

// ---------------------------------------------------------------------------
// isConfigButtonHeldOnBoot
// ---------------------------------------------------------------------------
// Checks, right at startup, whether the user is holding the config button
// in order to force CONFIG mode.
//
// Design notes:
//   • Called *before* any other slow initialisation so the window the user
//     must hold the button is exactly CONFIG_BUTTON_HOLD_MS, not longer.
//   • Uses a polling loop rather than an interrupt because this only runs once
//     during boot and the simplicity outweighs the marginal power cost.
//   • Returns false immediately if the pin is already HIGH (button not pressed)
//     so normal RUN-mode boots have essentially zero extra delay.
bool isConfigButtonHeldOnBoot() {
  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
    unsigned long started = millis();
    while (millis() - started < CONFIG_BUTTON_HOLD_MS) {
      if (digitalRead(CONFIG_BUTTON_PIN) == HIGH) {
        return false; // released before the hold time elapsed — not intentional
      }
      delay(10);
    }
    return true; // pin stayed LOW for the full hold period — force config mode
  }
  return false; // button was not pressed at all
}

// ---------------------------------------------------------------------------
// mappedPathForKey
// ---------------------------------------------------------------------------
// Looks up the URL path configured for a given HID key code.
// Returns an empty string if the key has no mapping, which HttpBridge
// interprets as "do nothing" and silently drops the key event.
String mappedPathForKey(uint8_t keyCode) {
  for (const KeyMapping& m : gKeyMappings) {
    if (m.keyCode == keyCode) {
      return m.path;
    }
  }
  return "";
}

// ---------------------------------------------------------------------------
// currentBaseUrl
// ---------------------------------------------------------------------------
// Thin accessor passed as a function pointer to HttpBridge::begin().
// Using a pointer to this function (instead of passing gBaseUrls directly)
// means HttpBridge always reads the *current* value even if the web UI or
// the button handler updates gSelectedUrlIndex after initialisation.
String currentBaseUrl() {
  if (gBaseUrls.empty() || gSelectedUrlIndex >= (uint8_t)gBaseUrls.size()) return "";
  return gBaseUrls[gSelectedUrlIndex];
}

// ---------------------------------------------------------------------------
// handleFactoryResetExtras
// ---------------------------------------------------------------------------
// Called by WebConfigApi during a factory-reset request AFTER it has already
// cleared NVS config.  This hook handles the BLE-side teardown:
//   1. deleteAllBonds() removes every stored BLE bond from NimBLE's NVS
//      partition so the next pairing starts with a clean slate.
//   2. clearPreferredBondedDevice() resets the in-RAM preferred address so
//      the auto-connect loop does not try to reconnect a now-deleted bond
//      before the imminent reboot.
void handleFactoryResetExtras() {
    NimBLEDevice::deleteAllBonds();
    BLEKeyboard::clearPreferredBondedDevice();
}

// ---------------------------------------------------------------------------
// cycleUrl — advance to the next base URL in the list (short button press)
// ---------------------------------------------------------------------------
void cycleUrl() {
  if (gBaseUrls.empty()) return;
  gSelectedUrlIndex = (uint8_t)((gSelectedUrlIndex + 1) % gBaseUrls.size());
  KeyLog::add(String("URL #") + String(gSelectedUrlIndex + 1) + ": " + currentBaseUrl());
  gD3BurstStartMs = millis();
  gD3BurstCount   = gSelectedUrlIndex + 1;
}

// ---------------------------------------------------------------------------
// saveSelectedUrl — persist the current URL selection to NVS (long press)
// ---------------------------------------------------------------------------
void saveSelectedUrl() {
  ConfigStore::saveSelectedUrlIndex(gSelectedUrlIndex);
  KeyLog::add(String("URL #") + String(gSelectedUrlIndex + 1) + " saved");
  unsigned long until = millis() + URL_SAVE_PULSE_MS;
  if (until > gHttpLedForceOffUntilMs) {
    gHttpLedForceOffUntilMs = until;
  }
}

// ---------------------------------------------------------------------------
// handleButton — poll the physical button for URL cycling in RUN mode
// ---------------------------------------------------------------------------
void handleButton() {
  bool pressed = (digitalRead(CONFIG_BUTTON_PIN) == LOW);
  unsigned long now = millis();
  if (pressed && !gBtnHeld) {
    gBtnHeld    = true;
    gBtnPressMs = now;
  } else if (!pressed && gBtnHeld) {
    gBtnHeld = false;
    unsigned long held = now - gBtnPressMs;
    if (held >= URL_BTN_LONG_PRESS_MS) {
      saveSelectedUrl();
    } else if (held >= URL_BTN_DEBOUNCE_MS) {
      cycleUrl();
    }
  }
}

// ---------------------------------------------------------------------------
// onBleKeyPress — callback registered with BLEKeyboard
// ---------------------------------------------------------------------------
// Invoked from the BLE notification path (Core 0) every time a key press
// is received from the connected HID keyboard.
//
// It does two things:
//   1. Arms the D1 double-blink window so the LED task will perform the
//      blink-off animation on its next tick, independently of how long the
//      subsequent HTTP request takes.  The window covers exactly
//      BLE_KEY_BLINK_COUNT full off/on cycles.
//
//   2. Enqueues the key code in HttpBridge for deferred HTTP dispatch.
//      HttpBridge::onKeyPress() only queues the code; the actual HTTP GET
//      happens in processPendingKeys() on the next loop() iteration, keeping
//      the BLE callback fast and non-blocking.
void onBleKeyPress(uint8_t keyCode) {
  // Record blink window; the LED task will drive the pin.
  unsigned long now = millis();
  gBleLedBlinkStartMs = now;
  gBleLedBlinkEndMs   = now + (BLE_KEY_BLINK_COUNT * (BLE_KEY_BLINK_OFF_MS + BLE_KEY_BLINK_ON_MS));
  HttpBridge::onKeyPress(keyCode);
}

// ---------------------------------------------------------------------------
// onHttpGetStart — callback invoked just before each HTTP GET is sent
// ---------------------------------------------------------------------------
// Intentionally left empty: we do not want any visual change at the start
// of a request so there is no ambiguity about whether D3 blinking means
// "sending" or "received 200".  Only a successful 200 response triggers the
// D3 blink-off (see onHttpGetResult below).
void onHttpGetStart() {
  // No LED action on GET start — see design note above.
}

// ---------------------------------------------------------------------------
// onHttpGetResult — callback invoked with the HTTP status code after each GET
// ---------------------------------------------------------------------------
// Arms the D3 forced-off window only when the server responds with HTTP 200.
// Any other status code (404, 500, network error, …) is intentionally silent
// on the LED so the user can distinguish a successful trigger from a failure.
//
// The "if until > current" guard ensures that rapid successive 200 responses
// do not keep shortening the blink window — the furthest deadline wins.
void onHttpGetResult(int statusCode) {
  if (statusCode == 200) {
    unsigned long until = millis() + HTTP_200_PULSE_MS;
    if (until > gHttpLedForceOffUntilMs) {
      gHttpLedForceOffUntilMs = until; // extend or start the blink-off window
    }
  }
}

// ---------------------------------------------------------------------------
// updateStatusLeds — called every 5 ms from the dedicated LED task
// ---------------------------------------------------------------------------
// This function is the single point that drives both status LEDs.  It is
// called exclusively from ledTask() and must remain fast and non-blocking.
//
// CONFIG MODE branch:
//   Both LEDs alternate in anti-phase at 1 Hz (period = 2 × 500 ms).
//   Integer division of millis() by the half-cycle period produces a value
//   that toggles between 0 and 1 every 500 ms.  D1 follows that bit; D3
//   follows its complement.  This is purely time-based — no state variables
//   are needed and it continues correctly even if the function is called
//   irregularly.
//
// RUN MODE — D1 (keyboard):
//   Baseline: ON when BLEKeyboard reports a connection, OFF otherwise.
//   During a key-press blink window (set in onBleKeyPress()) the LED is
//   modulated: we calculate how far we are into the window, take that modulo
//   one blink cycle (off + on), and force the LED OFF during the first
//   BLE_KEY_BLINK_OFF_MS of each cycle.  Outside the window the baseline
//   (connected = on) is restored immediately.
//
// RUN MODE — D3 (network):
//   Baseline: ON when WiFi.status() == WL_CONNECTED, OFF otherwise.
//   During the forced-off window (set in onHttpGetResult()) the LED is held
//   LOW regardless of connection state, producing the "blink off" effect.
//   The 180 ms window is long enough to be clearly visible without flickering.
void updateStatusLeds() {
  unsigned long now = millis();

  // --- CONFIG MODE: alternate both LEDs in anti-phase at 1 Hz ---
  if (gConfigMode) {
    // Integer-divide millis by the half-cycle duration; even quotient = first
    // half, odd quotient = second half.  The two LEDs are complements.
    bool firstHalf = ((now / CONFIG_ALT_HALF_CYCLE_MS) % 2) == 0;
    digitalWrite(BLE_LED_PIN,  firstHalf ? HIGH : LOW);
    digitalWrite(HTTP_LED_PIN, firstHalf ? LOW  : HIGH);
    return;
  }

  // --- RUN MODE: D1 — keyboard connection + key-press blink ---
  bool bleConnected = BLEKeyboard::isConnected();
  bool bleLedOn = bleConnected; // default: on iff connected
  if (bleLedOn && now < gBleLedBlinkEndMs) {
    // We are inside an active blink window.  Compute the phase within one
    // off+on cycle and force the LED off during the OFF portion.
    unsigned long elapsed = now - gBleLedBlinkStartMs;
    unsigned long cycle   = BLE_KEY_BLINK_OFF_MS + BLE_KEY_BLINK_ON_MS;
    unsigned long phase   = elapsed % cycle;   // 0 … cycle-1
    bleLedOn = phase >= BLE_KEY_BLINK_OFF_MS;  // OFF in [0, off_ms), ON in [off_ms, cycle)
  }

  // --- RUN MODE: D3 — WiFi connection + HTTP-200 blink + URL-select burst ---
  bool wifiConnected = WiFi.status() == WL_CONNECTED;
  bool httpLedOn = wifiConnected; // default: on iff connected

  // URL-selection burst blink takes priority over the HTTP-200 pulse.
  if (gD3BurstCount > 0) {
    unsigned long burstEnd = gD3BurstStartMs + (unsigned long)gD3BurstCount * URL_BLINK_PERIOD;
    if (now < burstEnd) {
      unsigned long elapsed = now - gD3BurstStartMs;
      unsigned long phase   = elapsed % URL_BLINK_PERIOD;
      httpLedOn = phase >= URL_BLINK_OFF_MS;
    } else {
      gD3BurstCount = 0;
    }
  } else if (httpLedOn && now < gHttpLedForceOffUntilMs) {
    // Inside the HTTP-200 or save-confirmation forced-off window.
    httpLedOn = false;
  }

  digitalWrite(BLE_LED_PIN,  bleLedOn  ? HIGH : LOW);
  digitalWrite(HTTP_LED_PIN, httpLedOn ? HIGH : LOW);
}

// ---------------------------------------------------------------------------
// ledTask — dedicated FreeRTOS task for LED driving
// ---------------------------------------------------------------------------
// Running the LED logic in its own task decouples it completely from the
// main loop, which can block for hundreds of milliseconds during HTTP GETs
// or BLE scanning.  Without this, short blink pulses (< ~200 ms) would be
// stretched or lost entirely depending on what else happens to be running.
//
// Design choices:
//   • Pinned to Core 1 (APP_CPU_NUM = 1).  The BLE and WiFi protocol stacks
//     run primarily on Core 0 (PRO_CPU); keeping LED work on Core 1 avoids
//     any scheduling interference with those timing-sensitive stacks.
//
//   • vTaskDelayUntil() rather than vTaskDelay().  vTaskDelay() sleeps for
//     a relative duration, so accumulated execution time causes the period
//     to drift.  vTaskDelayUntil() sleeps until an *absolute* wake time,
//     correcting for the time spent inside updateStatusLeds() and keeping
//     the 5 ms tick rock-steady.
//
//   • 5 ms period: fine enough to resolve the 80 ms blink phases accurately
//     (worst-case ±5 ms = ±6% of the shortest phase) while spending less
//     than 0.1% of Core 1's time on LED work.
//
//   • Stack size 2048 bytes: updateStatusLeds() is pure arithmetic + two
//     digitalWrite calls; 2 kB is generous.
//
//   • Priority 1 (lowest user-space priority): LEDs are cosmetic; any higher-
//     priority work on Core 1 should always be able to preempt this task.
static void ledTask(void* /*pvParameters*/) {
  const TickType_t period       = pdMS_TO_TICKS(5);   // 5 ms tick
  TickType_t       xLastWakeTime = xTaskGetTickCount();
  for (;;) {
    updateStatusLeds();
    vTaskDelayUntil(&xLastWakeTime, period); // sleep until next absolute tick
  }
}

void setup() {
    Serial.begin(115200);

  // Initialise LED pins as outputs and start them LOW (off).
  // This ensures a defined visual state before the LED task starts.
  pinMode(BLE_LED_PIN,  OUTPUT);
  pinMode(HTTP_LED_PIN, OUTPUT);
  digitalWrite(BLE_LED_PIN,  LOW);
  digitalWrite(HTTP_LED_PIN, LOW);

  // Start the LED task before any other (slow) initialisation so that the
  // config-mode alternating blink becomes visible immediately while NimBLE,
  // WiFi, and NVS are being initialised below.  By the time setup() finishes
  // gConfigMode is set to its final value, and the LED task picks it up
  // on the next 5 ms tick without any additional signalling needed.
  xTaskCreatePinnedToCore(
    ledTask,    // task function
    "led",      // human-readable task name (shown in FreeRTOS debug tools)
    2048,       // stack size in bytes
    nullptr,    // parameter passed to task (unused)
    1,          // priority (1 = lowest user-space; LEDs are cosmetic)
    nullptr,    // task handle (we don't need to control it later)
    1           // CPU core (1 = APP_CPU, away from BLE/WiFi on PRO_CPU)
  );

    // Read the boot button BEFORE anything else so that the user only needs
    // to hold it for CONFIG_BUTTON_HOLD_MS.  Everything below is slow and
    // would otherwise inflate the required hold time unpredictably.
    bool forceConfigMode = isConfigButtonHeldOnBoot();

    // Brief pause to let the serial monitor connect before the first output.
    delay(800);

    // --- NimBLE stack initialisation ---
    // "ESP32-KB-Receiver" is the device name advertised over BLE.  It is
    // only visible while scanning; as a central we rarely advertise.
    NimBLEDevice::init("ESP32-KB-Receiver");

    // Maximum TX power.  A stronger signal helps maintain the BLE connection
    // across a room and speeds up the initial scan-and-connect phase.
    NimBLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_DEFAULT);

    // Security mode: bonding enabled (true), MITM NOT required (false),
    // SC (Secure Connections / BLE 4.2 LE Secure Connections) enabled (true).
    // MITM is skipped because keyboards typically have no display or keypad,
    // so numeric comparison or passkey entry is impractical.
    NimBLEDevice::setSecurityAuth(true, false, true);

    // IO capability: no input, no output.  Combined with MITM=false this
    // selects the "Just Works" pairing model, which auto-accepts the bond
    // without requiring user interaction on either side.
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    // Key distribution during pairing: distribute encryption key (LTK) and
    // identity (IRK) in both directions.  The LTK allows re-encryption of
    // future connections without re-pairing; the IRK allows resolution of
    // private (random) BLE addresses used by the keyboard.
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

    // Wire up HttpBridge with function pointers:
    //   KeyLog::add       — log function for HTTP event messages
    //   currentBaseUrl    — called at dispatch time to get the latest base URL
    //   mappedPathForKey  — translates a key code to a relative path
    HttpBridge::begin(KeyLog::add, currentBaseUrl, mappedPathForKey);

    // Register LED notification callbacks so HttpBridge can signal the LED
    // state machine when a GET starts and when the result arrives.
    HttpBridge::setGetCallbacks(onHttpGetStart, onHttpGetResult);

    // Start the BLE keyboard module, passing the log function and the key-press
    // callback that will enqueue the key code in HttpBridge and arm the D1 blink.
    BLEKeyboard::begin(KeyLog::add, onBleKeyPress);

    // Load all persisted configuration from NVS into RAM.
    // After this call gWifiNetworks, gBaseUrls, and gKeyMappings reflect the
    // last values saved via the web UI (or are empty/default on first boot).
    ConfigStore::load(gWifiNetworks, gBaseUrls, gSelectedUrlIndex, gKeyMappings);
    KeyLog::add(
      String("Config: wifi=") + String(gWifiNetworks.size()) + String(" net(s)") +
      String(" urls=") + String(gBaseUrls.size()) +
      String(" sel=") + String(gSelectedUrlIndex) +
      String(" maps=") + String(gKeyMappings.size())
    );

    // Refresh the preferred bonded device address from NimBLE's bond store.
    // This picks the first (or previously chosen) bonded address so auto-
    // connect can start without the user having to go through the web UI again.
    BLEKeyboard::refreshPreferredBondedDevice();

    // Determine operating mode.  hasValidRunConfig() checks all four conditions
    // needed for unattended RUN mode operation.  If any is missing, or if the
    // user explicitly requested config mode at boot, we enter CONFIG mode.
    bool runConfigReady = ConfigStore::hasValidRunConfig(
      gWifiNetworks, gBaseUrls, gKeyMappings,
      BLEKeyboard::preferredBondedAddress()
    );
    gConfigMode = forceConfigMode || !runConfigReady;

    if (gConfigMode) {
      // ---- CONFIG MODE setup ----

      // Start the ESP32 as a WiFi SoftAP.  WIFI_AP mode runs only the AP
      // interface; no station connection is attempted.
      WiFi.mode(WIFI_AP);
      WiFi.softAP(AP_SSID, AP_PASSWORD);

      // Serve the bundled single-page web app at the root URL.
      // PAGE is a large const char[] defined in web_page.h.
      server.on("/", HTTP_GET, []() { server.send(200, "text/html", PAGE); });

      // Register all BLE management REST endpoints (scan, pair, connect, …).
      WebBleApi::registerRoutes(server);

      // Build the config API context — bundles pointers to all mutable
      // state plus callback hooks so WebConfigApi can modify config without
      // needing access to this file's globals directly.
      WebConfigApi::Context cfgCtx = {
        &gWifiNetworks,
        &gBaseUrls,
        &gSelectedUrlIndex,
        &gKeyMappings,
        KeyLog::add,
        handleFactoryResetExtras
      };
      // Register all configuration REST endpoints (WiFi, URL, mappings, reset).
      WebConfigApi::registerRoutes(server, cfgCtx);
      server.begin();

      Serial.println("\nESP32 BLE Keyboard Hub - CONFIG mode");
      Serial.print("Open GUI at: http://");
      Serial.println(WiFi.softAPIP()); // typically 192.168.4.1
      KeyLog::add("GUI ready");
    } else {
      // ---- RUN MODE setup ----

      // Switch to station (STA) mode so we can join an existing WiFi network.
      WiFi.mode(WIFI_STA);

      // Register all stored networks with WiFiMulti.  In the main loop,
      // WiFiMulti::run() will scan and connect to whichever of these SSIDs
      // has the strongest signal.
      for (const auto& net : gWifiNetworks) {
        gWifiMulti.addAP(net.ssid.c_str(), net.password.c_str());
      }

      // Attempt an initial WiFi connection with a generous 10 s timeout.
      // If it fails (e.g. the router is temporarily unreachable) the main
      // loop will retry every 5 s via WiFiMulti::run(500).
      gWifiMulti.run(10000);

      Serial.println("\nESP32 BLE Keyboard Hub - RUN mode");
      KeyLog::add(String("RUN mode: ") + String(gWifiNetworks.size()) + " WiFi network(s) configured");
      KeyLog::add("RUN mode: waiting for keyboard and mapped keypresses");
      // BLE auto-connect starts from loop() via maybeAutoConnectBondedKeyboard().
    }
}

// ---------------------------------------------------------------------------
// loop — main cooperative task, runs on Core 1 alongside the LED task
// ---------------------------------------------------------------------------
// Kept deliberately thin so it does not block the scheduler longer than
// necessary.  The LED task uses vTaskDelayUntil and will preempt this loop
// at its 5 ms tick regardless, but shorter loop iterations mean less latency
// between a key press and its HTTP dispatch.
void loop() {
    // CONFIG MODE: drive the HTTP server's client-handling state machine.
    // Must be called frequently (ideally every few ms) to avoid TCP timeouts
    // on the browser side.
    if (gConfigMode) {
      server.handleClient();
    }

    // Detect phantom-connected state: NimBLE's gConnected flag can lag behind
    // the actual link state.  syncConnectionState() reconciles the two by
    // checking whether the underlying client object still reports connected.
    BLEKeyboard::syncConnectionState();

    // In RUN mode: if no keyboard is connected and the auto-connect cooldown
    // has elapsed, scan for the preferred bonded keyboard and reconnect.
    // The 8 s cooldown prevents the continuous BLE scanning from hogging
    // the radio and interfering with WiFi.
    BLEKeyboard::maybeAutoConnectBondedKeyboard();

    if (!gConfigMode) {
      // WiFi watchdog: if the station link has dropped, ask WiFiMulti to
      // reconnect.  The 500 ms timeout keeps the loop responsive while still
      // giving the WiFi stack enough time to complete an association.
      static unsigned long lastWifiCheck = 0;
      if (WiFi.status() != WL_CONNECTED && millis() - lastWifiCheck > 5000) {
        gWifiMulti.run(500);
        lastWifiCheck = millis();
      }

      // Drain the key-press queue: for each pending key code, look up its
      // mapped path and fire the HTTP GET.  This is synchronous (blocking
      // during the network round-trip) but acceptable because key presses
      // are infrequent and the BLE notification callback only queues codes,
      // never blocks.
      HttpBridge::processPendingKeys();

      // Poll the physical button for URL cycling / saving.
      handleButton();
    }

    // Yield to the FreeRTOS scheduler.  Without at least a 1 ms yield the
    // idle task on Core 1 never runs, preventing the watchdog from being
    // fed and causing unexpected resets.  10 ms is a comfortable margin.
    delay(10);
}