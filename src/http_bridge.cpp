// =============================================================================
// http_bridge.cpp — key-press queue and HTTP GET dispatch
// =============================================================================
//
// Responsibilities
// ----------------
// This module sits between the BLE keyboard module and the network layer:
//
//   1. Key-press intake:  onKeyPress() enqueues a raw HID key code.
//      It is called from the BLE notification callback (fast path), so it
//      must be non-blocking and return immediately.
//
//   2. Dispatch:  processPendingKeys() drains the queue synchronously in the
//      main loop.  For each key code it looks up the mapped path, builds the
//      full URL, and performs a blocking HTTP GET.  This is intentionally
//      synchronous — the main loop is willing to block for the duration of
//      one HTTP round-trip (≈ 50–500 ms) because key presses are infrequent.
//
//   3. Repeat filtering:  Typing the same key in rapid succession (within
//      KEY_REPEAT_FILTER_MS) fires only a single HTTP GET.  This prevents a
//      held key from flooding the target server with duplicate requests.
//
//   4. Observability hooks:  Optional callbacks (gGetStartFn / gGetResultFn)
//      let the caller (main.cpp) react to GET lifecycle events — used to
//      drive the D3 LED blink without creating a dependency from this module
//      on the LED state machine.
// =============================================================================

#include "http_bridge.h"

#include <HTTPClient.h>
#include <WiFi.h>

#include <deque>

namespace {

// Queue of key codes waiting to be dispatched via HTTP GET.
// A deque is used so we can cap size by discarding from the front (oldest
// key) without shifting elements.  24 entries is large enough to absorb a
// burst of quick presses while the previous GET is still in flight, yet
// small enough that a stuck WiFi does not make the queue grow unboundedly.
std::deque<uint8_t> gPendingKeyCodes;

// Repeat-filter state: remember the last dispatched code and its timestamp.
// If the same code arrives again within KEY_REPEAT_FILTER_MS we drop it.
// This is particularly important for keyboards that send auto-repeat events
// while a key is held — we do not want to hammer the target endpoint.
uint8_t       gLastDispatchedKeyCode = 0;
unsigned long gLastDispatchMs        = 0;
const unsigned long KEY_REPEAT_FILTER_MS = 120;

// Injected dependencies — set via begin() and setGetCallbacks().
HttpBridge::LogFn        gLogFn       = nullptr;
HttpBridge::BaseUrlFn    gBaseUrlFn   = nullptr;
HttpBridge::MappedPathFn gMappedPathFn= nullptr;
HttpBridge::GetStartFn   gGetStartFn  = nullptr; // called just before http.GET()
HttpBridge::GetResultFn  gGetResultFn = nullptr; // called with the HTTP status code

void addKeyLog(const String& line) {
  if (gLogFn) gLogFn(line);
}

// ---------------------------------------------------------------------------
// dispatchKeyHttp — build the URL and execute the HTTP GET
// ---------------------------------------------------------------------------
// Called synchronously from processPendingKeys().  Blocks until the server
// responds or the HTTPClient times out.
//
// URL construction:
//   base URL and mapped path may or may not end/start with '/'.  We
//   normalise the junction so there is exactly one '/' between them,
//   avoiding both double-slashes and missing-slash bugs.
void dispatchKeyHttp(uint8_t keyCode) {
  if (!gMappedPathFn || !gBaseUrlFn) {
    return; // module not fully initialised
  }

  String path = gMappedPathFn(keyCode);
  if (path.length() == 0) {
    return; // key has no mapping — silently ignore
  }

  if (WiFi.status() != WL_CONNECTED) {
    addKeyLog("HTTP skipped: WiFi not connected");
    return;
  }

  // Normalise the base URL + path junction.
  String url         = gBaseUrlFn();
  bool baseHasSlash  = url.endsWith("/");
  bool pathHasSlash  = path.startsWith("/");
  if (baseHasSlash && pathHasSlash) {
    url.remove(url.length() - 1); // remove trailing slash to avoid double-slash
  } else if (!baseHasSlash && !pathHasSlash) {
    url += "/";                    // insert missing separator
  }
  url += path; // final URL e.g. "http://192.168.1.50:8080/door/open"

  HTTPClient http;
  if (!http.begin(url)) {
    addKeyLog(String("HTTP begin failed: ") + url);
    return; // invalid URL or unsupported scheme
  }

  // Notify the caller that a GET is about to start (used for LED timing).
  if (gGetStartFn) { gGetStartFn(); }

  int rc = http.GET(); // blocking — waits for the full response headers

  // Notify the caller of the result code (positive = HTTP status, negative = error).
  if (gGetResultFn) { gGetResultFn(rc); }

  if (rc > 0) {
    addKeyLog(String("HTTP GET ") + url + String(" -> ") + String(rc));
  } else {
    addKeyLog(String("HTTP GET failed: ") + HTTPClient::errorToString(rc));
  }
  http.end(); // release connection and free memory
}

} // namespace

namespace HttpBridge {

// Store the injected function pointers.  All three are required for dispatch;
// the GET callbacks are optional (nullptr = no notification).
void begin(LogFn logFn, BaseUrlFn baseUrlFn, MappedPathFn mappedPathFn) {
  gLogFn        = logFn;
  gBaseUrlFn    = baseUrlFn;
  gMappedPathFn = mappedPathFn;
}

// Register optional GET lifecycle callbacks.
// Separated from begin() so they can be updated independently (e.g. if the
// LED strategy changes without altering the dispatch configuration).
void setGetCallbacks(GetStartFn getStartFn, GetResultFn getResultFn) {
  gGetStartFn  = getStartFn;
  gGetResultFn = getResultFn;
}

// ---------------------------------------------------------------------------
// onKeyPress — called from the BLE notification callback (fast path)
// ---------------------------------------------------------------------------
// Enqueues a key code for deferred HTTP dispatch.  Must be non-blocking.
// The oldest entry is evicted when the queue is full to bound memory usage
// and ensure recent presses are always dispatched even under sustained load.
void onKeyPress(uint8_t keyCode) {
  if (keyCode == 0) {
    return; // key-up / release report — nothing to dispatch
  }
  if (gPendingKeyCodes.size() >= 24) {
    gPendingKeyCodes.pop_front(); // drop oldest to make room
  }
  gPendingKeyCodes.push_back(keyCode);
}

// ---------------------------------------------------------------------------
// processPendingKeys — called from the main loop
// ---------------------------------------------------------------------------
// Drains the queue, applying the repeat filter and dispatching each unique
// (or sufficiently delayed) key code via HTTP GET.
void processPendingKeys() {
  while (!gPendingKeyCodes.empty()) {
    uint8_t keyCode = gPendingKeyCodes.front();
    gPendingKeyCodes.pop_front();

    unsigned long now = millis();
    // Skip if this is an auto-repeat of the same key within the filter window.
    if (keyCode == gLastDispatchedKeyCode &&
        (now - gLastDispatchMs) < KEY_REPEAT_FILTER_MS) {
      continue;
    }

    gLastDispatchedKeyCode = keyCode;
    gLastDispatchMs        = now;
    dispatchKeyHttp(keyCode); // may block for the HTTP round-trip
  }
}

} // namespace HttpBridge
