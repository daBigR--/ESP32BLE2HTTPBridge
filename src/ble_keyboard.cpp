// =============================================================================
// ble_keyboard.cpp — BLE central (client) for HID keyboard devices
// =============================================================================
//
// Responsibilities
// ----------------
// This module manages the full lifecycle of a BLE keyboard connection:
//
//   Pairing:     Scan for a keyboard advertising in pairing mode, connect,
//                run the BLE pairing/bonding handshake, and store the bond.
//                After bonding, immediately disconnect so the keyboard can
//                enter its normal (non-pairing) advertising state.
//
//   Connecting:  Reconnect to a previously bonded keyboard.  Walk the GATT
//                table to find the HID service and subscribe to all input-
//                report characteristics, so key presses arrive as BLE
//                notifications.
//
//   Auto-connect: Periodically scan for the preferred bonded keyboard and
//                 reconnect if it reappears after a disconnect.
//
//   Key delivery: Parse incoming HID boot-protocol reports (8 bytes:
//                 modifiers, reserved, up to 6 key codes) and forward each
//                 non-zero key code to the registered KeyPressFn callback.
//
// BLE roles
// ---------
// This device is a *central* (GATT client).  The keyboard is the *peripheral*
// (GATT server) that advertises HID services.  NimBLE is used instead of the
// bundled ESP-IDF BLE because it is lighter (~100 kB smaller) and offers a
// more convenient C++ API for the central role.
//
// Security model
// --------------
// Keyboards carry no display or keypad, so the "Just Works" pairing model
// is used (IO capability = NO_INPUT_OUTPUT, MITM = false).  This produces
// an Unauthenticated Encrypted bond — traffic is encrypted (sniffing
// requires active hardware on the radio link) but not authenticated against
// MITM.  For a keyboard hub in a home environment this is an acceptable
// trade-off between usability and security.
//
// Bonding persists across reboots because NimBLE stores the Long-Term Key
// (LTK) and Identity Resolving Key (IRK) in the device's NVS flash.
// The IRK is particularly important: modern BLE peripherals rotate their
// Bluetooth address every few minutes (Resolvable Private Addresses, RPA)
// to prevent tracking.  With the IRK stored, the central can resolve the
// rotating address back to the original identity and reconnect automatically.
// =============================================================================

#include "ble_keyboard.h"

#include <vector>

#define HID_SERVICE_UUID      "1812"
#define HID_INPUT_REPORT_UUID "2A4D"

// ---------------------------------------------------------------------------
// Anonymous namespace — all symbols here are private to this translation unit
// ---------------------------------------------------------------------------
namespace {

// Active NimBLE client object.  nullptr when not connected; created by
// NimBLEDevice::createClient() and destroyed by NimBLEDevice::deleteClient().
NimBLEClient* gClient = nullptr;

// Mirrors the NimBLE connection state but is set/cleared synchronously in the
// ClientCallbacks so other code can read it without querying the BLE stack.
bool gConnected = false;

// Name and address of the currently connected keyboard (kept for the web UI
// status endpoint).  Cleared in disconnectKeyboard().
String gConnectedName    = "";
String gConnectedAddress = "";

// Count of successfully subscribed HID input characteristics.
// Stored after connect to aid diagnosis (logged if subscription fails).
size_t gSubscribedCharacteristicCount = 0;

// The address (and optional name) of the keyboard we prefer to auto-connect
// to.  Persisted indirectly: refreshPreferredBondedDevice() restores it from
// NimBLE's bond store on every boot.
String gPreferredBondedAddress = "";
String gPreferredBondedName    = "";

// Timestamp of the last auto-connect attempt.  Compared against millis() to
// enforce the quiet period between retries so BLE scanning does not saturate
// the radio and disrupt WiFi.
unsigned long gLastAutoConnectAttemptMs = 0;
const unsigned long AUTO_CONNECT_INTERVAL_MS = 8000; // 8 s between retries
const unsigned long POST_PAIR_RECONNECT_DELAY_MS = 3000; // settle time for finicky remotes

// When false, maybeAutoConnectBondedKeyboard() is a no-op.  Disabled by the
// web UI /scan handler to prevent scanner contention; re-enabled automatically
// after a successful pair or by an explicit setAutoConnectEnabled(true) call.
bool gAutoConnectEnabled = true;

// Recently paired target gets first auto-connect priority so we reconnect the
// device the user just paired even if preferred-bond canonicalization is
// ambiguous for that cycle.
String gPendingReconnectAddress = "";
String gPendingReconnectName    = "";

// Last non-zero key code received via HID notification (exposed through
// lastKeyCode() for the web UI status endpoint).
uint8_t gLastKeyCode = 0;

// Function pointers injected by begin().  Separated from the module
// implementation so the caller can supply its own log sink and key handler.
BLEKeyboard::LogFn      gLogFn      = nullptr;
BLEKeyboard::KeyPressFn gKeyPressFn = nullptr;

// Convenience wrapper so every log site reads cleanly.
void addKeyLog(const String& line) {
  if (gLogFn) {
    gLogFn(line);
  }
}

// ---------------------------------------------------------------------------
// SecurityCallbacks — handles SMP (Security Manager Protocol) events
// ---------------------------------------------------------------------------
// NimBLE fires these callbacks during the pairing/bonding handshake.
// Because we use IO capability = NO_INPUT_OUTPUT the stack never actually
// asks the user to confirm a passkey, but the callbacks must exist and
// return sensible values or the handshake fails.
class SecurityCallbacks : public NimBLESecurityCallbacks {
  // Invoked when the peripheral requests a passkey from us.
  // With Just Works this should never happen, but returning 0 is harmless.
  uint32_t onPassKeyRequest() override {
    addKeyLog("Passkey requested; returning 000000");
    return 0;
  }

