#pragma once
// =============================================================================
// ble_keyboard.h — Public interface for the BLE HID keyboard central module
// =============================================================================
//
// This module manages the complete lifecycle of a BLE *central* (client)
// connection to a paired HID keyboard:
//
//   Pairing:      SMP Just Works (IO_NO_INPUT_OUTPUT, MITM=false).  On success
//                 NimBLE stores a Long Term Key (LTK) and Identity Resolving
//                 Key (IRK) in NVS flash.  The paired address is also saved as
//                 the "preferred bonded keyboard" and used for all future
//                 auto-connect attempts.
//
//   Reconnection: connectToKeyboard() reuses the stored LTK to re-encrypt the
//                 connection without user interaction.  If the keyboard uses a
//                 resolvable private address (RPA), the IRK lets NimBLE resolve
//                 it back to the original identity address.
//
//   HID input:    subscribeToKeyboard() walks the remote GATT table, finds the
//                 HID service (0x1812) and its Report characteristics (0x2A4D),
//                 and subscribes to notifications.  The HID Boot Protocol
//                 input report is an 8-byte array; bytes 2–7 are key codes.
//
//   Auto-connect: maybeAutoConnectBondedKeyboard() performs a short BLE scan
//                 every AUTO_CONNECT_INTERVAL_MS ms and reconnects automatically
//                 when the preferred bonded keyboard is seen.
//
// All blocking operations run on the calling thread.  The main loop calls
// maybeAutoConnectBondedKeyboard() and syncConnectionState() each iteration.
// =============================================================================

#include <Arduino.h>
#include <NimBLEDevice.h>

namespace BLEKeyboard {

// Callback type for log messages.  Receives a human-readable line and should
// pass it to KeyLog::add() or Serial.println().
using LogFn = void (*)(const String& line);

// Callback type invoked on every key press.  Receives the raw HID usage-page
// key code.  Must return quickly — use a queue for any blocking work.
using KeyPressFn = void (*)(uint8_t keyCode);

// Initialise internal state and store the callbacks.  Must be called once in
// setup(), after NimBLEDevice::init() has been called by the caller.
void begin(LogFn logFn, KeyPressFn keyPressFn);

// Returns true if the given Bluetooth address already has a bond entry in
// NimBLE's NVS store.  Used by BleScanner to annotate scan results.
bool isBondedAddress(const String& address);

// Returns true if the discovered device's advertising flags indicate it is in
// Limited Discoverable mode — i.e. the keyboard is ready to accept new bonds.
// Most BLE keyboards only set this flag for ~30 s after entering pairing mode.
bool isAdvertisedAsPairingMode(NimBLEAdvertisedDevice& d);

// Read the preferred bonded keyboard address and name from NVS and cache them
// in RAM so repeated calls to preferredBondedAddress() are cheap.
// Call once after ConfigStore::load() in setup().
void refreshPreferredBondedDevice();

// Cached address of the keyboard chosen as the preferred auto-connect target.
// Returns a reference to an empty string if no keyboard has been paired yet.
const String& preferredBondedAddress();

// Cached display name corresponding to preferredBondedAddress().
const String& preferredBondedName();

// Erase the preferred bonded keyboard from NVS and clear the in-RAM cache.
// Called during factory reset so the next boot starts fresh.
void clearPreferredBondedDevice();

// Pair with the keyboard at address.  The keyboard must be advertising in
// pairing mode (Limited Discoverable).  On success the bond is stored in NVS
// and the keyboard becomes the new preferred bonded device.
// Returns true on success, false if the keyboard rejects the bond or a timeout
// occurs.
bool pairKeyboard(const String& address, const String& nameHint);

// Remove the NimBLE bond for address using a three-strategy exhaustive search
// (PUBLIC type, RANDOM type, string-walk of bond list) so no stale bond is
// left in NVS regardless of the address type used when pairing.
// Returns true if a bond was found and removed.
bool unpairKeyboard(const String& address);

// Connect to a previously bonded keyboard at address, restore BLE encryption
// using the stored LTK, subscribe to HID input report notifications, and set
// the keyboard as the active connected device.
// Blocks until connected and subscribed, or until an internal timeout.
// Returns true on success.
bool connectToKeyboard(const String& address, const String& nameHint);

// Gracefully disconnect from the currently connected keyboard without removing
// its bond.  Auto-connect will attempt to reconnect on the next scan cycle.
void disconnectKeyboard();

// Called from the main loop once per iteration.  If AUTO_CONNECT_INTERVAL_MS
// has elapsed since the last attempt, performs a ~2 s BLE scan; if the
// preferred bonded keyboard is seen, calls connectToKeyboard() automatically.
void maybeAutoConnectBondedKeyboard();

// Checks NimBLE's live connection state and updates the cached isConnected flag.
// Detects unexpected disconnections (e.g. keyboard powered off) so that the
// LED logic and main loop see the change immediately.
void syncConnectionState();

// Returns true if a keyboard is currently connected and BLE encryption is active.
bool isConnected();

// Display name of the currently connected keyboard, or empty string if none.
const String& connectedName();

// Bluetooth address of the currently connected keyboard, or empty string if none.
const String& connectedAddress();

// Most recently received HID usage-page key code.  Returns 0 if no key has
// been pressed since the last connection was established.
uint8_t lastKeyCode();

} // namespace BLEKeyboard
