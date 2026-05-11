// =============================================================================
// apsta.cpp — Transient APSTA mode implementation
// =============================================================================

#include "apsta.h"

namespace Apsta {

// ---------------------------------------------------------------------------
// enterApsta
// ---------------------------------------------------------------------------
// Algorithm:
//   1. Guard: reject empty network list immediately.
//   2. Switch to APSTA so the SoftAP keeps running while STA connects.
//   3. For each saved network: call WiFi.begin(), poll status for up to
//      perNetMs ms, break on terminal state.
//   4. On success: return with STA still connected (caller calls exitApsta).
//   5. On failure: restore AP-only mode before returning.
//
// Terminal states per WiFi.status():
//   WL_CONNECTED      — success, done
//   WL_NO_SSID_AVAIL  — SSID not found; try next network
//   WL_CONNECT_FAILED — authentication error; try next network
//   (anything else after timeout) — move on with "Connection timeout" reason
// ---------------------------------------------------------------------------
ConnectResult enterApsta(const std::vector<WifiCredential>& networks,
                         uint32_t timeout_ms) {
  if (networks.empty()) {
    return {false, "", "", "No WiFi network configured"};
  }

  // Switch to APSTA — SoftAP stays up, STA interface is added.
  WiFi.mode(WIFI_MODE_APSTA);
  WiFi.disconnect(); // drop any leftover STA state
  delay(100);

  // Divide budget equally; floor at 3000 ms so fast failures don't rush us.
  uint32_t perNetMs = (uint32_t)(timeout_ms / networks.size());
  if (perNetMs < 3000) perNetMs = 3000;

  String lastError = "Connection timeout";

  for (const auto& net : networks) {
    WiFi.begin(net.ssid.c_str(), net.password.c_str());

    unsigned long deadline = millis() + perNetMs;
    wl_status_t   st       = WL_IDLE_STATUS;

    while (millis() < deadline) {
      st = WiFi.status();
      if (st == WL_CONNECTED)      { break; }
      if (st == WL_NO_SSID_AVAIL) { lastError = String("Cannot find ") + net.ssid; break; }
      if (st == WL_CONNECT_FAILED) { lastError = "Authentication failed"; break; }
      delay(100);
    }

    if (st == WL_CONNECTED) {
      // Success — caller is responsible for calling exitApsta() when done.
      return {true, net.ssid, WiFi.localIP().toString(), ""};
    }

    // This network failed — disconnect cleanly before trying the next one.
    WiFi.disconnect();
    delay(200);
  }

  // All networks exhausted — restore AP-only before returning failure.
  WiFi.mode(WIFI_MODE_AP);
  return {false, "", "", lastError};
}

// ---------------------------------------------------------------------------
// exitApsta
// ---------------------------------------------------------------------------
void exitApsta() {
  wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_MODE_APSTA || mode == WIFI_MODE_STA) {
    WiFi.disconnect(); // disconnect STA; does not affect AP
    WiFi.mode(WIFI_MODE_AP);
  }
}

} // namespace Apsta