  // Informational: peripheral is displaying a passkey for the user to confirm.
  // With Just Works this is skipped, but log it if it ever occurs.
  void onPassKeyNotify(uint32_t pass_key) override {
    addKeyLog(String("Passkey notify: ") + String(pass_key));
  }

  // Numeric Comparison pairing: should we accept the displayed PIN?
  // Always return true; MITM protection is not required in our threat model.
  bool onConfirmPIN(uint32_t pass_key) override {
    addKeyLog(String("Confirm PIN: ") + String(pass_key));
    return true;
  }

  // Should we accept a security / encryption request from the peripheral?
  // Always yes.
  bool onSecurityRequest() override {
    addKeyLog("Security request accepted");
    return true;
  }

  // Called after the pairing handshake completes (successfully or not).
  // Log the resulting security state for diagnosis.
  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    if (!desc) {
      addKeyLog("Authentication complete: no descriptor");
      return;
    }
    if (desc->sec_state.encrypted) {
      String line = "Auth complete: encrypted=yes";
      line += desc->sec_state.authenticated ? " authenticated=yes" : " authenticated=no";
      line += desc->sec_state.bonded        ? " bonded=yes"        : " bonded=no";
      addKeyLog(line);
    } else {
      addKeyLog("Pairing failed (not encrypted)");
    }
  }
};

// Forward declarations for functions defined later in this anonymous namespace.
void disconnectKeyboard();
void logConnectionSecurity(const String& prefix);
bool openKeyboardLink(const String& address, const String& nameHint);
bool canPairDeviceNow(const String& address, String& reason);
bool removeBondByAddress(const String& address);
void pruneBondsExcept(const String& keepAddress);

// ---------------------------------------------------------------------------
// notifyCallback — HID input-report notification handler
// ---------------------------------------------------------------------------
// Invoked by NimBLE (from its internal task) each time the keyboard sends a
// key-press or key-release notification on a subscribed HID Report
// characteristic.
//
// HID Boot Protocol keyboard report format (8 bytes):
//   Byte 0: modifier keys bitmask (Shift, Ctrl, Alt, …)
//   Byte 1: reserved (always 0x00)
//   Bytes 2–7: up to 6 simultaneously pressed key codes (0x00 = no key)
//
// We start at byte 2 and forward the first non-zero key code to the
// registered handler.  Only one code per notification is forwarded to
// keep the upstream queue simple; chords are rare on a media remote.
static void notifyCallback(NimBLERemoteCharacteristic* pRemoteCharacteristic,
                           uint8_t* pData,
                           size_t length,
                           bool isNotify) {
  (void)pRemoteCharacteristic; // not used; notification is self-describing
  if (!isNotify || length < 3) {
    return; // ignore indication ACKs and malformed reports
  }

  for (int i = 2; i < (int)length && i < 8; i++) {
    if (pData[i] == 0) {
      continue; // key slot empty
    }
    gLastKeyCode = pData[i];
    if (gKeyPressFn) {
      gKeyPressFn(pData[i]); // call the main application handler
    }
    // Log as uppercase hex for readability (e.g. "KEY 0x28" = Enter).
    String line = "KEY 0x";
    if (pData[i] < 0x10) {
      line += "0"; // zero-pad single-hex-digit values
    }
    line += String(pData[i], HEX);
    addKeyLog(line);
    break; // only forward the first key in the report
  }
}

// ---------------------------------------------------------------------------
// ClientCallbacks — handles connection lifecycle events
// ---------------------------------------------------------------------------
class ClientCallbacks : public NimBLEClientCallbacks {
  // Called by NimBLE when the GAP connection is established (before
  // security / GATT work; the link is not yet encrypted at this point).
  void onConnect(NimBLEClient* pClient) override {
    (void)pClient;
    gConnected = true;
    addKeyLog("Connected");
  }

  // Called when the link is dropped for any reason (keyboard powered off,
  // moved out of range, BLE stack timeout, etc.).
  void onDisconnect(NimBLEClient* pClient) override {
    (void)pClient;
    gConnected = false;
    addKeyLog("Disconnected");
  }

