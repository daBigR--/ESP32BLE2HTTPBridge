#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WebServer.h>

#include <deque>
#include <vector>

#define HID_SERVICE_UUID      "1812"
#define HID_INPUT_REPORT_UUID "2A4D"

static const char* AP_SSID = "ESP32-Keyboard-Hub";
static const char* AP_PASSWORD = "12345678";

struct DiscoveredDevice {
    String name;
    String address;
    int rssi;
    bool bonded;
    bool seen;
    bool pairableNow;
};

WebServer server(80);

static NimBLEClient* gClient = nullptr;
static bool gConnected = false;
static String gConnectedName = "";
static String gConnectedAddress = "";

static std::vector<DiscoveredDevice> gDevices;
static std::deque<String> gKeyLog;
static const size_t MAX_KEY_LOG = 40;
static size_t gSubscribedCharacteristicCount = 0;
static String gPreferredBondedAddress = "";
static String gPreferredBondedName = "";
static unsigned long gLastAutoConnectAttemptMs = 0;
static const unsigned long AUTO_CONNECT_INTERVAL_MS = 8000;

void addKeyLog(const String& line);
bool isBondedAddress(const String& address);
void logConnectionSecurity(const String& prefix);
void disconnectKeyboard();
bool openKeyboardLink(const String& address, const String& nameHint);
bool pairKeyboard(const String& address, const String& nameHint);
bool unpairKeyboard(const String& address);
bool removeBondByAddress(const String& address);
void refreshPreferredBondedDevice();
void maybeAutoConnectBondedKeyboard();
bool isAdvertisedAsPairingMode(NimBLEAdvertisedDevice& d);
bool canPairDeviceNow(const String& address, String& reason);

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

String jsonEscape(const String& in) {
    String out = "";
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        const char c = in[i];
        if (c == '\\' || c == '"') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            out += c;
        }
    }
    return out;
}

void addKeyLog(const String& line) {
    if (line.length() == 0) {
        return;
    }
    gKeyLog.push_back(line);
    while (gKeyLog.size() > MAX_KEY_LOG) {
        gKeyLog.pop_front();
    }
    Serial.println(line);
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

  bool isAdvertisedAsPairingMode(NimBLEAdvertisedDevice& d) {
    // nRF Connect confirmed: keyboard sets LE Limited Discoverable Mode flag (0x01)
    // only when in explicit pairing mode, and clears it otherwise.
    return (d.getAdvFlags() & 0x01) != 0;
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
      if (isAdvertisedAsPairingMode(d)) {
        reason = "ok";
        return true;
      }

      reason = String("device not advertising for new pairing (advType=") + String(advType) + String(", flags=0x") + String(flags, HEX) + String(", directed=") + String(directed ? 1 : 0) + String(")");
      return false;
    }

    reason = "device not currently advertising";
    return false;
  }

