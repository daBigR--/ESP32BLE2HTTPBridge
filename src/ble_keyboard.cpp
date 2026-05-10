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
#include <NimBLEUtils.h>

#include <vector>

#include "config_store.h"
#include "json_util.h"

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

// Last HCI disconnect reason code, set just before onDisconnect fires.
// 0 means no disconnect has occurred yet this session.
// Key values: 0x3D=MIC Failure (LTK mismatch), 0x05/0x06=Auth/Key Missing,
//             0x13=Remote User Terminated, 0x22=LMP Response Timeout.
uint8_t gLastDisconnectReason = 0;

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

// ---------------------------------------------------------------------------
// Reconnect policy constants
// ---------------------------------------------------------------------------
// RUN mode: exponential-ish backoff steps (ms).  Clamped at the last value.
static const uint32_t RUN_BACKOFF_MS[]      = {1000, 5000, 15000, 30000};
static const int      RUN_BACKOFF_COUNT     = (int)(sizeof(RUN_BACKOFF_MS) / sizeof(RUN_BACKOFF_MS[0]));
// CONFIG mode: at most 2 boot attempts, 5 s apart.  After that, wait for user.
static const uint32_t CONFIG_BOOT_RETRY_GAP_MS  = 5000;
static const int      CONFIG_BOOT_MAX_ATTEMPTS  = 2;

const unsigned long POST_PAIR_RECONNECT_DELAY_MS = 3000; // settle time for finicky remotes
const unsigned long RECONNECT_SECURITY_WAIT_MS = 1200; // bounded wait for async security upgrade

// ---------------------------------------------------------------------------
// Reconnect state
// ---------------------------------------------------------------------------
// true = RUN mode (backoff retries); false = CONFIG mode (boot cap then stop).
bool gRunMode              = false;
// Scheduled time for the next attempt (millis).  0 = try immediately.
unsigned long gNextRetryMs = 0;
// RUN mode backoff step index.  Clamped to RUN_BACKOFF_COUNT-1.
int  gRunBackoffIndex      = 0;
// CONFIG mode: how many boot attempts have fired so far.
int  gBootAttemptCount     = 0;
// true once a disconnect (not just boot) has been seen — drives post-disconnect
// backoff start in RUN mode.
bool gDisconnectSeen       = false;
// Reconnect scheduling is active when true.
bool gRetryArmed           = false;

// When false, maybeAutoConnectBondedKeyboard() is a no-op.  Disabled by the
// web UI /scan handler to prevent scanner contention; re-enabled automatically
// after a successful pair or by an explicit setAutoConnectEnabled(true) call.
bool gAutoConnectEnabled = true;

// Recently paired target gets first auto-connect priority so we reconnect the
// device the user just paired even if preferred-bond canonicalization is
// ambiguous for that cycle.
String gPendingReconnectAddress = "";
String gPendingReconnectName    = "";

// Last burst signature received via HID notification (exposed through
// lastSignature() for the web UI status endpoint).
String gLastSignature = "";

// Recent burst event ring (last 10 events, for the web UI feed).
// Written from the NimBLE notification task, read from the HTTP server task.
// No mutex: best-effort UI data only.
struct SigEntry { String sig; String dev; uint32_t ms; };
static const size_t MAX_RECENT_SIGS = 10;
std::vector<SigEntry> gRecentSigs;

// Burst-detection state for the notify handler.
// A "burst" is the group of notifications a device emits for one physical
// button press; within-burst gaps are short (observed: 1–52 ms across all
// tested devices), between-burst gaps are >> BURST_GAP_MS.
const uint32_t BURST_GAP_MS = 100; // ms; tunable if a device needs adjustment
uint32_t gLastNotificationMs = 0;  // millis() timestamp of the last notification

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
bool openKeyboardLink(const String& address, const String& nameHint, bool useFreshScan = true, NimBLEAdvertisedDevice* scanTarget = nullptr);
bool canPairDeviceNow(const String& address, String& reason);
bool removeBondByAddress(const String& address);
void pruneBondsExcept(const String& keepAddress);
bool trySecurityUpgradeWithTimeout();

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

// Build a lowercase hex string from a byte array (no separators).
// e.g. {0x03, 0x7f, 0x7f} -> "037f7f"
static String bytesToHexString(const uint8_t* data, size_t len) {
  String s;
  s.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02x", data[i]);
    s += buf;
  }
  return s;
}

