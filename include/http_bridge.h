#pragma once
// =============================================================================
// http_bridge.h — BLE burst-signature to HTTP GET forwarding bridge
// =============================================================================
//
// Decouples the time-critical BLE notification callback from the potentially
// slow HTTP dispatch path.
//
// The problem:
//   BLE HID notifications arrive on the NimBLE internal task and must be
//   processed quickly.  An HTTP GET can block for anywhere from a few
//   milliseconds to several seconds (DNS, TCP handshake, server latency).
//   Blocking the NimBLE task during HTTP would starve the BLE stack and
//   cause the keyboard connection to drop.
//
// The solution:
//   onSigPress()         — non-blocking enqueue called from the BLE callback.
//   processPendingSigs() — drains the queue and fires HTTP GETs; called from
//                           the Arduino main loop, which can safely block.
//
// Burst-repeat suppression:
//   onSigPress() applies a 120 ms window duplicate filter.  If the same
//   signature arrives within 120 ms of the previous enqueue it is treated as
//   a duplicate burst and silently dropped.  This prevents a single long
//   button-hold from flooding the HTTP endpoint with dozens of identical GETs.
//
// Dependency injection:
//   All "knowledge" this module needs (base URL, path mapping, log function,
//   GET lifecycle callbacks) is provided by the caller via function pointers
//   at begin()/setGetCallbacks() time.  This keeps the module self-contained
//   and independent of the global application state in main.cpp.
// =============================================================================

#include <Arduino.h>

namespace HttpBridge {

// Log callback — receives human-readable event lines; forward to KeyLog::add().
using LogFn = void (*)(const String& line);

// Returns the current base URL from the caller's config state.
// Queried lazily at dispatch time so URL changes take effect immediately.
using BaseUrlFn = String (*)();

// Function type that maps a burst signature to a relative URL path.
// Return an empty string if no mapping is found (the press is silently dropped).
using MappedPathFn = String (*)(const String& signature);

// Optional callback fired just before HTTPClient::GET() is called.
// Used by main.cpp to arm the D3 network LED start pulse.
using GetStartFn = void (*)();

// Optional callback fired after HTTPClient::GET() returns with the HTTP
// status code (200, 404, -1 for timeout, etc.).
// Used by main.cpp to arm the D3 network LED result blink.
using GetResultFn = void (*)(int statusCode);

// Inject core dependencies.  Must be called once from setup() before any
// other function in this module.
void begin(LogFn logFn, BaseUrlFn baseUrlFn, MappedPathFn mappedPathFn);

// Register optional LED-control callbacks for HTTP GET lifecycle events.
// Separated from begin() so begin() can be called earlier in setup() before
// the LED variables are initialised.
void setGetCallbacks(GetStartFn getStartFn, GetResultFn getResultFn);

// Enqueue a burst signature for HTTP dispatch.
// Call this from the BLE notification callback (or any task).
// The actual HTTP request is sent from processPendingSigs() in the main loop.
void onSigPress(const String& signature);

// Process one pending signature dispatch per call.
// Call from loop() — does nothing if the queue is empty or WiFi is down.
void processPendingSigs();

} // namespace HttpBridge
