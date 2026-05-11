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
//       • The user holds the boot button (D10) LOW for ≥ 800 ms on power-on
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
#include <esp_sleep.h>      // Deep-sleep entry and wake-source configuration
#include <driver/rtc_io.h>  // RTC IO pull-up/down controls for sleep wake pins
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

// Table mapping burst signatures to relative URL paths.
// e.g. signature="037828" → "/door/open"
static std::vector<ButtonMapping> gButtonMappings;

// Whether the device is currently in CONFIG mode (true) or RUN mode (false).
// Starts as true; set to false in setup() once a valid config is confirmed.
static bool gConfigMode = true;

// ---------------------------------------------------------------------------
// Hardware pin assignments
// ---------------------------------------------------------------------------
// Single external button on D10 used for both boot-time force-config entry
// and runtime actions (URL cycling + deep-sleep wake).  D1 and D3 are
// onboard LEDs (active HIGH).
static const uint8_t CONFIG_BUTTON_PIN = D10;
static const uint8_t RUNTIME_BUTTON_PIN = CONFIG_BUTTON_PIN;

// USB-present sense input from external divider on D7.
// Divider expected behavior:
//   USB present (VBUS=5V)  -> pin HIGH (~2.5V with 100k/100k)
//   USB absent             -> pin LOW
static const uint8_t USB_SENSE_PIN = D7;

// ---------------------------------------------------------------------------
// Config-mode entry: button hold threshold
// ---------------------------------------------------------------------------
// The user must hold the shared button LOW for this many ms at boot to force
// CONFIG mode.
// 800 ms is long enough to be intentional but short enough to be comfortable.
static const unsigned long CONFIG_BUTTON_HOLD_MS = 800;

// Sleep-entry guard: wake pin must remain released (HIGH) for this long before
// entering deep sleep. This avoids immediate wake from line bounce/noise.
static const unsigned long SLEEP_ENTRY_RELEASE_STABLE_MS = 80;

// Native USB CDC on the ESP32-S3 can take a moment to re-enumerate after a
// reset. Wait briefly for the host serial monitor to reattach before emitting
// the late startup summary so mode diagnostics are not lost.
static const unsigned long SERIAL_MONITOR_ATTACH_TIMEOUT_MS = 2000;

// Deep-sleep wake source in phase-1 testing: external runtime button (active LOW).
static const uint8_t SLEEP_WAKE_BUTTON_PIN = RUNTIME_BUTTON_PIN;

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

// Button timing for URL cycling / save / sleep:
//   DEBOUNCE        — minimum hold to filter electrical noise
//   LONG_PRESS      — hold duration that triggers deep-sleep entry
//   EXTRA_LONG_PRESS— hold duration that triggers reboot into CONFIG mode
//   DOUBLE_WIN      — maximum gap between two short presses to count as double-press
static const unsigned long URL_BTN_DEBOUNCE_MS          =   50;
static const unsigned long URL_BTN_LONG_PRESS_MS        =  800;
static const unsigned long URL_BTN_EXTRA_LONG_PRESS_MS  = 4000;
static const unsigned long URL_BTN_DOUBLE_WINDOW_MS     =  400;

// D1 rapid-blink half-period while button is held in the long-press zone
// (between LONG_PRESS_MS and EXTRA_LONG_PRESS_MS).  100 ms → 5 Hz blink.
static const unsigned long BLE_HOLD_BLINK_HALF_MS = 100;
// D1 rapid-blink half-period while a BLE connect attempt is in flight.
// 100 ms → 5 Hz, same rate as the long-press hold blink.
static const unsigned long BLE_CONNECTING_BLINK_HALF_MS = 100;

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

// D1 hold-state indicator, written by handleButton(), read by updateStatusLeds():
//   0 — normal BLE connection + key-press blink behavior
//   1 — rapid blink (button held past LONG_PRESS_MS, before EXTRA_LONG_PRESS_MS)
//   2 — steady ON (button held past EXTRA_LONG_PRESS_MS)
static volatile uint8_t gD1HoldState = 0;

