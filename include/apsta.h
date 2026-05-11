#pragma once
// =============================================================================
// apsta.h — Transient APSTA mode helpers
// =============================================================================
//
// Provides two levels of helper for temporarily adding STA capability while
// keeping the SoftAP (and its associated clients) running:
//
//   withStaInterface(fn)  — enables WIFI_MODE_APSTA (STA radio active, NOT
//                           connected to any network), runs fn(), then restores
//                           AP-only mode.  Used by the WiFi scan handler.
//                           Template lives in the header so it can be
//                           instantiated with any callable at the call site.
//
//   enterApsta(networks)  — switches to APSTA and connects the STA to the
//   exitApsta()             best available saved WiFi network.  Used by the
//                           diagnostic endpoints and future outbound features.
//                           enterApsta() restores AP-only on failure before
//                           returning, so the caller never needs to call
//                           exitApsta() after a failed enterApsta().
//
// Both helpers always restore WIFI_MODE_AP on exit, even on error.
//
// Note: all functions block the calling task for the duration (same as the
// existing /scan and /pair handlers).  This is intentional and acceptable in
// the CONFIG mode HTTP server context.
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <vector>

#include "config_store.h"

namespace Apsta {

// ---------------------------------------------------------------------------
// ConnectResult — returned by enterApsta()
// ---------------------------------------------------------------------------
struct ConnectResult {
  bool   success;   // true iff STA is now connected and has an IP
  String ssid;      // SSID of the network we connected to (empty on failure)
  String ip;        // IP address as a dotted-decimal string (empty on failure)
  String error;     // human-readable failure reason (empty on success):
                    //   "No WiFi network configured"
                    //   "Cannot find <SSID>"
                    //   "Authentication failed"
                    //   "Connection timeout"
};

// ---------------------------------------------------------------------------
// withStaInterface — RAII mode-switching wrapper (template)
// ---------------------------------------------------------------------------
// Switches to WIFI_MODE_APSTA (STA radio up, not connected to any network),
// runs fn(), then restores the previous WiFi mode.  Mode is always restored
// even if fn() returns early.
//
// Usage (lambda must be void or return a value):
//   Apsta::withStaInterface([&]() {
//     int n = WiFi.scanNetworks(false, false);
//     // ... use results, call server.send() ...
//   });
//
// Works for both void and non-void callables (C++14 allows `return fn();`
// in a void-returning function when fn() returns void).
template <typename F>
auto withStaInterface(F&& fn) -> decltype(fn()) {
  bool needsSwitch = (WiFi.getMode() == WIFI_MODE_AP);
  if (needsSwitch) {
    WiFi.mode(WIFI_MODE_APSTA);
  }
  // RAII guard: destructor restores AP-only mode after fn() returns.
  // Safe in Arduino C++ because exceptions are disabled — no throw path.
  struct Guard {
    bool restore;
    ~Guard() { if (restore) WiFi.mode(WIFI_MODE_AP); }
  } guard{needsSwitch};
  return fn();
}

// ---------------------------------------------------------------------------
// enterApsta — connect STA to the best saved home WiFi network
// ---------------------------------------------------------------------------
// Switches to WIFI_MODE_APSTA and tries each saved network in order until
// one connects.  Per-network time budget is timeout_ms / count (min 3 s).
//
// On failure: restores WIFI_MODE_AP before returning; caller does NOT need
// to call exitApsta().
// On success: stays in APSTA; caller MUST call exitApsta() when done.
ConnectResult enterApsta(const std::vector<WifiCredential>& networks,
                         uint32_t timeout_ms = 12000);

// ---------------------------------------------------------------------------
// exitApsta — disconnect STA and return to AP-only mode
// ---------------------------------------------------------------------------
// Safe to call at any time; no-op if already in AP-only mode.
void exitApsta();

} // namespace Apsta
