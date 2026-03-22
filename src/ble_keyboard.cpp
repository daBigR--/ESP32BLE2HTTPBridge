#include "ble_keyboard.h"

#include <vector>

#define HID_SERVICE_UUID      "1812"
#define HID_INPUT_REPORT_UUID "2A4D"

namespace {

NimBLEClient* gClient = nullptr;
bool gConnected = false;
String gConnectedName = "";
String gConnectedAddress = "";
size_t gSubscribedCharacteristicCount = 0;
String gPreferredBondedAddress = "";
String gPreferredBondedName = "";
unsigned long gLastAutoConnectAttemptMs = 0;
const unsigned long AUTO_CONNECT_INTERVAL_MS = 8000;
uint8_t gLastKeyCode = 0;

BLEKeyboard::LogFn gLogFn = nullptr;
BLEKeyboard::KeyPressFn gKeyPressFn = nullptr;

void addKeyLog(const String& line) {
  if (gLogFn) {
    gLogFn(line);
  }
}

class SecurityCallbacks : public NimBLESecurityCallbacks {
  uint32_t onPassKeyRequest() override {
    addKeyLog("Passkey requested; returning 000000");
    return 0;
  }

  void onPassKeyNotify(uint32_t pass_key) override {
    addKeyLog(String("Passkey notify: ") + String(pass_key));
  }

  bool onConfirmPIN(uint32_t pass_key) override {
    addKeyLog(String("Confirm PIN: ") + String(pass_key));
    return true;
  }

  bool onSecurityRequest() override {
    addKeyLog("Security request accepted");
    return true;
  }

  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    if (!desc) {
      addKeyLog("Authentication complete: no descriptor");
      return;
    }
    if (desc->sec_state.encrypted) {
      String line = "Auth complete: encrypted=yes";
      line += desc->sec_state.authenticated ? " authenticated=yes" : " authenticated=no";
      line += desc->sec_state.bonded ? " bonded=yes" : " bonded=no";
      addKeyLog(line);
    } else {
      addKeyLog("Pairing failed (not encrypted)");
    }
  }
};

void disconnectKeyboard();
void logConnectionSecurity(const String& prefix);
bool openKeyboardLink(const String& address, const String& nameHint);
bool canPairDeviceNow(const String& address, String& reason);
bool removeBondByAddress(const String& address);

static void notifyCallback(NimBLERemoteCharacteristic* pRemoteCharacteristic,
                           uint8_t* pData,
                           size_t length,
                           bool isNotify) {
  (void)pRemoteCharacteristic;
  if (!isNotify || length < 3) {
    return;
  }

  for (int i = 2; i < (int)length && i < 8; i++) {
    if (pData[i] == 0) {
      continue;
    }
    gLastKeyCode = pData[i];
    if (gKeyPressFn) {
      gKeyPressFn(pData[i]);
    }
    String line = "KEY 0x";
    if (pData[i] < 0x10) {
      line += "0";
    }
    line += String(pData[i], HEX);
    addKeyLog(line);
    break;
  }
}

class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pClient) override {
    (void)pClient;
    gConnected = true;
    addKeyLog("Connected");
  }

  void onDisconnect(NimBLEClient* pClient) override {
    (void)pClient;
    gConnected = false;
    addKeyLog("Disconnected");
  }

  bool onConnParamsUpdateRequest(NimBLEClient* pClient, const ble_gap_upd_params* params) override {
    (void)pClient;
    (void)params;
    return true;
  }

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