  // The peripheral has requested a connection parameter update (interval,
  // latency, supervision timeout).  Returning true accepts the update;
  // we always accept because the keyboard's preferred parameters are
  // tuned for its battery and latency requirements.
  bool onConnParamsUpdateRequest(NimBLEClient* pClient, const ble_gap_upd_params* params) override {
    (void)pClient;
    (void)params;
    return true;
  }

  // Called after the SMP pairing/encryption handshake completes on an
  // already-established connection (i.e. on reconnect using a stored bond).
  // If encryption failed we close the link immediately; leaving an
  // unencrypted HID connection open would mean the keyboard is essentially
  // unauthenticated and could be impersonated.
  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    if (!desc) {
      addKeyLog("Auth callback missing descriptor");
      return;
    }
    if (!desc->sec_state.encrypted) {
      addKeyLog("Auth failed, disconnecting");
      NimBLEClient* c = NimBLEDevice::getClientByID(desc->conn_handle);
      if (c) {
        c->disconnect();
      }
      return;
    }
    addKeyLog("Auth success");
  }
};

// ---------------------------------------------------------------------------
// subscribeToKeyboard — GATT service discovery and HID notification setup
// ---------------------------------------------------------------------------
// Called after a secure connection has been established.  Walks the GATT
// attribute table of the connected keyboard, finds the HID service
// (UUID 0x1812) and subscribes to all input-report characteristics that
// support Notify or Indicate.
//
// Why we subscribe to ALL input characteristics instead of just one:
//   Some keyboards (especially multi-function ones) report consumer-control
//   keys (play/pause, volume, etc.) on a separate characteristic from the
//   standard boot-protocol keyboard report.  Subscribing to both 0x2A22
//   (boot keyboard input) and 0x2A4D (HID report) catches all key events.
//
// The 500 ms delays before and after discoverAttributes() are empirical:
//   • Some keyboards need a short pause after connection before they respond
//     to attribute requests (their GATT server is still initialising).
//   • Discovery can take several round-trips over the BLE link; the delay
//     after discovery lets the internal cache stabilise.
bool subscribeToKeyboard() {
  if (!gClient || !gClient->isConnected()) {
    return false;
  }

  gSubscribedCharacteristicCount = 0;

  addKeyLog("Discovering services...");
  delay(500); // let the keyboard's GATT server finish initialising

  if (!gClient->discoverAttributes()) {
    addKeyLog("Failed to discover attributes");
    return false;
  }

  delay(500); // allow the attribute cache to settle

  std::vector<NimBLERemoteService*>* services = gClient->getServices();
  if (!services) {
    addKeyLog("No services found after discovery");
    return false;
  }

  NimBLERemoteService* hidService = gClient->getService(HID_SERVICE_UUID);
  if (!hidService) {
    addKeyLog("HID service not found after discovery");
    return false;
  }

  std::vector<NimBLERemoteCharacteristic*>* hidChars = hidService->getCharacteristics();
  if (!hidChars) {
    addKeyLog("HID service has no characteristics");
    return false;
  }

  addKeyLog("Subscribing to HID input characteristics...");
  for (auto ch : *hidChars) {
    String charUUID = String(ch->getUUID().toString().c_str());
    // Accept both the boot keyboard input (0x2A22) and the generic HID report
    // (0x2A4D) characteristics.  NimBLE may return the UUID in "0x2a4d" or
    // "2a4d" form depending on the device, so we check both variants.
    bool isKeyboardInput =
      charUUID.equalsIgnoreCase("0x2a22") ||
      charUUID.equalsIgnoreCase("2a22")   ||
      charUUID.equalsIgnoreCase("0x2a4d") ||
      charUUID.equalsIgnoreCase("2a4d");
    bool canSignal = ch->canNotify() || ch->canIndicate();

    if (!isKeyboardInput || !canSignal) {
      continue; // skip non-keyboard or non-notifiable characteristics
    }

    // Prefer Notify over Indicate: Notify is fire-and-forget (lower latency);
    // Indicate requires an acknowledgement from us on every key event.
    bool preferNotify = ch->canNotify();
    if (ch->subscribe(preferNotify, notifyCallback)) {
      gSubscribedCharacteristicCount++;
      addKeyLog(String("Subscribed: ") + charUUID);
    } else {
      addKeyLog(String("Subscribe failed: ") + charUUID);
    }
  }

  if (gSubscribedCharacteristicCount == 0) {
    addKeyLog("Could not subscribe to any HID input characteristic");
    return false;
  }

  addKeyLog(String("Subscribed HID characteristics: ") + String(gSubscribedCharacteristicCount));
  return true;
}

// ---------------------------------------------------------------------------
// logConnectionSecurity — diagnostic helper
// ---------------------------------------------------------------------------
// Reads the current connection's security state from NimBLE's connection-info
// structure and writes a single-line summary to the key log.  Useful for
// confirming that encryption and bonding are in place after connect/pair.
void logConnectionSecurity(const String& prefix) {
  if (!gClient || !gClient->isConnected()) {
    addKeyLog(prefix + ": no active connection");
    return;
  }

  NimBLEConnInfo info = gClient->getConnInfo();
  String line = prefix;
  line += " encrypted=";    line += info.isEncrypted()    ? "yes" : "no";
  line += " authenticated="; line += info.isAuthenticated() ? "yes" : "no";
  line += " bonded=";        line += info.isBonded()        ? "yes" : "no";
  addKeyLog(line);
}

