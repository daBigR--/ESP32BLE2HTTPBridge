#pragma once
// =============================================================================
// ble_scanner.h — One-shot BLE device scanner for the web UI
// =============================================================================
//
// Provides a single active BLE scan that enriches its results with bond-store
// information from the BLEKeyboard module.
//
// Purpose: The web UI's keyboard discovery flow needs to show the user a list
// of nearby BLE keyboards annotated with whether each one is already bonded
// and whether it is currently in pairing mode.  This module handles that scan
// and builds the annotated JSON payload.
//
// Scan strategy:
//   - Active scan (sends scan-request packets) to retrieve the device name
//     from the Scan Response, which many keyboards only include there.
//   - Results are filtered to devices that are either bonded or in pairing
//     mode, to avoid filling the UI with random Bluetooth devices.
//   - Devices that are bonded but were not seen during the scan are still
//     included with rssi=-127, allowing the user to unpair them from the web
//     UI even when the physical keyboard is not nearby.
// =============================================================================

#include <Arduino.h>

namespace BleScanner {

// Perform an active BLE scan (~4 s) and store the results internally.
// Blocks until the scan completes.  Must be called before devicesJson().
void performScan();

// Return the most recent scan results as a JSON array.
// Each element is an object:
//   { "addr": "xx:xx:xx:xx:xx:xx",
//     "name": "<device display name or empty>",
//     "rssi": <signal strength in dBm, or -127 for bonded-but-not-seen>,
//     "bonded": <true if NimBLE has a stored bond for this address>,
//     "pairableNow": <true if device is in Limited Discoverable pairing mode> }
String devicesJson();

} // namespace BleScanner