bool subscribeToKeyboard() {
  if (!gClient || !gClient->isConnected()) {
    return false;
  }

  gSubscribedCharacteristicCount = 0;

  addKeyLog("Discovering services...");
  delay(500);

  if (!gClient->discoverAttributes()) {
    addKeyLog("Failed to discover attributes");
    return false;
  }

  delay(500);

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
    bool isKeyboardInput =
      charUUID.equalsIgnoreCase("0x2a22") ||
      charUUID.equalsIgnoreCase("2a22") ||
      charUUID.equalsIgnoreCase("0x2a4d") ||
      charUUID.equalsIgnoreCase("2a4d");
    bool canSignal = ch->canNotify() || ch->canIndicate();

    if (!isKeyboardInput || !canSignal) {
      continue;
    }

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

void logConnectionSecurity(const String& prefix) {
  if (!gClient || !gClient->isConnected()) {
    addKeyLog(prefix + ": no active connection");
    return;
  }

  NimBLEConnInfo info = gClient->getConnInfo();
  String line = prefix;
  line += " encrypted=";
  line += info.isEncrypted() ? "yes" : "no";
  line += " authenticated=";
  line += info.isAuthenticated() ? "yes" : "no";
  line += " bonded=";
  line += info.isBonded() ? "yes" : "no";
  addKeyLog(line);
}

bool removeBondByAddress(const String& address) {
  bool removed = false;

  if (NimBLEDevice::deleteBond(NimBLEAddress(address.c_str(), BLE_ADDR_PUBLIC))) {
    removed = true;
  }
  if (NimBLEDevice::deleteBond(NimBLEAddress(address.c_str(), BLE_ADDR_RANDOM))) {
    removed = true;
  }

  for (int i = NimBLEDevice::getNumBonds() - 1; i >= 0; i--) {
    NimBLEAddress bonded = NimBLEDevice::getBondedAddress(i);
    String bondedAddr = String(bonded.toString().c_str());
    if (!bondedAddr.equalsIgnoreCase(address)) {
      continue;
    }

    if (NimBLEDevice::deleteBond(bonded)) {
      removed = true;
    }
  }

  return removed;
}

bool canPairDeviceNow(const String& address, String& reason) {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setInterval(80);
  scan->setWindow(40);
  scan->setActiveScan(true);
  scan->clearResults();

  NimBLEScanResults results = scan->start(4, false);
  for (int i = 0; i < results.getCount(); i++) {
    NimBLEAdvertisedDevice d = results.getDevice(i);
    String found = String(d.getAddress().toString().c_str());
    if (!found.equalsIgnoreCase(address)) {
      continue;
    }

    uint8_t flags = d.getAdvFlags();
    uint8_t advType = d.getAdvType();
    bool directed = d.haveTargetAddress() || advType == 1 || advType == 4;
    if (BLEKeyboard::isAdvertisedAsPairingMode(d)) {
      reason = "ok";
      return true;
    }

    reason = String("device not advertising for new pairing (advType=") + String(advType) + String(", flags=0x") + String(flags, HEX) + String(", directed=") + String(directed ? 1 : 0) + String(")");
    return false;
  }

  reason = "device not currently advertising";
  return false;
}

void disconnectKeyboard() {
  if (gClient) {
    if (gClient->isConnected()) {
      gClient->disconnect();
    }
    NimBLEDevice::deleteClient(gClient);
    gClient = nullptr;
  }
  gConnected = false;
  gConnectedName = "";
  gConnectedAddress = "";
}

bool openKeyboardLink(const String& address, const String& nameHint) {
  disconnectKeyboard();

  gClient = NimBLEDevice::createClient();
  gClient->setClientCallbacks(new ClientCallbacks(), true);
  gClient->setConnectTimeout(10);

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setInterval(80);
  scan->setWindow(40);
  scan->setActiveScan(true);
  scan->clearResults();

  addKeyLog(String("Connecting to ") + address);
  NimBLEAdvertisedDevice* target = nullptr;
  NimBLEScanResults results = scan->start(4, false);
  for (int i = 0; i < results.getCount(); i++) {
    NimBLEAdvertisedDevice d = results.getDevice(i);
    String found = String(d.getAddress().toString().c_str());
    if (found.equalsIgnoreCase(address)) {
      target = new NimBLEAdvertisedDevice(d);
      break;
    }
  }

  bool ok = false;
  if (target) {
    ok = gClient->connect(target, false);
    delete target;
  } else {
    addKeyLog("Device not found in fresh scan, trying direct address");
    ok = gClient->connect(NimBLEAddress(address.c_str()), false);
  }

  if (!ok) {
    addKeyLog("Connect failed");
    NimBLEDevice::deleteClient(gClient);
    gClient = nullptr;
    return false;
  }

  gConnectedName = nameHint;
  gConnectedAddress = address;
  logConnectionSecurity("Link security");
  return true;
}

} // namespace

