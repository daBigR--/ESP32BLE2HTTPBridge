// =============================================================================
// http_bridge.cpp — burst-signature queue and HTTP GET dispatch
// =============================================================================
//
// Responsibilities
// ----------------
// This module sits between the BLE keyboard module and the network layer:
//
//   1. Signature intake:  onSigPress() enqueues a burst signature string.
//      It is called from the BLE notification callback (fast path), so it
//      must be non-blocking and return immediately.
//
//   2. Dispatch:  processPendingSigs() drains the queue synchronously in the
//      main loop.  For each signature it looks up the mapped path, builds the
//      full URL, and performs a blocking HTTP GET.  This is intentionally
//      synchronous — the main loop is willing to block for the duration of
//      one HTTP round-trip (≈ 50–500 ms) because button presses are infrequent.
//
//   3. Repeat filtering:  The same signature within KEY_REPEAT_FILTER_MS fires
//      only a single HTTP GET.  This prevents a held button from flooding the
//      target server with duplicate requests.
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

// Queue of burst signatures waiting to be dispatched via HTTP GET.
// A deque is used so we can cap size by discarding from the front (oldest)
// without shifting elements.
std::deque<String> gPendingSigs;

// Repeat-filter state: remember the last dispatched signature and its timestamp.
String        gLastDispatchedSig = "";
unsigned long gLastDispatchMs    = 0;
const unsigned long KEY_REPEAT_FILTER_MS = 120;

// Injected dependencies — set via begin() and setGetCallbacks().
HttpBridge::LogFn        gLogFn        = nullptr;
HttpBridge::BaseUrlFn    gBaseUrlFn    = nullptr;
HttpBridge::MappedPathFn gMappedPathFn = nullptr;
HttpBridge::GetStartFn   gGetStartFn   = nullptr;
HttpBridge::GetResultFn  gGetResultFn  = nullptr;

void addKeyLog(const String& line) {
  if (gLogFn) gLogFn(line);
}

// ---------------------------------------------------------------------------
// dispatchSigHttp — build the URL and execute the HTTP GET
// ---------------------------------------------------------------------------
void dispatchSigHttp(const String& sig) {
  if (!gMappedPathFn || !gBaseUrlFn) {
    return; // module not fully initialised
  }

  String path = gMappedPathFn(sig);
  if (path.length() == 0) {
    return; // signature has no mapping — silently ignore
  }

  if (WiFi.status() != WL_CONNECTED) {
    addKeyLog("HTTP skipped: WiFi not connected");
    return;
  }

  // Normalise the base URL + path junction.
  String url        = gBaseUrlFn();
  bool baseHasSlash = url.endsWith("/");
  bool pathHasSlash = path.startsWith("/");
  if (baseHasSlash && pathHasSlash) {
    url.remove(url.length() - 1);
  } else if (!baseHasSlash && !pathHasSlash) {
    url += "/";
  }
  url += path;

  HTTPClient http;
  if (!http.begin(url)) {
    addKeyLog(String("HTTP begin failed: ") + url);
    return;
  }

  if (gGetStartFn) { gGetStartFn(); }

  int rc = http.GET();

  if (gGetResultFn) { gGetResultFn(rc); }

  if (rc > 0) {
    addKeyLog(String("HTTP GET ") + url + String(" -> ") + String(rc));
  } else {
    addKeyLog(String("HTTP GET failed: ") + HTTPClient::errorToString(rc));
  }
  http.end();
}

} // namespace

namespace HttpBridge {

void begin(LogFn logFn, BaseUrlFn baseUrlFn, MappedPathFn mappedPathFn) {
  gLogFn        = logFn;
  gBaseUrlFn    = baseUrlFn;
  gMappedPathFn = mappedPathFn;
}

void setGetCallbacks(GetStartFn getStartFn, GetResultFn getResultFn) {
  gGetStartFn  = getStartFn;
  gGetResultFn = getResultFn;
}

// ---------------------------------------------------------------------------
// onSigPress — called from the BLE notification callback (fast path)
// ---------------------------------------------------------------------------
void onSigPress(const String& signature) {
  if (signature.length() == 0) {
    return;
  }
  if (gPendingSigs.size() >= 24) {
    gPendingSigs.pop_front(); // drop oldest to make room
  }
  gPendingSigs.push_back(signature);
}

// ---------------------------------------------------------------------------
// processPendingSigs — called from the main loop
// ---------------------------------------------------------------------------
void processPendingSigs() {
  while (!gPendingSigs.empty()) {
    String sig = gPendingSigs.front();
    gPendingSigs.pop_front();

    unsigned long now = millis();
    if (sig == gLastDispatchedSig &&
        (now - gLastDispatchMs) < KEY_REPEAT_FILTER_MS) {
      continue;
    }

    gLastDispatchedSig = sig;
    gLastDispatchMs    = now;
    dispatchSigHttp(sig);
  }
}

} // namespace HttpBridge