static void notifyCallback(NimBLERemoteCharacteristic* pRemoteCharacteristic,
                           uint8_t* pData,
                           size_t length,
                           bool isNotify) {
    if (!isNotify || length < 3) return;
    // HID report: byte 0 = modifiers, byte 1 = reserved, bytes 2-7 = keycodes
    for (int i = 2; i < (int)length && i < 8; i++) {
        if (pData[i] != 0) {
            Serial.write(pData[i]);
            String line = "KEY 0x";
            if (pData[i] < 0x10) line += "0";
            line += String(pData[i], HEX);
            addKeyLog(line);
            break;
        }
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

void performScan() {
    gDevices.clear();

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setInterval(80);
    scan->setWindow(40);
    scan->setActiveScan(true);
    scan->clearResults();

    NimBLEScanResults results = scan->start(4, false);
    for (int i = 0; i < results.getCount(); i++) {
      NimBLEAdvertisedDevice d = results.getDevice(i);
      if (!d.haveName()) {
            continue;
        }

      String address = String(d.getAddress().toString().c_str());
      bool bonded = isBondedAddress(address);
      bool pairableNow = !bonded && isAdvertisedAsPairingMode(d);

      if (!bonded && !pairableNow) {
        continue;
      }

        DiscoveredDevice item;
        item.name = String(d.getName().c_str());
      item.address = address;
        item.rssi = d.getRSSI();
      item.bonded = bonded;
        item.seen = true;
      item.pairableNow = pairableNow;
        gDevices.push_back(item);
    }

      const int bondCount = NimBLEDevice::getNumBonds();
      for (int i = 0; i < bondCount; i++) {
        String bondedAddr = String(NimBLEDevice::getBondedAddress(i).toString().c_str());
        bool alreadyListed = false;
        for (DiscoveredDevice& device : gDevices) {
          if (device.address.equalsIgnoreCase(bondedAddr)) {
            device.bonded = true;
            alreadyListed = true;
            break;
          }
        }

        if (alreadyListed) {
          continue;
        }

        DiscoveredDevice item;
        item.address = bondedAddr;
        item.rssi = -127;
        item.bonded = true;
        item.seen = false;
        item.pairableNow = false;
        if (bondedAddr.equalsIgnoreCase(gPreferredBondedAddress) && gPreferredBondedName.length() > 0) {
          item.name = gPreferredBondedName;
        } else {
          item.name = "(bonded device)";
        }
        gDevices.push_back(item);
      }
}

String devicesJson() {
    String out = "[";
    for (size_t i = 0; i < gDevices.size(); i++) {
        if (i > 0) {
            out += ",";
        }
        out += "{\"name\":\"" + jsonEscape(gDevices[i].name) + "\",";
        out += "\"address\":\"" + jsonEscape(gDevices[i].address) + "\",";
        out += "\"rssi\":" + String(gDevices[i].rssi) + ",";
        out += "\"bonded\":";
        out += gDevices[i].bonded ? "true" : "false";
        out += ",\"seen\":";
        out += gDevices[i].seen ? "true" : "false";
        out += ",\"pairableNow\":";
        out += gDevices[i].pairableNow ? "true" : "false";
        out += "}";
    }
    out += "]";
    return out;
}

String keyLogJson() {
    String out = "[";
    size_t index = 0;
    for (const String& line : gKeyLog) {
        if (index++ > 0) {
            out += ",";
        }
        out += "\"" + jsonEscape(line) + "\"";
    }
    out += "]";
    return out;
}

void handleState() {
    String out = "{";
    out += "\"connected\":";
    out += gConnected ? "true" : "false";
    out += ",\"name\":\"" + jsonEscape(gConnectedName) + "\"";
    out += ",\"address\":\"" + jsonEscape(gConnectedAddress) + "\"";
    out += ",\"bondedAddress\":\"" + jsonEscape(gPreferredBondedAddress) + "\"";
    out += ",\"bondedName\":\"" + jsonEscape(gPreferredBondedName) + "\"";
    out += ",\"keys\":" + keyLogJson();
    out += "}";
    server.send(200, "application/json", out);
}

const char PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>ESP32 Keyboard Hub</title>
  <style>
    :root {
      --bg-a: #f6f7f2;
      --bg-b: #d9e7df;
      --card: #ffffff;
      --ink: #15201a;
      --muted: #486257;
      --line: #c5d3cb;
      --ok: #2c7d4f;
      --accent: #1f6252;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: "Trebuchet MS", "Segoe UI", sans-serif;
      background: radial-gradient(circle at top left, var(--bg-a), var(--bg-b));
      color: var(--ink);
      min-height: 100vh;
      padding: 16px;
    }
    .wrap {
      max-width: 980px;
      margin: 0 auto;
      display: grid;
      grid-template-columns: 1fr;
      gap: 14px;
    }
    .card {
      background: var(--card);
      border: 1px solid var(--line);
      border-radius: 14px;
      padding: 14px;
      box-shadow: 0 8px 26px rgba(0, 0, 0, 0.06);
    }
    h1 { margin: 4px 0 10px 0; font-size: 1.4rem; }
    h2 { margin: 0 0 10px 0; font-size: 1.1rem; }
    .row { display: flex; gap: 10px; flex-wrap: wrap; align-items: center; }
    .actions { display: flex; gap: 8px; flex-wrap: wrap; }
    button {
      border: 0;
      border-radius: 10px;
      padding: 9px 12px;
      background: var(--accent);
      color: #fff;
      font-weight: 700;
      cursor: pointer;
    }
    button.alt { background: #6b7f75; }
    button.warn { background: #a44a3f; }
    ul { list-style: none; margin: 0; padding: 0; }
    li {
      border: 1px solid var(--line);
      border-radius: 10px;
      padding: 10px;
      margin-bottom: 8px;
      display: flex;
      justify-content: space-between;
      gap: 10px;
      align-items: center;
      animation: fadeIn .22s ease;
    }
    @keyframes fadeIn {
      from { opacity: 0; transform: translateY(5px); }
      to { opacity: 1; transform: translateY(0); }
    }
    .mono { font-family: Consolas, monospace; font-size: 0.9rem; }
    .status {
      padding: 8px 10px;
      border-radius: 10px;
      background: #eef4f1;
      color: var(--muted);
      border: 1px solid var(--line);
    }
    .ok { color: var(--ok); font-weight: 700; }
    .pill {
      display: inline-block;
      margin-left: 8px;
      padding: 2px 8px;
      border-radius: 999px;
      font-size: 0.78rem;
      font-weight: 700;
      vertical-align: middle;
    }
    .pill.ok { background: #dceedd; }
    .pill.warn { background: #efe6cf; color: #765d12; }
    .log {
      height: 260px;
      overflow: auto;
      border: 1px dashed var(--line);
      border-radius: 10px;
      padding: 8px;
      background: #fbfcfb;
      font-family: Consolas, monospace;
      font-size: 0.92rem;
      line-height: 1.35;
    }
    @media (max-width: 680px) {
      .log { height: 220px; }
      li { flex-direction: column; align-items: flex-start; }
    }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card">
      <h1>ESP32 BLE Keyboard Hub</h1>
      <div class="row">
        <button onclick="scan()">Scan Keyboards</button>
      </div>
      <div id="state" class="status" style="margin-top:10px">Idle</div>
    </div>

    <div class="card">
      <h2>Discovered BLE Devices</h2>
      <ul id="devices"></ul>
    </div>

    <div class="card" id="bondedCard" style="display:none">
      <h2>Bonded Device</h2>
      <div id="bondedInfo"></div>
    </div>

    <div class="card">
      <h2>Pressed Keys</h2>
      <div id="keys" class="log"></div>
    </div>
  </div>

  <script>
    const elState = document.getElementById('state');
    const elDevices = document.getElementById('devices');
    const elKeys = document.getElementById('keys');

    function status(text) {
      elState.textContent = text;
    }

    async function scan() {
      status('Scanning for 4 seconds...');
      const r = await fetch('/scan');
      const data = await r.json();
      renderDevices(data.devices || []);
      status('Scan complete');
    }

    function renderDevices(devices) {
      elDevices.innerHTML = '';
      const unpaired = devices.filter(d => !d.bonded && d.pairableNow);
      if (!unpaired.length) {
        elDevices.innerHTML = '<li style="border:none;color:var(--muted)">No devices in pairing mode found.</li>';
        return;
      }
      for (const d of unpaired) {
        const li = document.createElement('li');
        const left = document.createElement('div');
        const seenText = d.seen ? 'In range' : 'Not in range';
        const seenClass = d.seen ? 'ok' : 'warn';
        const rssiText = d.seen ? ('RSSI ' + d.rssi) : 'offline';
        left.innerHTML = `<strong>${d.name}</strong><span class="pill ${seenClass}">${seenText}</span><div class="mono">${d.address} | ${rssiText}</div>`;
        const actions = document.createElement('div');
        actions.className = 'actions';
        const btn = document.createElement('button');
        btn.textContent = 'Pair';
        btn.onclick = () => pairDevice(d.address, d.name);
        actions.appendChild(btn);
        li.appendChild(left);
        li.appendChild(actions);
        elDevices.appendChild(li);
      }
    }

    function renderBondedPanel(s) {
      const card = document.getElementById('bondedCard');
      const info = document.getElementById('bondedInfo');
      if (!s.bondedAddress) {
        card.style.display = 'none';
        return;
      }
      card.style.display = '';
      const name = s.bondedName || s.bondedAddress;
      const connBadge = s.connected
        ? '<span class="pill ok">Connected</span>'
        : '<span class="pill warn">Disconnected</span>';
      info.innerHTML = `
        <div style="display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap">
          <div>
            <strong>${name}</strong>${connBadge}
            <div class="mono">${s.bondedAddress}</div>
          </div>
          <div class="actions" id="bondedActions"></div>
        </div>`;
      const acts = document.getElementById('bondedActions');
      if (s.connected) {
        const d = document.createElement('button');
        d.className = 'alt';
        d.textContent = 'Disconnect';
        d.onclick = disconnectNow;
        acts.appendChild(d);
      } else {
        const c = document.createElement('button');
        c.textContent = 'Connect';
        c.onclick = () => connectDevice(s.bondedAddress, s.bondedName);
        acts.appendChild(c);
      }
      const u = document.createElement('button');
      u.className = 'warn';
      u.textContent = 'Unpair';
      u.onclick = () => unpairDevice(s.bondedAddress, s.bondedName);
      acts.appendChild(u);
    }

    async function pairDevice(address, name) {
      status('Pairing with ' + address + ' ...');
      const r = await fetch('/pair?addr=' + encodeURIComponent(address) + '&name=' + encodeURIComponent(name));
      const data = await r.json();
      if (data.ok) {
        status('Paired with ' + name + '. Scan again or connect now.');
        await scan();
      } else {
        status(data.error || 'Pair failed');
      }
    }

    async function connectDevice(address, name) {
      status('Connecting to ' + address + ' ...');
      const r = await fetch('/connect?addr=' + encodeURIComponent(address) + '&name=' + encodeURIComponent(name));
      const data = await r.json();
      if (data.ok) {
        status('Connected to ' + name);
      } else {
        status(data.error || 'Connect failed');
      }
    }

    async function unpairDevice(address, name) {
      status('Unpairing ' + address + ' ...');
      const r = await fetch('/unpair?addr=' + encodeURIComponent(address));
      const data = await r.json();
      if (data.ok) {
        status('Unpaired ' + name);
        await scan();
      } else {
        status(data.error || 'Unpair failed');
      }
    }

    async function disconnectNow() {
      await fetch('/disconnect');
      status('Disconnected');
    }

    async function refreshState() {
      const r = await fetch('/state');
      const s = await r.json();
      const head = s.connected
        ? `Connected: ${s.name || '(unknown)'} (${s.address})`
        : 'Not connected';
      elState.innerHTML = s.connected ? `<span class="ok">${head}</span>` : head;
      elKeys.innerHTML = (s.keys || []).map(k => `<div>${k}</div>`).join('');
      elKeys.scrollTop = elKeys.scrollHeight;
      renderBondedPanel(s);
    }

    setInterval(refreshState, 500);
    refreshState();
  </script>
</body>
</html>
)HTML";

void handleScan() {
    performScan();
    String out = "{\"devices\":" + devicesJson() + "}";
    server.send(200, "application/json", out);
}

void handleConnect() {
    if (!server.hasArg("addr")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing addr\"}");
        return;
    }
    const String addr = server.arg("addr");
    const String name = server.hasArg("name") ? server.arg("name") : "";
    const bool ok = connectToKeyboard(addr, name);
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"connect failed or device not bonded\"}");
  }

  void handlePair() {
    if (!server.hasArg("addr")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing addr\"}");
      return;
    }
    const String addr = server.arg("addr");
    const String name = server.hasArg("name") ? server.arg("name") : "";
    const bool ok = pairKeyboard(addr, name);
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"pair failed (device may not accept new bonding now)\"}");
}

  void handleUnpair() {
    if (!server.hasArg("addr")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing addr\"}");
      return;
    }
    const String addr = server.arg("addr");
    const bool ok = unpairKeyboard(addr);
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"unpair failed\"}");
  }

void setup() {
    Serial.begin(115200);
    delay(800);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);

    NimBLEDevice::init("ESP32-KB-Receiver");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_DEFAULT);
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityCallbacks(new SecurityCallbacks());

    refreshPreferredBondedDevice();

    server.on("/", HTTP_GET, []() { server.send(200, "text/html", PAGE); });
    server.on("/scan", HTTP_GET, handleScan);
    server.on("/pair", HTTP_GET, handlePair);
    server.on("/connect", HTTP_GET, handleConnect);
    server.on("/unpair", HTTP_GET, handleUnpair);
    server.on("/disconnect", HTTP_GET, []() {
        disconnectKeyboard();
        server.send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/state", HTTP_GET, handleState);
    server.begin();

    Serial.println("\nESP32 BLE Keyboard Hub");
    Serial.print("Open GUI at: http://");
    Serial.println(WiFi.softAPIP());
    addKeyLog("GUI ready");
}

void loop() {
    server.handleClient();
    if (gClient && gConnected && !gClient->isConnected()) {
        gConnected = false;
    }
  maybeAutoConnectBondedKeyboard();
    delay(10);
}