// Button state machine for runtime URL cycling / save / sleep (RUN mode only).
// BTN_IDLE            — waiting for the first press
// BTN_PRESSED         — button currently held; timing the hold duration
// BTN_WAIT_DOUBLE     — first press released; waiting for possible second press
// BTN_WAIT_SECOND_RELEASE — second press confirmed; waiting for its release
enum BtnState { BTN_IDLE, BTN_PRESSED, BTN_WAIT_DOUBLE, BTN_WAIT_SECOND_RELEASE };
static BtnState      gBtnState     = BTN_IDLE;
static unsigned long gBtnPressMs   = 0;
static unsigned long gBtnReleaseMs = 0;

// Deep-sleep policy state.
static uint32_t      gSleepTimeoutMs = ConfigStore::DEFAULT_SLEEP_TIMEOUT_MS;
static unsigned long gLastActivityMs = 0;

// ---------------------------------------------------------------------------
// Forward declarations (implementations follow in this file)
// ---------------------------------------------------------------------------
bool   isConfigButtonHeldOnBoot();
String mappedPathForKey(uint8_t keyCode);
void   handleFactoryResetExtras();
void   onBleSigPress(const String& signature);
void   onHttpGetStart();
void   onHttpGetResult(int statusCode);
void   updateStatusLeds();
void   cycleUrl();
void   saveSelectedUrl();
void   handleButton();
void   handleConfigButton();
void   markUserActivity();
bool   isRunningOnBattery();
void   maybeEnterDeepSleep();
void   enterDeepSleep();
const char* wakeupCauseToText(esp_sleep_wakeup_cause_t cause);
const char* powerSourceToText();
void   initPinsAndLedTask();
void   initBleAndHttpStack();
void   loadPersistedConfig();
bool   determineConfigMode(bool forceConfigMode);
void   startConfigModeServer();
void   startRunModeWiFi();
void   startSelectedMode();
void   waitForSerialMonitorAttach();
void   printLateStartupSummary();

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
// mappedPathForSig
// ---------------------------------------------------------------------------
// Looks up the URL path configured for a given burst signature.
// Returns an empty string if the signature has no mapping, which HttpBridge
// interprets as "do nothing" and silently drops the event.
String mappedPathForSig(const String& signature) {
  String sigLower = signature;
  sigLower.toLowerCase();
  for (const ButtonMapping& m : gButtonMappings) {
    String mLower = m.signature;
    mLower.toLowerCase();
    if (mLower == sigLower) {
      return m.url;
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
  markUserActivity();
  gSelectedUrlIndex = (uint8_t)((gSelectedUrlIndex + 1) % gBaseUrls.size());
  KeyLog::add(String("URL #") + String(gSelectedUrlIndex + 1) + ": " + currentBaseUrl());
  gD3BurstStartMs = millis();
  gD3BurstCount   = gSelectedUrlIndex + 1;
}

// ---------------------------------------------------------------------------
// saveSelectedUrl — persist the current URL selection to NVS (double press)
// ---------------------------------------------------------------------------
void saveSelectedUrl() {
  markUserActivity();
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
// Button protocol in RUN mode:
//   Short press  — cycle to the next base URL (cycleUrl)
//   Double press — save the current URL selection to NVS (saveSelectedUrl)
//   Long press   — enter deep sleep regardless of power source (enterDeepSleep)
// Single vs. double discrimination: after a valid short release, wait up to
// URL_BTN_DOUBLE_WINDOW_MS for a second press.  If it arrives → double press.
// If the window expires first → single press.  This means single-press actions
// are delayed by up to DOUBLE_WIN ms, which is imperceptible in practice.
// ---------------------------------------------------------------------------
// handleConfigButton — CONFIG mode button handler
// ---------------------------------------------------------------------------
// A short press (debounced, released before URL_BTN_LONG_PRESS_MS) reboots
// the device.  With a complete NVS config the reboot lands in RUN mode.
//
// Known edge case: if the user held D10 for ≥800 ms to force CONFIG mode at
// boot, the button may still be held when loop() first calls this function.
// sArmed would then fire on the tail-end release, causing a spurious reboot.
// A one-time "wait for button release" guard on first call would fix this;
// not implemented yet as it has not been observed in practice.
void handleConfigButton() {
  bool pressed = (digitalRead(RUNTIME_BUTTON_PIN) == LOW);
  static unsigned long sPressMs = 0;
  static bool          sArmed   = false;

  if (pressed && !sArmed) {
    sArmed   = true;
    sPressMs = millis();
  } else if (!pressed && sArmed) {
    unsigned long held = millis() - sPressMs;
    sArmed = false;
    if (held >= URL_BTN_DEBOUNCE_MS && held < URL_BTN_LONG_PRESS_MS) {
      KeyLog::add("Config button: rebooting to RUN mode");
      delay(500);
      esp_restart();
    }
  }
}

void handleButton() {
  bool pressed = (digitalRead(RUNTIME_BUTTON_PIN) == LOW);
  unsigned long now = millis();

  switch (gBtnState) {
    case BTN_IDLE:
      if (pressed) {
        markUserActivity();           // prevent inactivity timeout during hold
        gBtnState   = BTN_PRESSED;
        gBtnPressMs = now;
      }
      break;

    case BTN_PRESSED:
      if (pressed) {
        // Button still held — update D1 hold indicator so the LED task can
        // give the user visual feedback about which press zone they are in.
        unsigned long holding = now - gBtnPressMs;
        if (holding >= URL_BTN_EXTRA_LONG_PRESS_MS) {
          gD1HoldState = 2; // steady ON — extra-long zone (config reboot on release)
        } else if (holding >= URL_BTN_LONG_PRESS_MS) {
          gD1HoldState = 1; // rapid blink — long-press zone (sleep on release)
        } else {
          gD1HoldState = 0; // below long-press threshold — normal D1 behavior
        }
      } else {
        // Button released — clear hold indicator, then dispatch exactly one action.
        gD1HoldState = 0;
        unsigned long held = now - gBtnPressMs;
        if (held >= URL_BTN_EXTRA_LONG_PRESS_MS) {
          // Extra-long press: set NVS flag and reboot into CONFIG mode.
          // No sleep, no URL cycle — only this fires.
          KeyLog::add("Button: extra-long press → reboot to CONFIG");
          ConfigStore::setConfigBootFlag();
          delay(200);
          esp_restart();
        } else if (held >= URL_BTN_LONG_PRESS_MS) {
          enterDeepSleep();           // button already released — guard will pass
          gBtnState = BTN_IDLE;
        } else if (held >= URL_BTN_DEBOUNCE_MS) {
          gBtnReleaseMs = now;        // start double-press window
          gBtnState     = BTN_WAIT_DOUBLE;
        } else {
          gBtnState = BTN_IDLE;       // too short — electrical noise, ignore
        }
      }
      break;

    case BTN_WAIT_DOUBLE:
      if (pressed) {
        // Second press arrived within the window — confirmed double press
        saveSelectedUrl();
        gBtnState = BTN_WAIT_SECOND_RELEASE;
      } else if (now - gBtnReleaseMs >= URL_BTN_DOUBLE_WINDOW_MS) {
        // Window expired with no second press — confirmed single press.
        // If BLE is connected: cycle the base URL (original behavior).
        // If not connected: attempt an immediate reconnect; backoff schedule
        // resumes unchanged if the attempt fails.
        if (BLEKeyboard::isConnected()) {
          cycleUrl();
        } else {
          BLEKeyboard::tryConnectNow();
        }
        gBtnState = BTN_IDLE;
      }
      break;

    case BTN_WAIT_SECOND_RELEASE:
      if (!pressed) {
        gBtnState = BTN_IDLE;
      }
      break;
  }
}

// ---------------------------------------------------------------------------
// onBleSigPress — callback registered with BLEKeyboard
// ---------------------------------------------------------------------------
// Invoked from the BLE notification path (Core 0) every time a new burst
// (button press) is received from the connected HID keyboard.
//
// It does two things:
//   1. Arms the D1 double-blink window so the LED task will perform the
//      blink-off animation on its next tick, independently of how long the
//      subsequent HTTP request takes.  The window covers exactly
//      BLE_KEY_BLINK_COUNT full off/on cycles.
//
//   2. Enqueues the burst signature in HttpBridge for deferred HTTP dispatch.
//      HttpBridge::onSigPress() only queues it; the actual HTTP GET
//      happens in processPendingSigs() on the next loop() iteration, keeping
//      the BLE callback fast and non-blocking.
void onBleSigPress(const String& signature) {
  markUserActivity();
  // Record blink window; the LED task will drive the pin.
  unsigned long now = millis();
  gBleLedBlinkStartMs = now;
  gBleLedBlinkEndMs   = now + (BLE_KEY_BLINK_COUNT * (BLE_KEY_BLINK_OFF_MS + BLE_KEY_BLINK_ON_MS));
  HttpBridge::onSigPress(signature);
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

  // Connecting blink: while a reconnect attempt is in flight and BLE is not
  // yet connected, rapid-blink D1 so the user can tell the device is trying.
  // Cleared automatically to OFF when the attempt finishes without success.
  if (!bleConnected && BLEKeyboard::isConnecting()) {
    bleLedOn = (now / BLE_CONNECTING_BLINK_HALF_MS) % 2 == 0;
  }

  // Hold-state override: while D10 is held past LONG_PRESS_MS the button handler
  // sets gD1HoldState to give the user tactile confirmation of which zone they
  // are in.  This takes priority over the connection/blink-window logic above.
  //   1 → rapid blink at BLE_HOLD_BLINK_HALF_MS (long-press zone: sleep)
  //   2 → steady ON                              (extra-long zone:  config reboot)
  //   0 → no override; normal behavior restored after button release
  if (gD1HoldState == 2) {
    bleLedOn = true;
  } else if (gD1HoldState == 1) {
    bleLedOn = (now / BLE_HOLD_BLINK_HALF_MS) % 2 == 0;
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

// ---------------------------------------------------------------------------
// markUserActivity — update inactivity timer baseline
// ---------------------------------------------------------------------------
void markUserActivity() {
  gLastActivityMs = millis();
}

// ---------------------------------------------------------------------------
// isRunningOnBattery — true when USB 5V is not detected on D7
// ---------------------------------------------------------------------------
bool isRunningOnBattery() {
  return digitalRead(USB_SENSE_PIN) == LOW;
}

// ---------------------------------------------------------------------------
// powerSourceToText — readable power-source label
// ---------------------------------------------------------------------------
const char* powerSourceToText() {
  return isRunningOnBattery() ? "BATTERY" : "USB";
}

// ---------------------------------------------------------------------------
// wakeupCauseToText — readable label for boot logs
// ---------------------------------------------------------------------------
const char* wakeupCauseToText(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "UNDEFINED (cold boot/reset)";
    case ESP_SLEEP_WAKEUP_EXT0:      return "EXT0";
    case ESP_SLEEP_WAKEUP_EXT1:      return "EXT1";
    case ESP_SLEEP_WAKEUP_TIMER:     return "TIMER";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:  return "TOUCHPAD";
    case ESP_SLEEP_WAKEUP_ULP:       return "ULP";
    case ESP_SLEEP_WAKEUP_GPIO:      return "GPIO";
    case ESP_SLEEP_WAKEUP_UART:      return "UART";
    default:                         return "OTHER";
  }
}

// ---------------------------------------------------------------------------
// enterDeepSleep — configure wake source and start deep sleep
// ---------------------------------------------------------------------------
void enterDeepSleep() {
  // Guard: only sleep when wake button is released and stable HIGH.
  unsigned long stableStart = millis();
  while (millis() - stableStart < SLEEP_ENTRY_RELEASE_STABLE_MS) {
    if (digitalRead(SLEEP_WAKE_BUTTON_PIN) == LOW) {
      markUserActivity();
      return;
    }
    delay(2);
  }

  KeyLog::add(String("Entering deep sleep after ") + String(gSleepTimeoutMs) + String(" ms inactivity"));
  BLEKeyboard::disconnectKeyboard();

  // Keep wake pin biased HIGH in deep sleep to avoid floating-trigger wake.
  rtc_gpio_pullup_en((gpio_num_t)SLEEP_WAKE_BUTTON_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)SLEEP_WAKE_BUTTON_PIN);

  // Wake when the button pin is pulled LOW (button press).
  if (!esp_sleep_is_valid_wakeup_gpio((gpio_num_t)SLEEP_WAKE_BUTTON_PIN)) {
    KeyLog::add(String("Sleep aborted: wake GPIO invalid for deep sleep: ") + String(SLEEP_WAKE_BUTTON_PIN));
    markUserActivity();
    return;
  }
  esp_err_t wakeCfg = esp_sleep_enable_ext0_wakeup((gpio_num_t)SLEEP_WAKE_BUTTON_PIN, 0);
  if (wakeCfg != ESP_OK) {
    KeyLog::add(String("Sleep aborted: ext0 cfg failed err=") + String((int)wakeCfg));
    markUserActivity();
    return;
  }

  delay(50);
  Serial.flush();
  esp_deep_sleep_start();
}

// ---------------------------------------------------------------------------
// maybeEnterDeepSleep — RUN-mode inactivity sleep gate
// ---------------------------------------------------------------------------
void maybeEnterDeepSleep() {
  if (gConfigMode) return;
  if (!isRunningOnBattery()) return;
  if (gSleepTimeoutMs < ConfigStore::MIN_SLEEP_TIMEOUT_MS) return;

  unsigned long now = millis();
  if (now - gLastActivityMs >= gSleepTimeoutMs) {
    enterDeepSleep();
  }
}

// ---------------------------------------------------------------------------
// initPinsAndLedTask — configure I/O and start dedicated LED task
// ---------------------------------------------------------------------------
void initPinsAndLedTask() {
  // Runtime/config button (active LOW).
  pinMode(RUNTIME_BUTTON_PIN, INPUT_PULLUP);

  // USB presence detector (HIGH=USB, LOW=battery).
  pinMode(USB_SENSE_PIN, INPUT);
  Serial.print("Power source (early): ");
  Serial.println(powerSourceToText());

  // Ensure a defined LED state before the task starts.
  pinMode(BLE_LED_PIN,  OUTPUT);
  pinMode(HTTP_LED_PIN, OUTPUT);
  digitalWrite(BLE_LED_PIN,  LOW);
  digitalWrite(HTTP_LED_PIN, LOW);

  // Start LED task early so status feedback is live during boot.
  xTaskCreatePinnedToCore(
    ledTask,    // task function
    "led",      // human-readable task name (shown in FreeRTOS debug tools)
    2048,       // stack size in bytes
    nullptr,    // parameter passed to task (unused)
    1,          // priority (1 = lowest user-space; LEDs are cosmetic)
    nullptr,    // task handle (we don't need to control it later)
    1           // CPU core (1 = APP_CPU, away from BLE/WiFi on PRO_CPU)
  );
}

// ---------------------------------------------------------------------------
// initBleAndHttpStack — initialise BLE stack and HTTP bridge plumbing
// ---------------------------------------------------------------------------
void initBleAndHttpStack() {
  // NimBLE stack initialisation.
  NimBLEDevice::init("ESP32-KB-Receiver");

  // Strong TX power for more robust keyboard links.
  NimBLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_DEFAULT);

  // Bonding + secure connections, with keyboard-friendly IO settings.
  NimBLEDevice::setSecurityAuth(true, false, true);

  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

  // Wire up HTTP bridge callbacks.
  HttpBridge::begin(KeyLog::add, currentBaseUrl, mappedPathForSig);

  // Register HTTP result hooks for LED signaling.
  HttpBridge::setGetCallbacks(onHttpGetStart, onHttpGetResult);

  // Start keyboard module and sig callback chain.
  BLEKeyboard::begin(KeyLog::add, onBleSigPress);
}

// ---------------------------------------------------------------------------
// loadPersistedConfig — load NVS config into RAM and log summary
// ---------------------------------------------------------------------------
void loadPersistedConfig() {
  // Load persisted config from NVS into runtime state.
  ConfigStore::load(gWifiNetworks, gBaseUrls, gSelectedUrlIndex, gButtonMappings, gSleepTimeoutMs);
  KeyLog::add(
    String("Config: wifi=") + String(gWifiNetworks.size()) + String(" net(s)") +
    String(" urls=") + String(gBaseUrls.size()) +
    String(" sel=") + String(gSelectedUrlIndex) +
    String(" maps=") + String(gButtonMappings.size()) +
    String(" sleepMs=") + String(gSleepTimeoutMs)
  );

  // Refresh preferred bonded keyboard used by auto-connect.
  BLEKeyboard::refreshPreferredBondedDevice();
}

// ---------------------------------------------------------------------------
// determineConfigMode — decide config vs run mode from boot + persisted state
// ---------------------------------------------------------------------------
bool determineConfigMode(bool forceConfigMode) {
  // Check (and always clear) the one-shot NVS flag set by the extra-long press handler.
  // This must be done before any early-return so the flag is never left set.
  bool nvsFlagSet = ConfigStore::getAndClearConfigBootFlag();

  // RUN mode is allowed only with complete persisted configuration.
  bool runConfigReady = ConfigStore::hasValidRunConfig(
    gWifiNetworks,
    gBaseUrls,
    gButtonMappings,
    BLEKeyboard::preferredBondedAddress()
  );
  return forceConfigMode || nvsFlagSet || !runConfigReady;
}

// ---------------------------------------------------------------------------
// startConfigModeServer — start AP + HTTP server routes for config mode
// ---------------------------------------------------------------------------
void startConfigModeServer() {
  // Start config AP.
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  // Serve the embedded web app.
  server.on("/", HTTP_GET, []() { server.send(200, "text/html", PAGE); });

  // Register BLE and config APIs.
  WebBleApi::registerRoutes(server);

  WebConfigApi::Context cfgCtx = {
    &gWifiNetworks,
    &gBaseUrls,
    &gSelectedUrlIndex,
    &gButtonMappings,
    &gSleepTimeoutMs,
    KeyLog::add,
    handleFactoryResetExtras
  };
  WebConfigApi::registerRoutes(server, cfgCtx);
  server.begin();

  Serial.println("\nESP32 BLE Keyboard Hub - CONFIG mode");
  Serial.print("Power source: ");
  Serial.println(powerSourceToText());
  Serial.print("Open GUI at: http://");
  Serial.println(WiFi.softAPIP()); // typically 192.168.4.1
  KeyLog::add("Mode: CONFIG");
  KeyLog::add(String("AP: http://") + WiFi.softAPIP().toString());
  KeyLog::add("GUI ready");
  KeyLog::add(String("Power source: ") + powerSourceToText());
}

// ---------------------------------------------------------------------------
// startRunModeWiFi — configure STA mode and initial WiFi connection
// ---------------------------------------------------------------------------
void startRunModeWiFi() {
  // Start station mode and register saved APs.
  WiFi.mode(WIFI_STA);

  for (const auto& net : gWifiNetworks) {
    gWifiMulti.addAP(net.ssid.c_str(), net.password.c_str());
  }

  // Initial connect attempt; loop() handles retries.
  gWifiMulti.run(10000);

  Serial.println("\nESP32 BLE Keyboard Hub - RUN mode");
  Serial.print("Power source: ");
  Serial.println(powerSourceToText());
  KeyLog::add(String("RUN mode: ") + String(gWifiNetworks.size()) + " WiFi network(s) configured");
  KeyLog::add("RUN mode: waiting for keyboard and mapped keypresses");
  KeyLog::add(String("Power source: ") + powerSourceToText());
  // BLE auto-connect starts from loop().
}

// ---------------------------------------------------------------------------
// startSelectedMode — dispatch to config/run startup based on gConfigMode
// ---------------------------------------------------------------------------
void startSelectedMode() {
  if (gConfigMode) {
    startConfigModeServer();
  } else {
    startRunModeWiFi();
  }
}

// ---------------------------------------------------------------------------
// waitForSerialMonitorAttach — bounded wait for native USB CDC reattach
// ---------------------------------------------------------------------------
void waitForSerialMonitorAttach() {
  if (isRunningOnBattery()) {
    return;
  }

  unsigned long started = millis();
  while (!Serial && millis() - started < SERIAL_MONITOR_ATTACH_TIMEOUT_MS) {
    delay(10);
  }

  // Give the host a brief settle window after the port opens so the next
  // lines are not truncated by the monitor reconnect sequence.
  delay(150);
}

// ---------------------------------------------------------------------------
// printLateStartupSummary — emit mode details after USB monitor reconnect
// ---------------------------------------------------------------------------
void printLateStartupSummary() {
  Serial.println();
  Serial.println("Startup ready");
  Serial.print("Mode: ");
  Serial.println(gConfigMode ? "CONFIG" : "RUN");
  Serial.print("Power source: ");
  Serial.println(powerSourceToText());

  if (gConfigMode) {
    Serial.print("Config AP: ");
    Serial.println(AP_SSID);
    Serial.print("Open GUI at: http://");
    Serial.println(WiFi.softAPIP());
    KeyLog::add(String("Startup: ") + (gConfigMode ? "CONFIG" : "RUN") + " mode");
    KeyLog::add(String("AP SSID: ") + AP_SSID);
    KeyLog::add(String("UI: http://") + WiFi.softAPIP().toString());
  } else {
    Serial.print("WiFi status: ");
    Serial.println(WiFi.status() == WL_CONNECTED ? "connected" : "not connected yet");
  }
}

void setup() {
  // Phase 1: baseline boot telemetry.
  Serial.begin(115200);

  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  Serial.print("Wake cause: ");
  Serial.println(wakeupCauseToText(wakeCause));

  // Phase 2: early I/O state and user override sampling.
  initPinsAndLedTask();

  // Read button early so hold timing is predictable.
  bool forceConfigMode = isConfigButtonHeldOnBoot();

  // Brief pause to let the serial monitor connect before the first output.
  delay(800);

  Serial.print("Power source: ");
  Serial.println(powerSourceToText());
  KeyLog::add(String("Power source: ") + powerSourceToText());

  // Phase 3: protocol stacks and persisted state.
  initBleAndHttpStack();
  loadPersistedConfig();
  gConfigMode = determineConfigMode(forceConfigMode);

  // Tell the BLE reconnect controller which policy to apply.
  BLEKeyboard::setReconnectMode(!gConfigMode);

  // Phase 4: mode-specific startup.
  startSelectedMode();
  waitForSerialMonitorAttach();
  printLateStartupSummary();

  markUserActivity();
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
    handleConfigButton();
  }

  // Detect phantom-connected state: NimBLE's gConnected flag can lag behind
  // the actual link state.  syncConnectionState() reconciles the two by
  // checking whether the underlying client object still reports connected.
  BLEKeyboard::syncConnectionState();

  // Auto-connect runs in both config and run mode.  In config mode it is
  // suppressed while a web UI scan is in progress (setAutoConnectEnabled(false)
  // is called by the /scan handler to avoid scanner contention).  It resumes
  // automatically after pairing, or on reboot.
  BLEKeyboard::maybeAutoConnectBondedKeyboard();

  if (!gConfigMode) {
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
    HttpBridge::processPendingSigs();

    // Poll the physical button for URL cycling / saving.
    handleButton();

    // Enter deep sleep after prolonged inactivity in battery mode.
    maybeEnterDeepSleep();
  }

  // Yield to the FreeRTOS scheduler.  Without at least a 1 ms yield the
  // idle task on Core 1 never runs, preventing the watchdog from being
  // fed and causing unexpected resets.  10 ms is a comfortable margin.
  delay(10);
}