static void notifyCallback(NimBLERemoteCharacteristic* pRemoteCharacteristic,
                           uint8_t* pData,
                           size_t length,
                           bool isNotify) {
  (void)pRemoteCharacteristic; // not used; notification is self-describing
  if (!isNotify || length < 3) {
    return; // ignore indication ACKs and malformed reports
  }

  uint32_t now = (uint32_t)millis();
  uint32_t gap = now - gLastNotificationMs;
  bool isNewBurst = (gLastNotificationMs == 0) || (gap > BURST_GAP_MS);
  gLastNotificationMs = now;

  String signature = bytesToHexString(pData, length);
  if (isNewBurst) {
    addKeyLog(String("[BURST] new burst gap=") + String(gap) + "ms payload=" + signature);
    gLastSignature = signature;
    // Add to recent sigs ring.
    if (gRecentSigs.size() >= MAX_RECENT_SIGS) {
      gRecentSigs.erase(gRecentSigs.begin());
    }
    gRecentSigs.push_back({signature, gConnectedName, now});
    // Dispatch to URL layer.
    if (gKeyPressFn) {
      gKeyPressFn(signature);
    }
    // Keep KEY 0x... log line for debugging visibility (not used for URL dispatch).
    for (int i = 2; i < (int)length && i < 8; i++) {
      if (pData[i] == 0) continue;
      String line = "KEY 0x";
      if (pData[i] < 0x10) line += "0";
      line += String(pData[i], HEX);
      addKeyLog(line);
      break;
    }
  } else {
    addKeyLog(String("[BURST] mid-burst gap=") + String(gap) + "ms payload=" + signature);
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
    gLastNotificationMs = 0; // reset burst state on each new connection
    gLastSignature = "";     // reset last sig so UI doesn't show stale data
    // Reset all reconnect state: connection succeeded.
    gRetryArmed       = false;
    gRunBackoffIndex  = 0;
    gBootAttemptCount = 0;
    gDisconnectSeen   = false;
    addKeyLog("Connected");
  }

  // Called when the link is dropped for any reason (keyboard powered off,
  // moved out of range, BLE stack timeout, etc.).
  void onDisconnect(NimBLEClient* pClient, int reason) override {
    (void)pClient;
    gConnected = false;
    gLastNotificationMs = 0; // reset burst state on disconnect
    gLastSignature = "";     // reset last sig on disconnect
    gLastDisconnectReason = (uint8_t)(reason & 0xFF);
    addKeyLog(String("Disconnected reason=0x") + String(gLastDisconnectReason, HEX));
    // Arm reconnect scheduler.
    gDisconnectSeen = true;
    if (gRunMode) {
      // RUN: start backoff from step 0.
      gRunBackoffIndex = 0;
      gNextRetryMs     = millis() + RUN_BACKOFF_MS[0];
      gRetryArmed      = true;
      addKeyLog(String("[RECON] mode=RUN trigger=disconnect retry_index=0 (after ") + String(RUN_BACKOFF_MS[0]) + "ms)");
    } else {
      // CONFIG: do not auto-retry after a disconnect.
      gRetryArmed = false;
      addKeyLog("[RECON] mode=CONFIG disconnect, no auto-retry");
    }
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
// subscribeServiceChanged — enable Service Changed indications before HID setup
// ---------------------------------------------------------------------------
// Some HOGP-compliant peripherals send a Service Changed indication at the
// start of each encrypted session to signal that the GATT table may have
// been modified since the last bond.  If the client has not registered for
// these indications (CCCD 0x0002 on characteristic 0x2A05 of service 0x1801)
// the peripheral may stall waiting for the client to acknowledge the
// indication before it will respond to subsequent ATT requests.
//
// Called after encryption is active and before any HID CCCD writes.
// Fails gracefully (logs and returns) if 0x1801 or 0x2A05 is not present.
void subscribeServiceChanged() {
  if (!gClient || !gClient->isConnected()) {
    return;
  }

  NimBLERemoteService* gattSvc = gClient->getService("1801");
  if (!gattSvc) {
    addKeyLog("SvcChg: service 0x1801 not present, skipping");
    return;
  }

  NimBLERemoteCharacteristic* scChr = gattSvc->getCharacteristic("2A05");
  if (!scChr) {
    addKeyLog("SvcChg: characteristic 0x2A05 not present, skipping");
    return;
  }

  if (!scChr->canIndicate()) {
    addKeyLog("SvcChg: 0x2A05 does not support indicate, skipping");
    return;
  }

  // Write 0x0002 (indicate-enable) to the CCCD; subscribe(false,...) = indicate.
  // nullptr callback: we don't need to process the indication content,
  // we only need the peripheral to see that we are registered.
  bool ok = scChr->subscribe(false, nullptr);
  addKeyLog(ok ? "SvcChg: subscribed to 0x2A05 indicate" : "SvcChg: subscribe failed (non-fatal)");
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
// trySecurityUpgradeWithTimeout — bounded reconnect security restoration
// ---------------------------------------------------------------------------
// On reconnect we prefer encrypted links for devices that support bond-based
// security restoration, but we must avoid indefinite blocking calls that can
// hang with some peripherals.  This helper starts security asynchronously and
// waits for a short bounded window for encryption to become active.
bool trySecurityUpgradeWithTimeout() {
  if (!gClient || !gClient->isConnected()) {
    return false;
  }

  if (gClient->getConnInfo().isEncrypted()) {
    return true;
  }

  int rc = NimBLEDevice::startSecurity(gClient->getConnId());
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    addKeyLog(String("Security upgrade start failed rc=") + String(rc));
    return false;
  }
  if (rc == BLE_HS_EALREADY) {
    // Peripheral has already initiated the handshake — it owns the exchange.
    // Proceeding to GATT immediately is correct; encryption completes
    // asynchronously during the 500 ms delay inside discoverAttributes().
    // Waiting in a poll loop here causes the Kobo to disconnect before the
    // loop times out (observed: "DisconSecurity upgrade timeout" in log).
    addKeyLog("Security already in progress (peripheral-initiated)");
    return true;
  }

  unsigned long t0 = millis();
  while (gClient && gClient->isConnected() && (millis() - t0 < RECONNECT_SECURITY_WAIT_MS)) {
    if (gClient->getConnInfo().isEncrypted()) {
      return true;
    }
    delay(20);
  }

  addKeyLog("Security upgrade timeout");
  return gClient && gClient->isConnected() && gClient->getConnInfo().isEncrypted();
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

    reason = "ok";
    return true;
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
bool openKeyboardLink(const String& address, const String& nameHint, bool useFreshScan, NimBLEAdvertisedDevice* scanTarget) {
  disconnectKeyboard(); // ensure clean state before attempting a new connection

  // If the previous client was abandoned (disconnect timeout above), bail
  // immediately rather than layering a new connection attempt on top of a
  // potentially still-active BLE link.
  if (gClient != nullptr) {
    addKeyLog("Connect aborted: previous client still active");
    if (scanTarget) {
      delete scanTarget;
    }
    return false;
  }

  gClient = NimBLEDevice::createClient();
  gClient->setClientCallbacks(new ClientCallbacks(), true); // true = auto-delete
  gClient->setConnectTimeout(10); // seconds; long enough for slow keyboards
  gClient->setConnectionParams(24, 40, 0, 42); // 30-50 ms interval matches Kobo/HOGP peripherals; 420 ms supervision timeout
#if CONFIG_BT_NIMBLE_EXT_ADV
  // With CONFIG_BT_NIMBLE_EXT_ADV, connect() uses ble_gap_ext_connect() with
  // the default phyMask = 1M|2M|CODED.  Sending a 3-PHY extended connection
  // request to a legacy BLE 4.x device (e.g. Kobo Remote) causes rc=574
  // because the peripheral only understands 1M legacy connection initiation.
  // Force 1M-only so the controller uses a backward-compatible connection request.
  gClient->setConnectPhy(BLE_GAP_LE_PHY_1M_MASK);
#endif

  addKeyLog(String("Connecting to ") + address);
  NimBLEAdvertisedDevice* target = scanTarget;
  NimBLEAddress explicitTargetAddress;
  int explicitTargetType = -1;
  if (scanTarget) {
    explicitTargetAddress = scanTarget->getAddress();
    explicitTargetType    = scanTarget->getAddressType();
    addKeyLog(String("Using existing advertised scan record; addr=") + String(explicitTargetAddress.toString().c_str()) +
              String(" addr type=") + String(explicitTargetType) +
              String(" flags=0x") + String(scanTarget->getAdvFlags(), HEX) +
              String(" rssi=") + String(scanTarget->getRSSI()));
  } else if (useFreshScan) {
    class TargetScanCB : public NimBLEAdvertisedDeviceCallbacks {
      public:
        TargetScanCB(const String& addr, NimBLEAdvertisedDevice** targetPtr)
          : address(addr), ppTarget(targetPtr) {}

        void onResult(NimBLEAdvertisedDevice* d) override {
          String found = String(d->getAddress().toString().c_str());
          if (!found.equalsIgnoreCase(address)) {
            return;
          }
          // Stop on first match regardless of advType/isConnectable().
          // isConnectable() is unreliable under CONFIG_BT_NIMBLE_EXT_ADV for
          // legacy BLE 4.x advertisers (advType=0 can report as non-connectable).
          if (!*ppTarget) {
            *ppTarget = new NimBLEAdvertisedDevice(*d);
          }
          NimBLEDevice::getScan()->stop();
        }

      private:
        const String address;
        NimBLEAdvertisedDevice** ppTarget;
    } callback(address, &target);

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&callback, false);
    scan->setInterval(80);
    scan->setWindow(80);    // full duty-cycle: window == interval for fastest detection
    scan->setActiveScan(false); // passive scan — ADV_IND fires callback immediately
                                // without waiting for SCAN_RSP; SCAN_RSP overwrites
                                // advType to 4 (non-connectable) which breaks connect()
    scan->setDuplicateFilter(false); // allow multiple adverts from the same device
    scan->clearResults();

    scan->start(4, false); // 4 s scan, non-blocking
    while (scan->isScanning()) {
      delay(10);
    }

    NimBLEScanResults results = scan->getResults();
    int scanCount = results.getCount();
    addKeyLog(String("Scan found ") + String(scanCount) + String(" devices"));
    if (target) {
      addKeyLog(String("Target found in scan; addr type=") + String(target->getAddressType()) +
                String(" flags=0x") + String(target->getAdvFlags(), HEX) +
                String(" rssi=") + String(target->getRSSI()) +
                String(" advType=0x") + String(target->getAdvType(), HEX));
    }
    scan->setAdvertisedDeviceCallbacks(nullptr);
  } else {
    addKeyLog("Skipping fresh scan for bonded connect");
  }

  bool ok = false;
  if (target) {
    if (explicitTargetType >= 0) {
      addKeyLog(String("Target scan record address=") + String(explicitTargetAddress.toString().c_str()) +
                String(" type=") + String(explicitTargetType));
    } else {
      addKeyLog(String("Target scan record address=") + String(target->getAddress().toString().c_str()));
    }
    // Preferred: connect via the full advertising device record so NimBLE
    // can use the address type and resolve RPAs correctly.
    addKeyLog("Attempting connect via advertised device record");
    delay(100); // give peripheral time to stabilize after scan
    ok = gClient->connect(target, false);
    if (!ok && explicitTargetType >= 0) {
      addKeyLog(String("Connect via scan record failed rc=") + String(gClient->getLastError()) +
                String(" (") + String(NimBLEUtils::returnCodeToString(gClient->getLastError())) + String(")"));
      addKeyLog(String("Retrying with explicit scan-record address type=") + String(explicitTargetType));
      ok = gClient->connect(NimBLEAddress(explicitTargetAddress.toString().c_str(), explicitTargetType), false);
    }
    delete target;
    if (!ok) {
      addKeyLog(String("Connect via scan record failed rc=") + String(gClient->getLastError()) +
                String(" (") + String(NimBLEUtils::returnCodeToString(gClient->getLastError())) + String(")"));
    } else {
      addKeyLog("Connected via advertised device record");
    }
  }

  if (!ok) {
    // Fallback: the device was not seen in the scan (may be in directed
    // advertising or just powering up), or connect(target) failed.
    // Try NimBLE's generic address constructor as a last attempt.
    if (!target) {
      addKeyLog("Device not found in fresh scan, trying direct address fallback");
    }
    
    addKeyLog("Trying direct address generic");
    ok = gClient->connect(NimBLEAddress(address.c_str()), false);
    if (!ok) {
      addKeyLog("connect failed: scan miss + direct address timeout");
    }
  }

  if (!ok) {
    addKeyLog(String("Connect failed, final rc=") + String(gClient->getLastError()));
    NimBLEDevice::deleteClient(gClient);
    gClient = nullptr;
    return false;
  }

  addKeyLog("Connect succeeded");

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
    // Existing preference is still valid; restore name from NVS if not in RAM.
    if (gPreferredBondedName.length() == 0) {
      gPreferredBondedName = ConfigStore::loadBondedName();
    }
    return;
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
  gPreferredBondedName    = ConfigStore::loadBondedName();
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
  ConfigStore::saveBondedName("");
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
    ConfigStore::saveBondedName(nameHint);
    pruneBondsExcept(gPreferredBondedAddress);
    gPendingReconnectAddress = gPreferredBondedAddress;
    gPendingReconnectName    = gPreferredBondedName;
    gAutoConnectEnabled     = true;
    gNextRetryMs = 0; // attempt on next loop iteration
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

  // openKeyboardLink uses its own callback scan that stops immediately on
  // finding the target and connects right away — do not pass the advisory
  // scan result here, that record is stale by the time we'd use it.
  if (!openKeyboardLink(address, nameHint, true)) {
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
  ConfigStore::saveBondedName(nameHint);
  pruneBondsExcept(gPreferredBondedAddress);
  gPendingReconnectAddress = gPreferredBondedAddress;
  gPendingReconnectName    = nameHint;

  // DIAG: skip post-pair disconnect — stay on the live encrypted link and
  // attempt HID subscription now.  This tests whether the first encrypted
  // session works without the round-trip reconnect that fails on Kobo.
  addKeyLog("DIAG: staying connected after pair; attempting HID subscribe");
  gAutoConnectEnabled = true;
  if (subscribeToKeyboard()) {
    gConnectedName    = nameHint;
    gConnectedAddress = preferredAddress;
    logConnectionSecurity("Post-pair HID ready");
    gPendingReconnectAddress = ""; // already connected, no pending reconnect needed
    gPendingReconnectName    = "";
  } else {
    addKeyLog("DIAG: HID subscribe failed on live link; falling back to disconnect+reconnect");
    disconnectKeyboard();
    // Schedule reconnect after POST_PAIR_RECONNECT_DELAY_MS settle time.
    gNextRetryMs = millis() + POST_PAIR_RECONNECT_DELAY_MS;
    gRetryArmed  = true;
    addKeyLog(String("Auto-connect scheduled in ") + String(POST_PAIR_RECONNECT_DELAY_MS) + String(" ms"));
  }
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
    ConfigStore::saveBondedName("");
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
//   3. Restores link encryption before any GATT work — HOGP-compliant
//      peripherals (e.g. Kobo) require encryption before they will respond
//      to attribute discovery or CCCD writes.
//   4. Subscribes to HID input characteristics.
bool connectToKeyboard(const String& address, const String& nameHint, NimBLEAdvertisedDevice* scanTarget) {
  if (!isBondedAddress(address)) {
    addKeyLog("Connect rejected: device is not bonded. Pair it first.");
    if (scanTarget) {
      delete scanTarget;
    }
    return false;
  }

  // Scan when no pre-discovered record is provided (e.g. post-pair auto-connect).
  // Devices with public addresses (Kobo) require a scan-record connect; direct
  // address attempts fail with rc=574 without advertising context.
  bool doFreshScan = (scanTarget == nullptr);
  if (!openKeyboardLink(address, nameHint, doFreshScan, scanTarget)) {
    return false;
  }

  // Restore encryption before any GATT work.  Devices that enforce
  // BLE_ATT_ERR_INSUFFICIENT_AUTHENTICATION on discovery will fail if we
  // send ATT requests on an unencrypted link.
  if (!gClient->getConnInfo().isEncrypted()) {
    addKeyLog("Upgrading link security before GATT work");
    if (!trySecurityUpgradeWithTimeout()) {
      addKeyLog("Security upgrade failed; aborting connect");
      disconnectKeyboard();
      return false;
    }
  }

  // Subscribe to Service Changed indications (service 0x1801 / char 0x2A05)
  // before writing any HID CCCDs.  Some HOGP peripherals stall if they send
  // a Service Changed indication and the client has not registered for it.
  subscribeServiceChanged();

  bool subscribed = subscribeToKeyboard();
  if (!subscribed) {
    addKeyLog("Connected, but keyboard input subscribe failed");
    disconnectKeyboard();
    return false;
  }

  gPreferredBondedAddress = address;
  gPreferredBondedName    = nameHint;
  ConfigStore::saveBondedName(nameHint);
  logConnectionSecurity("Ready to use");
  return true;
}

// Public wrapper that delegates to the private disconnectKeyboard() helper.
void disconnectKeyboard() {
  ::disconnectKeyboard();
}

void setAutoConnectEnabled(bool enabled) {
  gAutoConnectEnabled = enabled;
  if (enabled) {
    gNextRetryMs = 0; // trigger immediately on next loop
  }
}

// ---------------------------------------------------------------------------
// setReconnectMode — switch between CONFIG and RUN reconnect policy
// ---------------------------------------------------------------------------
// Call once from main.cpp after the mode is determined (setup), and again
// if the mode ever changes at runtime.
void setReconnectMode(bool runMode) {
  if (runMode == gRunMode) return; // no change
  if (runMode) {
    // Switching CONFIG → RUN: start backoff from step 0 if not connected.
    gRunMode = true;
    if (!gConnected) {
      gRunBackoffIndex = 0;
      gNextRetryMs     = millis() + RUN_BACKOFF_MS[0];
      gRetryArmed      = true;
      addKeyLog("[RECON] mode switch CONFIG→RUN, starting backoff");
    }
  } else {
    // Switching RUN → CONFIG: cancel any pending retry.
    gRunMode    = false;
    gRetryArmed = false;
    addKeyLog("[RECON] mode switch RUN→CONFIG, cancelling pending retry");
  }
}

// ---------------------------------------------------------------------------
// resetReconnectState — used by /connect (user action) to force an immediate
// attempt regardless of mode or pending backoff state.
// ---------------------------------------------------------------------------
void resetReconnectState() {
  gRetryArmed      = false;
  gRunBackoffIndex = 0;
  gBootAttemptCount = 0;
  gNextRetryMs     = 0;
}

// ---------------------------------------------------------------------------
// doConnect — perform one connect attempt for the preferred bonded device.
// ---------------------------------------------------------------------------
// Scans for 2 s to check if the device is advertising; if found, connects
// via the advertised target.  If not found, falls through to a direct-address
// connection attempt (some peripherals stop advertising after bonding).
// Returns true on success, false on failure.
// Not static so tryConnectNow() can call it directly.
bool doConnect() {
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

  if (preferredByAddrIdx >= 0) {
    NimBLEAdvertisedDevice d = results.getDevice(preferredByAddrIdx);
    NimBLEAdvertisedDevice* target = new NimBLEAdvertisedDevice(d);
    String found = String(d.getAddress().toString().c_str());
    String name  = d.haveName() ? String(d.getName().c_str()) : gPreferredBondedName;
    addKeyLog(String("Auto-connecting to bonded keyboard: ") + found);
    bool ok = connectToKeyboard(found, name, target);
    if (!ok) addKeyLog("Auto-connect failed");
    return ok;
  }

  // Device not found in scan — fall through to direct-address connect.
  // Some bonded peripherals (e.g. Kobo) stop general advertising after
  // bonding but still accept a direct-address connection.
  addKeyLog("Preferred device not in scan, attempting direct connect");
  bool ok = connectToKeyboard(gPreferredBondedAddress, gPreferredBondedName, nullptr);
  if (!ok) addKeyLog("Auto-connect (direct) failed");
  return ok;
}

// ---------------------------------------------------------------------------
// maybeAutoConnectBondedKeyboard — called from the main loop
// ---------------------------------------------------------------------------
// Implements mode-aware reconnect policy:
//   CONFIG mode: up to CONFIG_BOOT_MAX_ATTEMPTS at boot (5 s apart), then stop.
//   RUN mode   : exponential backoff (1/5/15/30/60 s, then 60 s repeating).
// The post-pair pending-reconnect path is still first-priority in both modes.
void maybeAutoConnectBondedKeyboard() {
  if (!gAutoConnectEnabled) {
    return; // suppressed by /scan handler
  }
  if (gConnected) {
    return; // already connected
  }

  unsigned long now = millis();

  // ---- Post-pair priority reconnect (mode-agnostic) ----------------------
  // A freshly paired device gets one immediate attempt regardless of policy.
  if (gPendingReconnectAddress.length() > 0) {
    addKeyLog(String("[RECON] post-pair connect: ") + gPendingReconnectAddress);
    if (connectToKeyboard(gPendingReconnectAddress, gPendingReconnectName)) {
      gPendingReconnectAddress = "";
      gPendingReconnectName    = "";
    } else {
      addKeyLog("[RECON] post-pair connect failed");
      gPendingReconnectAddress = "";
      gPendingReconnectName    = "";
    }
    return;
  }

  // ---- No bond = nothing to connect to -----------------------------------
  refreshPreferredBondedDevice();
  if (gPreferredBondedAddress.length() == 0) {
    return;
  }

  // ---- CONFIG mode -------------------------------------------------------
  if (!gRunMode) {
    // Only retry at boot (gDisconnectSeen == false) and only up to the cap.
    if (gDisconnectSeen) {
      return; // post-disconnect: no auto-retry in CONFIG mode
    }
    if (gBootAttemptCount >= CONFIG_BOOT_MAX_ATTEMPTS) {
      return; // exhausted boot attempts
    }
    // First attempt: gRetryArmed may not be set yet — allow it.
    if (gBootAttemptCount == 0 && !gRetryArmed) {
      gRetryArmed  = true;
      gNextRetryMs = 0; // immediate
    }
    if (!gRetryArmed || now < gNextRetryMs) {
      return;
    }
    gBootAttemptCount++;
    gRetryArmed = false;
    addKeyLog(String("[RECON] mode=CONFIG trigger=boot attempt=") + String(gBootAttemptCount));
    bool ok = doConnect();
    if (!ok && gBootAttemptCount < CONFIG_BOOT_MAX_ATTEMPTS) {
      // Schedule the next boot attempt.
      gNextRetryMs = millis() + CONFIG_BOOT_RETRY_GAP_MS;
      gRetryArmed  = true;
      addKeyLog(String("[RECON] mode=CONFIG trigger=boot attempt=") +
                String(gBootAttemptCount + 1) + " (after " + String(CONFIG_BOOT_RETRY_GAP_MS) + "ms)");
    } else if (!ok) {
      addKeyLog("[RECON] giving up (CONFIG mode boot exhausted)");
    }
    return;
  }

  // ---- RUN mode ----------------------------------------------------------
  if (!gRetryArmed) {
    // Not yet armed: arm for step 0 (happens if module starts in RUN mode
    // and was never disconnected yet, i.e. very first boot attempt).
    gRunBackoffIndex = 0;
    gNextRetryMs     = now + RUN_BACKOFF_MS[0];
    gRetryArmed      = true;
    return;
  }
  if (now < gNextRetryMs) {
    return; // not time yet
  }
  int step = gRunBackoffIndex < RUN_BACKOFF_COUNT ? gRunBackoffIndex : RUN_BACKOFF_COUNT - 1;
  addKeyLog(String("[RECON] mode=RUN trigger=disconnect retry_index=") + String(step) +
            " (after " + String(RUN_BACKOFF_MS[step]) + "ms)");
  bool ok = doConnect();
  if (!ok) {
    // Advance to next backoff step (clamp at last).
    if (gRunBackoffIndex < RUN_BACKOFF_COUNT - 1) {
      gRunBackoffIndex++;
    }
    int next = gRunBackoffIndex < RUN_BACKOFF_COUNT ? gRunBackoffIndex : RUN_BACKOFF_COUNT - 1;
    gNextRetryMs = millis() + RUN_BACKOFF_MS[next];
    gRetryArmed  = true;
  }
  // On success, onConnect() resets gRetryArmed and counters.
}

// ---------------------------------------------------------------------------
// tryConnectNow — user-initiated immediate connect attempt (RUN mode button)
// ---------------------------------------------------------------------------
// Calls doConnect() immediately, bypassing the backoff schedule.
// On success: onConnect() resets backoff state normally.
// On failure: backoff state is untouched — the existing retry schedule resumes.
bool tryConnectNow() {
  if (gPreferredBondedAddress.length() == 0) return false;
  addKeyLog("[BTN] manual connect attempt");
  bool ok = doConnect();
  if (!ok) addKeyLog("[BTN] manual connect failed, resuming backoff schedule");
  return ok;
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
const String& lastSignature()   { return gLastSignature;    }

// Returns a JSON array of the last 20 burst events, oldest-to-newest.
// Format: [{"sig":"...","dev":"...","ms":12345}, ...]
String recentSigsJson() {
  String out = "[";
  for (size_t i = 0; i < gRecentSigs.size(); i++) {
    if (i > 0) out += ",";
    out += "{\"sig\":\""  + JsonUtil::escape(gRecentSigs[i].sig) +
           "\",\"dev\":\"" + JsonUtil::escape(gRecentSigs[i].dev) +
           "\",\"ms\":"   + String(gRecentSigs[i].ms) + "}";
  }
  out += "]";
  return out;
}

} // namespace BLEKeyboard
