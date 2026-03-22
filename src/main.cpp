#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WebServer.h>
#include <algorithm>

#include <deque>
#include <vector>

#include "ble_keyboard.h"
#include "config_store.h"
#include "http_bridge.h"

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

static std::vector<DiscoveredDevice> gDevices;
static std::deque<String> gKeyLog;
static const size_t MAX_KEY_LOG = 40;

static String gBaseUrl = "";
static String gWifiSsid = "";
static String gWifiPassword = "";
static std::vector<KeyMapping> gKeyMappings;
static bool gConfigMode = true;

static const uint8_t CONFIG_BUTTON_PIN = D9;
static const unsigned long CONFIG_BUTTON_HOLD_MS = 800;

void addKeyLog(const String& line);
bool isConfigButtonHeldOnBoot();
String mappedPathForKey(uint8_t keyCode);
void handleConfigGet();
void handleSetUrl();
void handleSetWifi();
void handleSetMapping();
void handleDelMapping();
void handleReboot();
void handleFactoryReset();

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

bool isConfigButtonHeldOnBoot() {
  pinMode(CONFIG_BUTTON_PIN, INPUT);
  if (digitalRead(CONFIG_BUTTON_PIN) == HIGH) {
    unsigned long started = millis();
    while (millis() - started < CONFIG_BUTTON_HOLD_MS) {
      if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
        return false; // released before hold time
      }
      delay(10);
    }
    return true; // stayed HIGH for full hold period
  }
  return false;
}

String mappedPathForKey(uint8_t keyCode) {
  for (const KeyMapping& m : gKeyMappings) {
    if (m.keyCode == keyCode) {
      return m.path;
    }
  }
  return "";
}