// ---------------------------------------------------------------------------
// removeBondByAddress — exhaustively removes a bond from NimBLE's store
// ---------------------------------------------------------------------------
// NimBLE can store an address as PUBLIC or RANDOM type.  Because we may not
// always know which type the keyboard used when it bonded, we try all three
// strategies:
//   1. Delete by explicit PUBLIC type address.
//   2. Delete by explicit RANDOM type address.
//   3. Iterate the bond list and delete any entry whose string representation
//      matches (case-insensitive), catching any remaining type variants.
bool removeBondByAddress(const String& address) {
  bool removed = false;

  if (NimBLEDevice::deleteBond(NimBLEAddress(address.c_str(), BLE_ADDR_PUBLIC))) {
    removed = true;
  }
  if (NimBLEDevice::deleteBond(NimBLEAddress(address.c_str(), BLE_ADDR_RANDOM))) {
    removed = true;
  }

  // Walk the bond list in reverse so index-based deletion is safe.
  for (int i = NimBLEDevice::getNumBonds() - 1; i >= 0; i--) {
    NimBLEAddress bonded    = NimBLEDevice::getBondedAddress(i);
    String        bondedAddr = String(bonded.toString().c_str());
    if (!bondedAddr.equalsIgnoreCase(address)) {
      continue;
    }
    if (NimBLEDevice::deleteBond(bonded)) {
      removed = true;
    }
  }

  return removed;
}

// Keep only the specified bonded device and remove all others.
void pruneBondsExcept(const String& keepAddress) {
  std::vector<String> toRemove;
  const int bondCount = NimBLEDevice::getNumBonds();
  toRemove.reserve(bondCount);
  for (int i = 0; i < bondCount; i++) {
    String bondedAddr = String(NimBLEDevice::getBondedAddress(i).toString().c_str());
    if (!bondedAddr.equalsIgnoreCase(keepAddress)) {
      toRemove.push_back(bondedAddr);
    }
  }

  for (const String& addr : toRemove) {
    if (removeBondByAddress(addr)) {
      addKeyLog(String("Removed old bond: ") + addr);
    }
  }
}

// ---------------------------------------------------------------------------
// canPairDeviceNow — checks whether a device is currently in pairing mode
// ---------------------------------------------------------------------------
// Before attempting to pair (which involves a connection + SMP handshake),
// we perform a short scan to verify the device is:
//   a) currently advertising (visible), AND
//   b) advertising with the "Limited Discoverable" or "General Discoverable"
//      LE flag set — which most keyboards only set when the user has
//      explicitly triggered pairing mode (e.g. holding a pairing button).
//
// Pairing without this check often fails because a keyboard that is NOT in
// pairing mode will reject the SMP pairing request even though it accepts
// the GAP connection (it expects to resume an existing bond instead).
bool canPairDeviceNow(const String& address, String& reason) {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setInterval(80);
  scan->setWindow(40);
  scan->setActiveScan(true);
  scan->clearResults();

  NimBLEScanResults results = scan->start(4, false); // 4 s scan
  for (int i = 0; i < results.getCount(); i++) {
    NimBLEAdvertisedDevice d = results.getDevice(i);
    String found = String(d.getAddress().toString().c_str());
    if (!found.equalsIgnoreCase(address)) {
      continue;
    }

    uint8_t flags   = d.getAdvFlags();
    uint8_t advType = d.getAdvType();
    bool    directed = d.haveTargetAddress() || advType == 1 || advType == 4;
    if (BLEKeyboard::isAdvertisedAsPairingMode(d)) {
      reason = "ok";
      return true;
    }

    // Device found but not in pairing mode — give a diagnostic reason string.
    reason = String("device not advertising for new pairing (advType=") +
             String(advType) + String(", flags=0x") + String(flags, HEX) +
             String(", directed=") + String(directed ? 1 : 0) + String(")");
    return false;
  }

  reason = "device not currently advertising";
  return false;
}