namespace BLEKeyboard {

void begin(LogFn logFn, KeyPressFn keyPressFn) {
  gLogFn = logFn;
  gKeyPressFn = keyPressFn;
  NimBLEDevice::setSecurityCallbacks(new SecurityCallbacks());
}

bool isBondedAddress(const String& address) {
  if (NimBLEDevice::isBonded(NimBLEAddress(address.c_str(), BLE_ADDR_PUBLIC))) {
    return true;
  }
  if (NimBLEDevice::isBonded(NimBLEAddress(address.c_str(), BLE_ADDR_RANDOM))) {
    return true;
  }

  const int bondCount = NimBLEDevice::getNumBonds();
  for (int i = 0; i < bondCount; i++) {
    String bondedAddr = String(NimBLEDevice::getBondedAddress(i).toString().c_str());
    if (bondedAddr.equalsIgnoreCase(address)) {
      return true;
    }
  }
  return false;
}

bool isAdvertisedAsPairingMode(NimBLEAdvertisedDevice& d) {
  return (d.getAdvFlags() & 0x01) != 0;
}

void refreshPreferredBondedDevice() {
  if (gPreferredBondedAddress.length() > 0 && isBondedAddress(gPreferredBondedAddress)) {
    return;
  }

  gPreferredBondedAddress = "";
  gPreferredBondedName = "";
  const int bondCount = NimBLEDevice::getNumBonds();
  if (bondCount <= 0) {
    return;
  }

  NimBLEAddress addr = NimBLEDevice::getBondedAddress(0);
  gPreferredBondedAddress = String(addr.toString().c_str());
}

const String& preferredBondedAddress() {
  return gPreferredBondedAddress;
}

const String& preferredBondedName() {
  return gPreferredBondedName;
}

void clearPreferredBondedDevice() {
  gPreferredBondedAddress = "";
  gPreferredBondedName = "";
}

bool pairKeyboard(const String& address, const String& nameHint) {
  if (isBondedAddress(address)) {
    gPreferredBondedAddress = address;
    gPreferredBondedName = nameHint;
    addKeyLog("Device already bonded");
    return true;
  }

  String pairReason;
  if (!canPairDeviceNow(address, pairReason)) {
    addKeyLog(String("Pair rejected: ") + pairReason);
    return false;
  }
  if (pairReason != "ok") {
    addKeyLog(pairReason);
  }

  if (!openKeyboardLink(address, nameHint)) {
    return false;
  }

  addKeyLog("Starting pairing");
  if (!gClient->secureConnection()) {
    addKeyLog("Pairing request failed");
    disconnectKeyboard();
    return false;
  }

  logConnectionSecurity("Post-pair security");
  bool bonded = isBondedAddress(address) || gClient->getConnInfo().isBonded();
  if (!bonded) {
    addKeyLog("Pairing finished without a stored bond");
    disconnectKeyboard();
    return false;
  }

  gPreferredBondedAddress = address;
  gPreferredBondedName = nameHint;
  addKeyLog("Bond stored; disconnecting until normal connect");
  disconnectKeyboard();
  return true;
}

bool unpairKeyboard(const String& address) {
  if (gConnectedAddress.equalsIgnoreCase(address)) {
    disconnectKeyboard();
  }

  bool removed = removeBondByAddress(address);
  bool stillBonded = isBondedAddress(address);
  if (!removed || stillBonded) {
    addKeyLog(String("Unpair failed: ") + address);
    return false;
  }

  addKeyLog(String("Unpaired: ") + address);
  if (gPreferredBondedAddress.equalsIgnoreCase(address)) {
    gPreferredBondedAddress = "";
    gPreferredBondedName = "";
  }
  refreshPreferredBondedDevice();
  return true;
}

bool connectToKeyboard(const String& address, const String& nameHint) {
  if (!isBondedAddress(address)) {
    addKeyLog("Connect rejected: device is not bonded. Pair it first.");
    return false;
  }

  if (!openKeyboardLink(address, nameHint)) {
    return false;
  }

  if (!gClient->getConnInfo().isEncrypted()) {
    addKeyLog("Restoring secure bonded connection");
    if (!gClient->secureConnection()) {
      addKeyLog("Secure reconnect failed");
      disconnectKeyboard();
      return false;
    }
  }

  gPreferredBondedAddress = address;
  gPreferredBondedName = nameHint;
  logConnectionSecurity("Ready to use");
  if (!subscribeToKeyboard()) {
    addKeyLog("Connected, but keyboard input subscribe failed");
    return false;
  }
  return true;
}

void disconnectKeyboard() {
  ::disconnectKeyboard();
}

void maybeAutoConnectBondedKeyboard() {
  if (gConnected) {
    return;
  }

  if (millis() - gLastAutoConnectAttemptMs < AUTO_CONNECT_INTERVAL_MS) {
    return;
  }

  refreshPreferredBondedDevice();
  if (gPreferredBondedAddress.length() == 0) {
    return;
  }

  gLastAutoConnectAttemptMs = millis();

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setInterval(80);
  scan->setWindow(40);
  scan->setActiveScan(true);
  scan->clearResults();

  NimBLEScanResults results = scan->start(2, false);
  for (int i = 0; i < results.getCount(); i++) {
    NimBLEAdvertisedDevice d = results.getDevice(i);
    String found = String(d.getAddress().toString().c_str());
    if (!found.equalsIgnoreCase(gPreferredBondedAddress)) {
      continue;
    }

    String name = d.haveName() ? String(d.getName().c_str()) : gPreferredBondedName;
    addKeyLog(String("Auto-connecting to bonded keyboard: ") + found);
    if (!connectToKeyboard(found, name)) {
      addKeyLog("Auto-connect failed");
    }
    return;
  }
}

void syncConnectionState() {
  if (gClient && gConnected && !gClient->isConnected()) {
    gConnected = false;
  }
}

bool isConnected() {
  return gConnected;
}

const String& connectedName() {
  return gConnectedName;
}

const String& connectedAddress() {
  return gConnectedAddress;
}

uint8_t lastKeyCode() {
  return gLastKeyCode;
}

} // namespace BLEKeyboard
