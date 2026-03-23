#pragma once
// =============================================================================
// web_ble_api.h — HTTP routes for BLE keyboard management
// =============================================================================
//
// Registers GET endpoints on the WebServer instance that allow the browser-
// based web UI to perform all BLE keyboard operations.
//
// Routes registered by registerRoutes():
//   GET /scan        — ~4 s active BLE scan; returns JSON device list
//   GET /pair        — pair with keyboard at ?addr= (must be in pairing mode)
//   GET /connect     — reconnect to bonded keyboard at ?addr=
//   GET /unpair      — remove the bond for ?addr=
//   GET /disconnect  — drop the current BLE connection
//   GET /state       — return connection state + recent event log (polled by UI)
// =============================================================================

#include <WebServer.h>

namespace WebBleApi {

// Register all BLE API routes on the given server instance.
// Must be called during setup() before server.begin().
void registerRoutes(WebServer& server);

} // namespace WebBleApi