// ---------------------------------------------------------------------------
// disconnectKeyboard (internal helper)
// ---------------------------------------------------------------------------
// Tears down the active BLE connection and destroys the client object.
// Always resets the connection-state variables so other code sees a clean
// "not connected" state immediately, regardless of whether the link was
// already down before this call.
//
// The 500 ms wait after disconnect() lets the BLE stack finish tearing down
// the link before deleteClient() frees the client object.  Calling
// deleteClient while the radio is still processing a disconnect can corrupt
// NimBLE's internal state and cause hard-to-reproduce reconnect failures.
// If the link does not drop within 500 ms we abandon the client pointer
// rather than risk a crash — the memory leaks but the state is at least
// consistent.
void disconnectKeyboard() {
  if (gClient) {
    if (gClient->isConnected()) {
      gClient->disconnect();
      // Wait up to 500 ms for the BLE stack to finish the teardown.
      unsigned long t = millis();
      while (gClient->isConnected() && millis() - t < 500) {
        delay(10);
      }
      if (gClient->isConnected()) {
        // Stack did not complete disconnect in time — abandon the pointer
        // rather than delete a still-active client.
        addKeyLog("Disconnect timeout: client abandoned");
        gClient = nullptr;
        gConnected        = false;
        gConnectedName    = "";
        gConnectedAddress = "";
        return;
      }
    }
    NimBLEDevice::deleteClient(gClient);
    gClient = nullptr;
  }
  gConnected        = false;
  gConnectedName    = "";
  gConnectedAddress = "";
}

// ---------------------------------------------------------------------------
// openKeyboardLink — low-level connect (no security / GATT work)
// ---------------------------------------------------------------------------
// Scans for the target device and establishes a raw GAP connection.  The
// caller is responsible for running the security handshake and GATT
// subscription steps afterwards.
//
// Two-step scan-then-connect strategy:
//   1. Perform a fresh 4-second scan and look for the target address.  If
//      found, pass the discovered device object to gClient->connect() so
//      NimBLE can use the full advertising data (including address type and
//      IRK hint) for a reliable connection.
//   2. If the device is not visible in the scan (it may be in directed-ADV
//      mode aimed at us, which is invisible to active scans), fall back to
//      gClient->connect(NimBLEAddress) which uses the address directly.
bool openKeyboardLink(const String& address, const String& nameHint) {
  disconnectKeyboard(); // ensure clean state before attempting a new connection

  // If the previous client was abandoned (disconnect timeout above), bail
  // immediately rather than layering a new connection attempt on top of a
  // potentially still-active BLE link.
  if (gClient != nullptr) {
    addKeyLog("Connect aborted: previous client still active");
    return false;
  }

  gClient = NimBLEDevice::createClient();
  gClient->setClientCallbacks(new ClientCallbacks(), true); // true = auto-delete
  gClient->setConnectTimeout(10); // seconds; long enough for slow keyboards

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setInterval(80);
  scan->setWindow(40);
  scan->setActiveScan(true);
  scan->clearResults();

  addKeyLog(String("Connecting to ") + address);
  NimBLEAdvertisedDevice* target = nullptr;
  NimBLEScanResults results = scan->start(4, false); // 4 s scan
  for (int i = 0; i < results.getCount(); i++) {
    NimBLEAdvertisedDevice d = results.getDevice(i);
    String found = String(d.getAddress().toString().c_str());
    if (found.equalsIgnoreCase(address)) {
      target = new NimBLEAdvertisedDevice(d); // copy; scan results are invalidated
      break;
    }
  }

  bool ok = false;
  if (target) {
    // Preferred: connect via the full advertising device record so NimBLE
    // can use the address type and resolve RPAs correctly.
    ok = gClient->connect(target, false);
    delete target;
    if (!ok) {
      addKeyLog("Connect via scan record failed, trying direct address");
    }
  }

  if (!ok) {
    // Fallback: the device was not seen in the scan (may be in directed
    // advertising or just powering up), or connect(target) failed.
    // First try NimBLE's generic address constructor (historically the most
    // compatible path), then explicit RANDOM/PUBLIC address types.
    if (!target) {
      addKeyLog("Device not found in fresh scan, trying direct address");
    }
    ok = gClient->connect(NimBLEAddress(address.c_str()), false);
    if (!ok) {
      ok = gClient->connect(NimBLEAddress(address.c_str(), BLE_ADDR_RANDOM), false);
    }
    if (!ok) {
      ok = gClient->connect(NimBLEAddress(address.c_str(), BLE_ADDR_PUBLIC), false);
    }
  }

  if (!ok) {
    addKeyLog("Connect failed");
    NimBLEDevice::deleteClient(gClient);
    gClient = nullptr;
    return false;
  }

  gConnectedName    = nameHint;
  gConnectedAddress = address;
  logConnectionSecurity("Link security"); // log encryption/bond state at GAP level
  return true;
}

} // namespace