String currentBaseUrl() {
  return gBaseUrl;
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
      bool bonded = BLEKeyboard::isBondedAddress(address);
      bool pairableNow = !bonded && BLEKeyboard::isAdvertisedAsPairingMode(d);

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
        if (bondedAddr.equalsIgnoreCase(BLEKeyboard::preferredBondedAddress()) && BLEKeyboard::preferredBondedName().length() > 0) {
          item.name = BLEKeyboard::preferredBondedName();
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
  out += BLEKeyboard::isConnected() ? "true" : "false";
  out += ",\"name\":\"" + jsonEscape(BLEKeyboard::connectedName()) + "\"";
  out += ",\"address\":\"" + jsonEscape(BLEKeyboard::connectedAddress()) + "\"";
  out += ",\"bondedAddress\":\"" + jsonEscape(BLEKeyboard::preferredBondedAddress()) + "\"";
  out += ",\"bondedName\":\"" + jsonEscape(BLEKeyboard::preferredBondedName()) + "\"";
    out += ",\"lastKey\":\"";
  if (BLEKeyboard::lastKeyCode() > 0) {
    if (BLEKeyboard::lastKeyCode() < 0x10) out += "0";
    out += String(BLEKeyboard::lastKeyCode(), HEX);
    }
    out += "\"";
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
    input[type=text] { border: 1px solid var(--line); border-radius: 8px; padding: 8px 10px; font-size: 0.95rem; background: #f8faf9; color: var(--ink); }
    .cfg-label { font-weight: 700; display: block; margin-bottom: 6px; }
    .cfg-section { margin-bottom: 16px; }
    .mapping-row { display: flex; align-items: center; gap: 10px; padding: 7px 0; border-bottom: 1px solid var(--line); }
    .mapping-row:last-child { border-bottom: none; }
    .captured-box { background: #eef4f1; border: 1px solid var(--line); border-radius: 8px; padding: 8px 12px; font-family: Consolas, monospace; font-size: 0.95rem; min-width: 70px; text-align: center; }
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
      <h2>Key Mapping Configuration</h2>
      <div class="cfg-section">
        <label class="cfg-label">WiFi Router</label>
        <div class="row" style="gap:8px;margin-bottom:8px">
          <input type="text" id="wifiSsidInput" placeholder="SSID" style="flex:1" />
          <input type="text" id="wifiPwdInput" placeholder="Password" style="flex:1" />
          <button onclick="saveWifi()">Save WiFi</button>
        </div>
      </div>
      <div class="cfg-section">
        <label class="cfg-label">Base URL</label>
        <div class="row" style="gap:8px">
          <input type="text" id="baseUrlInput" placeholder="http://192.168.x.x:8080" style="flex:1" />
          <button onclick="saveBaseUrl()">Save URL</button>
        </div>
      </div>
      <div class="cfg-section">
        <label class="cfg-label">Assign a Key</label>
        <div class="row" style="margin-bottom:8px;gap:8px">
          <button id="captureBtn" onclick="startCapture()">Capture Key</button>
          <span class="captured-box" id="capturedKey">&mdash;</span>
        </div>
        <div class="row" style="gap:8px">
          <input type="text" id="mappingPath" placeholder="/event/1" style="flex:1" />
          <button id="assignBtn" onclick="saveMapping()" disabled>Assign</button>
        </div>
      </div>
      <div class="cfg-section" style="margin-bottom:0">
        <label class="cfg-label">Current Mappings</label>
        <div id="mappingsList"></div>
      </div>
    </div>

    <div class="card">
      <h2>Device Control</h2>
      <div style="display:flex;gap:12px;flex-wrap:wrap;margin-bottom:10px">
        <button onclick="rebootDevice()">&#x21BA; Reboot</button>
        <button class="warn" onclick="factoryReset()">&#x26A0; Factory Reset</button>
      </div>
      <div style="color:var(--muted);font-size:0.85rem">
        <b>Reboot</b> restarts into run mode if config is complete.
        <b>Factory Reset</b> clears all settings &amp; unpairs the keyboard.
      </div>
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

    let capturing = false;
    let capturedKeyHex = '';
    let lastSeenKey = '';

    async function loadConfig() {
      const r = await fetch('/config');
      const c = await r.json();
      document.getElementById('wifiSsidInput').value = c.wifiSsid || '';
      document.getElementById('wifiPwdInput').value = c.wifiPassword || '';
      document.getElementById('baseUrlInput').value = c.baseUrl || '';
      renderMappings(c.mappings || []);
    }

    async function saveWifi() {
      const ssid = document.getElementById('wifiSsidInput').value.trim();
      const pwd = document.getElementById('wifiPwdInput').value;
      if (!ssid) { status('Enter WiFi SSID first'); return; }
      await fetch('/config/setwifi?ssid=' + encodeURIComponent(ssid) + '&pwd=' + encodeURIComponent(pwd));
      status('WiFi credentials saved');
    }

    function renderMappings(mappings) {
      const el = document.getElementById('mappingsList');
      if (!mappings.length) {
        el.innerHTML = '<div style="color:var(--muted);font-size:0.9rem">No mappings defined yet.</div>';
        return;
      }
      el.innerHTML = mappings.map(m =>
        `<div class="mapping-row">
          <span class="mono" style="min-width:52px">0x${m.key}</span>
          <span style="flex:1">${m.path}</span>
          <button class="warn" style="padding:5px 10px;font-size:0.8rem" onclick="deleteMapping('${m.key}')">Delete</button>
        </div>`
      ).join('');
    }

    async function saveBaseUrl() {
      const url = document.getElementById('baseUrlInput').value.trim();
      if (!url) { status('Enter a base URL first'); return; }
      await fetch('/config/seturl?url=' + encodeURIComponent(url));
      status('Base URL saved');
    }

    function startCapture() {
      capturing = true;
      capturedKeyHex = '';
      document.getElementById('capturedKey').textContent = '...';
      document.getElementById('assignBtn').disabled = true;
      document.getElementById('captureBtn').textContent = 'Waiting...';
      document.getElementById('captureBtn').disabled = true;
    }

    function checkCapture(lastKey) {
      if (!capturing || !lastKey || lastKey === lastSeenKey) return;
      capturing = false;
      capturedKeyHex = lastKey;
      document.getElementById('capturedKey').textContent = '0x' + lastKey;
      document.getElementById('assignBtn').disabled = false;
      document.getElementById('captureBtn').textContent = 'Capture Key';
      document.getElementById('captureBtn').disabled = false;
    }

    async function saveMapping() {
      if (!capturedKeyHex) return;
      const path = document.getElementById('mappingPath').value.trim();
      if (!path) { status('Enter a path first'); return; }
      await fetch('/config/setmapping?key=' + encodeURIComponent(capturedKeyHex) + '&path=' + encodeURIComponent(path));
      status('Mapped 0x' + capturedKeyHex + ' \u2192 ' + path);
      capturedKeyHex = '';
      document.getElementById('capturedKey').innerHTML = '&mdash;';
      document.getElementById('mappingPath').value = '';
      document.getElementById('assignBtn').disabled = true;
      await loadConfig();
    }

    async function deleteMapping(key) {
      await fetch('/config/delmapping?key=' + encodeURIComponent(key));
      await loadConfig();
    }

    async function rebootDevice() {
      if (!confirm('Reboot the device now?')) return;
      status('Rebooting...');
      await fetch('/reboot');
    }

    async function factoryReset() {
      if (!confirm('Factory reset will erase ALL settings and unpair the keyboard. Are you sure?')) return;
      status('Resetting...');
      await fetch('/factory-reset');
      status('Factory reset done. Device is rebooting.');
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
      if (s.lastKey && s.lastKey !== lastSeenKey) {
        checkCapture(s.lastKey);
        lastSeenKey = s.lastKey;
      }
    }

    setInterval(refreshState, 500);
    refreshState();
    loadConfig();
  </script>
</body>
</html>
)HTML";

void handleConfigGet() {
  server.send(200, "application/json", ConfigStore::configJson(gWifiSsid, gWifiPassword, gBaseUrl, gKeyMappings, jsonEscape));
}

void handleSetUrl() {
    if (!server.hasArg("url")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing url\"}");
        return;
    }
    gBaseUrl = server.arg("url");
    ConfigStore::save(gWifiSsid, gWifiPassword, gBaseUrl, gKeyMappings);
    addKeyLog(String("Base URL: ") + gBaseUrl);
    server.send(200, "application/json", "{\"ok\":true}");
}

  void handleSetWifi() {
    if (!server.hasArg("ssid")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ssid\"}");
      return;
    }
    gWifiSsid = server.arg("ssid");
    gWifiPassword = server.hasArg("pwd") ? server.arg("pwd") : "";
    ConfigStore::save(gWifiSsid, gWifiPassword, gBaseUrl, gKeyMappings);
    addKeyLog(String("WiFi SSID: ") + gWifiSsid);
    server.send(200, "application/json", "{\"ok\":true}");
  }

void handleSetMapping() {
    if (!server.hasArg("key") || !server.hasArg("path")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing key or path\"}");
        return;
    }
    uint8_t code = (uint8_t)strtol(server.arg("key").c_str(), nullptr, 16);
    String path = server.arg("path");
    for (auto& m : gKeyMappings) {
        if (m.keyCode == code) {
            m.path = path;
        ConfigStore::save(gWifiSsid, gWifiPassword, gBaseUrl, gKeyMappings);
            server.send(200, "application/json", "{\"ok\":true}");
            return;
        }
    }
    gKeyMappings.push_back({code, path});
    ConfigStore::save(gWifiSsid, gWifiPassword, gBaseUrl, gKeyMappings);
    addKeyLog(String("Map 0x") + String(code, HEX) + String(" -> ") + path);
    server.send(200, "application/json", "{\"ok\":true}");
}

