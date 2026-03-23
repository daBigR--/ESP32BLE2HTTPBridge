#pragma once
// =============================================================================
// http_bridge.h — BLE key-press to HTTP GET forwarding bridge
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
//   onKeyPress()         — non-blocking enqueue called from the BLE callback.
//   processPendingKeys() — drains the queue and fires HTTP GETs; called from
//                           the Arduino main loop, which can safely block.
//
// Key-repeat suppression:
//   onKeyPress() applies a 120 ms window duplicate filter.  If the same key
//   code arrives within 120 ms of the previous enqueue it is treated as
//   keyboard auto-repeat and silently dropped.  This prevents a single long
//   key-hold from flooding the HTTP endpoint with dozens of identical GETs.
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

// Given a HID key code, returns the mapped relative URL path (or empty string
// if the key has no mapping, in which case the HTTP GET is skipped).
using MappedPathFn = String (*)(uint8_t keyCode);

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

// Enqueue a key press for later HTTP dispatch.  Non-blocking and safe to call
// from a BLE notification callback or any RTOS task.  If the internal queue
// is already at its maximum size (24 entries), the oldest entry is evicted to
// make room.
void onKeyPress(uint8_t keyCode);

// Drain the pending key queue, applying the repeat filter and firing one
// HTTP GET per remaining entry.  Must be called from the main loop.
// Blocks until all pending GETs have completed or timed out.
void processPendingKeys();

} // namespace HttpBridge
