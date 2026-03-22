#include "web_page.h"

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