void handleDelMapping() {
    if (!server.hasArg("key")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing key\"}");
        return;
    }
    uint8_t code = (uint8_t)strtol(server.arg("key").c_str(), nullptr, 16);
    size_t before = gKeyMappings.size();
    gKeyMappings.erase(
        std::remove_if(gKeyMappings.begin(), gKeyMappings.end(),
            [code](const KeyMapping& m){ return m.keyCode == code; }),
        gKeyMappings.end()
    );
    if (gKeyMappings.size() == before) {
        server.send(200, "application/json", "{\"ok\":false,\"error\":\"key not found\"}");
        return;
    }
    ConfigStore::save(gWifiSsid, gWifiPassword, gBaseUrl, gKeyMappings);
    addKeyLog(String("Del map 0x") + String(code, HEX));
    server.send(200, "application/json", "{\"ok\":true}");
}

void handleReboot() {
    server.send(200, "application/json", "{\"ok\":true}");
    delay(300);
    ESP.restart();
}

void handleFactoryReset() {
    NimBLEDevice::deleteAllBonds();
  ConfigStore::clearAll();
    gBaseUrl = "";
    gWifiSsid = "";
    gWifiPassword = "";
    gKeyMappings.clear();
    BLEKeyboard::clearPreferredBondedDevice();
    server.send(200, "application/json", "{\"ok\":true}");
    delay(300);
    ESP.restart();
}

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
    const bool ok = BLEKeyboard::connectToKeyboard(addr, name);
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"connect failed or device not bonded\"}");
  }

  void handlePair() {
    if (!server.hasArg("addr")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing addr\"}");
      return;
    }
    const String addr = server.arg("addr");
    const String name = server.hasArg("name") ? server.arg("name") : "";
    const bool ok = BLEKeyboard::pairKeyboard(addr, name);
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"pair failed (device may not accept new bonding now)\"}");
}

  void handleUnpair() {
    if (!server.hasArg("addr")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing addr\"}");
      return;
    }
    const String addr = server.arg("addr");
    const bool ok = BLEKeyboard::unpairKeyboard(addr);
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"unpair failed\"}");
  }