namespace BLEKeyboard {

// ---------------------------------------------------------------------------
// begin — module initialisation
// ---------------------------------------------------------------------------
// Must be called once from setup() before any other function in this module.
// Stores the injected function pointers and installs the SecurityCallbacks
// instance that handles SMP events during pairing.
void begin(LogFn logFn, KeyPressFn keyPressFn) {
  gLogFn      = logFn;
  gKeyPressFn = keyPressFn;
  NimBLEDevice::setSecurityCallbacks(new SecurityCallbacks());
}

// ---------------------------------------------------------------------------
// isBondedAddress — checks whether an address is in NimBLE's bond store
// ---------------------------------------------------------------------------
// Tries both PUBLIC and RANDOM address types first (fast path), then falls
// back to a string-compare walk of the bond list to catch any remaining edge
// cases (e.g. address type changed after a firmware update on the keyboard).
bool isBondedAddress(const String& address) {
  if (NimBLEDevice::isBonded(NimBLEAddress(address.c_str(), BLE_ADDR_PUBLIC))) {
    return true;
  }
  if (NimBLEDevice::isBonded(NimBLEAddress(address.c_str(), BLE_ADDR_RANDOM))) {
    return true;
  }
  // Fallback: string-compare walk
  const int bondCount = NimBLEDevice::getNumBonds();
  for (int i = 0; i < bondCount; i++) {
    String bondedAddr = String(NimBLEDevice::getBondedAddress(i).toString().c_str());
    if (bondedAddr.equalsIgnoreCase(address)) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// isAdvertisedAsPairingMode — interprets the LE advertising flags
// ---------------------------------------------------------------------------
// Returns true if the device is advertising as discoverable — either:
//   • Bit 0 (0x01) = LE Limited Discoverable Mode  — used by most keyboards
//     only during an explicit pairing window (typically ~30 s).
//   • Bit 1 (0x02) = LE General Discoverable Mode  — used by some devices
//     (e.g. Boox RemoteControl, Kobo Remote) at all times to indicate they
//     will accept a new bond without a separate pairing-mode trigger.
// Both are valid indicators that the device will accept a fresh bond.
bool isAdvertisedAsPairingMode(NimBLEAdvertisedDevice& d) {
  return (d.getAdvFlags() & 0x03) != 0; // bit 0 = Limited, bit 1 = General
}

// ---------------------------------------------------------------------------
// refreshPreferredBondedDevice — syncs in-RAM preferred address with NVS bonds
// ---------------------------------------------------------------------------
// Called on boot and after any bond change.  If the current preferred address
// is still present in the bond store we keep it; otherwise we fall back to
// the first stored bond (index 0).  This handles the case where the user has
// unpaired, paired a new keyboard, or the bond was deleted by a factory reset.
void refreshPreferredBondedDevice() {
  if (gPreferredBondedAddress.length() > 0 && isBondedAddress(gPreferredBondedAddress)) {
    return; // existing preference is still valid
  }
  // Preference lost — reset and repopulate from bond store.
  gPreferredBondedAddress = "";
  gPreferredBondedName    = "";
  const int bondCount = NimBLEDevice::getNumBonds();
  if (bondCount <= 0) {
    return; // no bonds at all
  }
  NimBLEAddress addr = NimBLEDevice::getBondedAddress(0); // use first bond
  gPreferredBondedAddress = String(addr.toString().c_str());
}

// Simple read-only accessors for the preferred bond — used by the web UI
// status route and by the config-validity check in main.cpp.
const String& preferredBondedAddress() { return gPreferredBondedAddress; }
const String& preferredBondedName()    { return gPreferredBondedName;    }

// Clears the in-RAM preferred device.  Called during factory reset so the
// auto-connect loop does not try to reconnect a bond that has just been erased.
void clearPreferredBondedDevice() {
  gPreferredBondedAddress = "";
  gPreferredBondedName    = "";
}

// ---------------------------------------------------------------------------
// pairKeyboard — full pairing flow
// ---------------------------------------------------------------------------
// High-level function called from the web UI's /pair endpoint.
//
// Flow:
//   1. If already bonded, just record it as the preferred device and return.
//   2. Probe whether the keyboard is currently advertising in pairing mode
//      (advisory only; does not hard-block pairing attempts).
//   3. Open a raw GAP connection via openKeyboardLink().
//   4. Run the SMP pairing/bonding handshake via secureConnection().
//   5. Verify a bond was actually stored — some keyboards accept the SMP
//      exchange but do not persist a bond (unusual but seen in the wild).
//   6. Disconnect immediately: the keyboard will now enter its normal
//      (non-pairing) advertisement mode and the auto-connect path will
//      find and reconnect it.
bool pairKeyboard(const String& address, const String& nameHint) {
  if (isBondedAddress(address)) {
    // Device is already bonded — just adopt it as the preferred device.
    gPreferredBondedAddress = address;
    gPreferredBondedName    = nameHint;
    pruneBondsExcept(gPreferredBondedAddress);
    gPendingReconnectAddress = gPreferredBondedAddress;
    gPendingReconnectName    = gPreferredBondedName;
    gAutoConnectEnabled     = true;
    gLastAutoConnectAttemptMs = 0; // attempt on next loop iteration
    addKeyLog("Device already bonded");
    return true;
  }

  // Snapshot current bond list so we can identify which address was added by
  // this pairing attempt (important for peripherals that rotate/resolve addr).
  std::vector<String> bondsBefore;
  const int beforeCount = NimBLEDevice::getNumBonds();
  bondsBefore.reserve(beforeCount);
  for (int i = 0; i < beforeCount; i++) {
    bondsBefore.push_back(String(NimBLEDevice::getBondedAddress(i).toString().c_str()));
  }

  String pairReason;
  if (!canPairDeviceNow(address, pairReason)) {
    addKeyLog(String("Pair pre-check advisory: ") + pairReason);
  }
  if (pairReason != "ok") {
    addKeyLog(pairReason);
  }

  if (!openKeyboardLink(address, nameHint)) {
    return false;
  }

  addKeyLog("Starting pairing");
  if (!gClient->secureConnection()) {
    // SMP handshake failed — usually because the keyboard rejected the bond
    // (e.g. its pairing mode timed out between our scan and connect).
    addKeyLog("Pairing request failed");
    disconnectKeyboard();
    return false;
  }

  logConnectionSecurity("Post-pair security");
  bool bonded = isBondedAddress(address) || gClient->getConnInfo().isBonded();
  if (!bonded) {
    // The handshake completed without error but no LTK was stored — the
    // keyboard may have declined to bond.  Treat as failure.
    addKeyLog("Pairing finished without a stored bond");
    disconnectKeyboard();
    return false;
  }

  String preferredAddress = address;
  if (!isBondedAddress(preferredAddress)) {
    const int afterCount = NimBLEDevice::getNumBonds();
    for (int i = 0; i < afterCount; i++) {
      String candidate = String(NimBLEDevice::getBondedAddress(i).toString().c_str());
      bool existedBefore = false;
      for (const String& oldAddr : bondsBefore) {
        if (candidate.equalsIgnoreCase(oldAddr)) {
          existedBefore = true;
          break;
        }
      }
      if (!existedBefore) {
        preferredAddress = candidate;
        addKeyLog(String("Pair address canonicalized: ") + preferredAddress);
        break;
      }
    }
  }

  gPreferredBondedAddress = preferredAddress;
  gPreferredBondedName    = nameHint;
  pruneBondsExcept(gPreferredBondedAddress);
  gPendingReconnectAddress = gPreferredBondedAddress;
  gPendingReconnectName    = nameHint;
  addKeyLog("Bond stored; disconnecting until normal connect");
  disconnectKeyboard(); // auto-connect will handle the reconnection
  // Re-enable auto-connect and trigger reconnect soon, but not immediately.
  // Some remotes need a short settle period after bond store/disconnect.
  gAutoConnectEnabled        = true;
  gLastAutoConnectAttemptMs  = millis() - (AUTO_CONNECT_INTERVAL_MS - POST_PAIR_RECONNECT_DELAY_MS);
  addKeyLog(String("Auto-connect scheduled in ") + String(POST_PAIR_RECONNECT_DELAY_MS) + String(" ms"));
  return true;
}

// ---------------------------------------------------------------------------
// unpairKeyboard — remove a bond entirely
// ---------------------------------------------------------------------------
// Disconnects if currently connected to that address, then removes all bond
// data from NimBLE's NVS store.  Also clears the preferred device if it
// matched, and refreshes from the remaining bonds.
bool unpairKeyboard(const String& address) {
  if (gConnectedAddress.equalsIgnoreCase(address)) {
    disconnectKeyboard(); // drop the live link before deleting the bond
  }

  bool removed      = removeBondByAddress(address);
  bool stillBonded  = isBondedAddress(address); // verify deletion succeeded
  if (!removed || stillBonded) {
    addKeyLog(String("Unpair failed: ") + address);
    return false;
  }

  addKeyLog(String("Unpaired: ") + address);
  if (gPreferredBondedAddress.equalsIgnoreCase(address)) {
    gPreferredBondedAddress = "";
    gPreferredBondedName    = "";
  }
  refreshPreferredBondedDevice(); // adopt next available bond, if any
  return true;
}

// ---------------------------------------------------------------------------
// connectToKeyboard — reconnect to an already-bonded keyboard
// ---------------------------------------------------------------------------
// This is the normal-operation connect path (not pairing).  It:
//   1. Refuses to connect if the device is not bonded (safety guard).
//   2. Opens a raw GAP connection via openKeyboardLink().
//   3. Re-establishes encryption using the stored LTK via secureConnection().
//      Some keyboards will have already initiated encryption themselves;
//      the isEncrypted() check skips the redundant secureConnection() call
//      in that case.
//   4. Subscribes to HID input characteristics so key presses start flowing.
bool connectToKeyboard(const String& address, const String& nameHint) {
  if (!isBondedAddress(address)) {
    addKeyLog("Connect rejected: device is not bonded. Pair it first.");
    return false;
  }

  if (!openKeyboardLink(address, nameHint)) {
    return false;
  }

  // D07-class remotes have been observed to hang inside secureConnection()
  // during bonded reconnect.  For those devices we skip the explicit SMP
  // trigger and continue to HID subscription directly.
  bool skipExplicitSecureReconnect =
    nameHint.equalsIgnoreCase("D07") ||
    nameHint.indexOf("D07") >= 0;

  if (!gClient->getConnInfo().isEncrypted() && !skipExplicitSecureReconnect) {
    // The keyboard has not encrypted the link on its own — initiate from
    // our side using the stored LTK.
    addKeyLog("Restoring secure bonded connection");
    if (!gClient->secureConnection()) {
      addKeyLog("Secure reconnect failed");
      disconnectKeyboard();
      return false;
    }
  } else if (skipExplicitSecureReconnect && !gClient->getConnInfo().isEncrypted()) {
    addKeyLog("Skipping explicit secure reconnect for D07");
  }

  gPreferredBondedAddress = address;
  gPreferredBondedName    = nameHint;
  logConnectionSecurity("Ready to use");

  if (!subscribeToKeyboard()) {
    addKeyLog("Connected, but keyboard input subscribe failed");
    // Force a clean disconnect so gConnected goes false and auto-connect
    // can retry — without this the client stays connected but unusable
    // and auto-connect never triggers because it sees gConnected == true.
    disconnectKeyboard();
    return false;
  }
  return true;
}

// Public wrapper that delegates to the private disconnectKeyboard() helper.
void disconnectKeyboard() {
  ::disconnectKeyboard();
}

void setAutoConnectEnabled(bool enabled) {
  gAutoConnectEnabled = enabled;
  if (enabled) {
    gLastAutoConnectAttemptMs = 0; // trigger immediately on next loop
  }
}

// ---------------------------------------------------------------------------
// maybeAutoConnectBondedKeyboard — called from the main loop
// ---------------------------------------------------------------------------
// Periodically scans for the preferred bonded keyboard and connects to it
// automatically without any user interaction.  This is what makes RUN mode
// self-healing: if the keyboard is power-cycled or goes out of range, normal
// operation resumes a few seconds after it comes back.
//
// The AUTO_CONNECT_INTERVAL_MS cooldown prevents continuous scanning, which
// would saturate the radio, increase power consumption, and interfere with
// WiFi (both share the 2.4 GHz band).
void maybeAutoConnectBondedKeyboard() {
  if (!gAutoConnectEnabled) {
    return; // suspended — web UI scan in progress or explicitly disabled
  }
  if (gConnected) {
    return; // already connected — nothing to do
  }

  if (millis() - gLastAutoConnectAttemptMs < AUTO_CONNECT_INTERVAL_MS) {
    return; // still in cooldown period
  }

  // First priority: reconnect the device that was just paired.
  if (gPendingReconnectAddress.length() > 0) {
    gLastAutoConnectAttemptMs = millis();
    addKeyLog(String("Auto-connecting recently paired keyboard: ") + gPendingReconnectAddress);
    if (connectToKeyboard(gPendingReconnectAddress, gPendingReconnectName)) {
      gPendingReconnectAddress = "";
      gPendingReconnectName    = "";
      return;
    }

    addKeyLog("Auto-connect (recent pair) failed");
  }

  // Refresh in case a new bond was stored (or the old one deleted) since
  // the last attempt.
  refreshPreferredBondedDevice();
  if (gPreferredBondedAddress.length() == 0) {
    return; // no bond to connect to
  }

  gLastAutoConnectAttemptMs = millis();

  // Quick 2-second scan to see if the keyboard is currently advertising.
  // We only attempt connect if the device is visible; trying to connect
  // to a powered-off keyboard wastes time and disturbs the radio.
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setInterval(80);
  scan->setWindow(40);
  scan->setActiveScan(true);
  scan->clearResults();

  NimBLEScanResults results = scan->start(2, false); // 2 s — short but sufficient
  int preferredByAddrIdx = -1;

  for (int i = 0; i < results.getCount(); i++) {
    NimBLEAdvertisedDevice d = results.getDevice(i);
    String found = String(d.getAddress().toString().c_str());
    if (found.equalsIgnoreCase(gPreferredBondedAddress)) {
      preferredByAddrIdx = i;
      break;
    }
  }

  int chosenIdx = preferredByAddrIdx;
  if (chosenIdx >= 0) {
    NimBLEAdvertisedDevice d = results.getDevice(chosenIdx);
    String found = String(d.getAddress().toString().c_str());
    String name = d.haveName() ? String(d.getName().c_str()) : gPreferredBondedName;

    addKeyLog(String("Auto-connecting to bonded keyboard: ") + found);
    if (!connectToKeyboard(found, name)) {
      addKeyLog("Auto-connect failed");
    }
    return;
  }
}

// ---------------------------------------------------------------------------
// syncConnectionState — reconcile gConnected with the NimBLE client state
// ---------------------------------------------------------------------------
// NimBLE's client object can detect a drop slightly after the ClientCallbacks
// onDisconnect fires.  Calling this from the main loop catches any lingering
// mismatch so the rest of the application always reads a consistent state.
void syncConnectionState() {
  if (gClient && gConnected && !gClient->isConnected()) {
    gConnected = false;
  }
}

// Simple read-only state accessors used by the web UI status route.
bool          isConnected()     { return gConnected;        }
const String& connectedName()   { return gConnectedName;    }
const String& connectedAddress(){ return gConnectedAddress; }
uint8_t       lastKeyCode()     { return gLastKeyCode;       }

} // namespace BLEKeyboard