void setup() {
    Serial.begin(115200);

    // Check boot button FIRST, before any slow init, so the user doesn't
    // have to hold it for longer than CONFIG_BUTTON_HOLD_MS.
    bool forceConfigMode = isConfigButtonHeldOnBoot();

    delay(800);

    NimBLEDevice::init("ESP32-KB-Receiver");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_DEFAULT);
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    HttpBridge::begin(addKeyLog, currentBaseUrl, mappedPathForKey);
    BLEKeyboard::begin(addKeyLog, HttpBridge::onKeyPress);

    ConfigStore::load(gWifiSsid, gWifiPassword, gBaseUrl, gKeyMappings);
    addKeyLog(
      String("Config: wifi=") + (gWifiSsid.length() ? gWifiSsid : "(none)") +
      String(" url=") + (gBaseUrl.length() ? gBaseUrl : "(none)") +
      String(" maps=") + String(gKeyMappings.size())
    );
    BLEKeyboard::refreshPreferredBondedDevice();

    bool runConfigReady = ConfigStore::hasValidRunConfig(gWifiSsid, gBaseUrl, gKeyMappings, BLEKeyboard::preferredBondedAddress());
    gConfigMode = forceConfigMode || !runConfigReady;

    if (gConfigMode) {
      WiFi.mode(WIFI_AP);
      WiFi.softAP(AP_SSID, AP_PASSWORD);

      server.on("/", HTTP_GET, []() { server.send(200, "text/html", PAGE); });
      server.on("/scan", HTTP_GET, handleScan);
      server.on("/pair", HTTP_GET, handlePair);
      server.on("/connect", HTTP_GET, handleConnect);
      server.on("/unpair", HTTP_GET, handleUnpair);
      server.on("/disconnect", HTTP_GET, []() {
          BLEKeyboard::disconnectKeyboard();
          server.send(200, "application/json", "{\"ok\":true}");
      });
      server.on("/state", HTTP_GET, handleState);
      server.on("/config", HTTP_GET, handleConfigGet);
      server.on("/config/seturl", HTTP_GET, handleSetUrl);
      server.on("/config/setwifi", HTTP_GET, handleSetWifi);
      server.on("/config/setmapping", HTTP_GET, handleSetMapping);
      server.on("/config/delmapping", HTTP_GET, handleDelMapping);
      server.on("/reboot", HTTP_GET, handleReboot);
      server.on("/factory-reset", HTTP_GET, handleFactoryReset);
      server.begin();

      Serial.println("\nESP32 BLE Keyboard Hub - CONFIG mode");
      Serial.print("Open GUI at: http://");
      Serial.println(WiFi.softAPIP());
      addKeyLog("GUI ready");
    } else {
      WiFi.mode(WIFI_STA);
      WiFi.begin(gWifiSsid.c_str(), gWifiPassword.c_str());
      Serial.println("\nESP32 BLE Keyboard Hub - RUN mode");
      addKeyLog(String("RUN mode WiFi SSID: ") + gWifiSsid);
      addKeyLog("RUN mode: waiting for keyboard and mapped keypresses");
    }
}

void loop() {
    if (gConfigMode) {
      server.handleClient();
    }
    BLEKeyboard::syncConnectionState();
    BLEKeyboard::maybeAutoConnectBondedKeyboard();
    if (!gConfigMode) {
      HttpBridge::processPendingKeys();
    }
    delay(10);
}