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
    .pill.info { background: #d4e3f7; color: #1a4a7a; }
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
    input[type=text], input[type=number] { border: 1px solid var(--line); border-radius: 8px; padding: 8px 10px; font-size: 0.95rem; background: #f8faf9; color: var(--ink); }
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
      <h2 style="margin-top:16px">Discovered BLE Devices</h2>
      <ul id="devices"></ul>
    </div>

    <div class="card" id="bondedCard" style="display:none">
      <h2>Bonded Device</h2>
      <div id="bondedInfo"></div>
    </div>

    <div class="card">
      <h2>Configuration</h2>
      <div class="cfg-section">
        <label class="cfg-label">WiFi Networks</label>
        <div class="row" style="gap:8px;margin-bottom:8px">
          <input type="text" id="wifiSsidInput" placeholder="SSID" style="flex:1" />
          <input type="text" id="wifiPwdInput" placeholder="Password" style="flex:1" />
          <button onclick="addWifi()">Add</button>
        </div>
        <div id="wifiNetworksList" style="margin-bottom:8px"></div>
      </div>
      <div class="cfg-section">
        <label class="cfg-label">Power &amp; Sleep</label>
        <div style="font-size:0.82rem;color:var(--muted);margin-bottom:6px">Inactivity timeout before deep sleep (used in run mode when battery policy allows sleep).</div>
        <div class="row" style="gap:8px">
          <input type="number" id="sleepTimeoutMinInput" min="0.5" step="0.5" placeholder="10" style="width:140px" />
          <span style="color:var(--muted)">minutes</span>
          <button onclick="saveSleepTimeout()">Save Timeout</button>
        </div>
      </div>
      <div class="cfg-section" style="margin-top:22px">
        <label class="cfg-label">Base URLs</label>
        <div style="font-size:0.82rem;color:var(--muted);margin-bottom:6px">Short-press the device button to cycle URLs at runtime. Long-press (&ge;0.8&nbsp;s) to save the selection.</div>
        <div class="row" style="gap:8px;margin-bottom:8px">
          <input type="text" id="newUrlInput" placeholder="http://192.168.x.x:8080" style="flex:1" />
          <button id="urlActionBtn" onclick="addUrl()">Add URL</button>
          <button id="urlCancelBtn" class="alt" onclick="cancelUrlEdit()" style="display:none">Cancel</button>
        </div>
        <div id="baseUrlsList" style="margin-bottom:8px"></div>
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
          <button id="mappingCancelBtn" class="alt" onclick="cancelMappingEdit()" style="display:none">Cancel</button>
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
    let currentBondedAddress = ''; // tracks the selected bonded device address

    function status(text) {
      elState.textContent = text;
    }

    async function scan() {
      status('Scanning for 4 seconds...');
      const r = await fetch('/scan');
      const data = await r.json();
      renderDevices(data.devices || [], currentBondedAddress);
      status('Scan complete');
    }

    function renderDevices(devices, selectedBondedAddress = '') {
      elDevices.innerHTML = '';
      const seen = devices.filter(d => d.seen && d.address.toUpperCase() !== selectedBondedAddress.toUpperCase());
      if (!seen.length) {
        elDevices.innerHTML = '<li style="border:none;color:var(--muted)">No BLE devices found in this scan.</li>';
        return;
      }

      function byRssi(a, b) {
        return b.rssi - a.rssi;
      }

      // Separate named from unnamed.
      const isUnnamed = d => d.name === '(unnamed)' || d.name === '';
      const named = seen.filter(d => !isUnnamed(d));
      const unnamed = seen.filter(d => isUnnamed(d));

      // Within named: discoverable vs. not.
      const discoverableNamed = named.filter(d => d.pairableNow).sort(byRssi);
      const nonDiscoverableNamed = named.filter(d => !d.pairableNow).sort(byRssi);
      
      // Unnamed devices (will show with more detail in "Other devices" section).
      const otherDevices = unnamed.sort(byRssi);

      function addSectionHeader(label, color = 'var(--primary)') {
        const li = document.createElement('li');
        li.style.cssText = `border:none;padding:8px 0 4px;background:none;display:block;margin-top:12px`;
        li.innerHTML = `<span style="font-size:1.1em;font-weight:700;color:${color}">${label}</span>`;
        elDevices.appendChild(li);
      }

      function addNamedDeviceRow(d) {
        const li = document.createElement('li');
        const left = document.createElement('div');
        const rssiText = 'RSSI ' + d.rssi;
        const bondPill = d.bonded
          ? '<span class="pill info">Bonded</span>'
          : '<span class="pill warn">Unbonded</span>';
        left.innerHTML = `<strong>${d.name}</strong>${bondPill}<div class="mono">${d.address} | ${rssiText}</div>`;
        const actions = document.createElement('div');
        actions.className = 'actions';
        if (!d.bonded) {
          const btn = document.createElement('button');
          btn.textContent = 'Pair';
          btn.onclick = () => pairDevice(d.address, d.name);
          actions.appendChild(btn);
        } else {
          const btn = document.createElement('button');
          btn.className = 'warn';
          btn.textContent = 'Unpair';
          btn.onclick = () => unpairDevice(d.address, d.name);
          actions.appendChild(btn);
        }
        li.appendChild(left);
        li.appendChild(actions);
        elDevices.appendChild(li);
      }

      function addUnnamedDeviceRow(d) {
        const li = document.createElement('li');
        const left = document.createElement('div');
        const rssiText = 'RSSI ' + d.rssi;
        const discoverablePill = d.pairableNow
          ? '<span class="pill ok">Discoverable</span>'
          : '<span class="pill warn">Not discoverable</span>';
        const bondPill = d.bonded
          ? '<span class="pill info">Bonded</span>'
          : '<span class="pill warn">Unbonded</span>';
        left.innerHTML = `<strong>(no name)</strong>${bondPill}${discoverablePill}<div class="mono">${d.address} | ${rssiText}</div>`;
        const actions = document.createElement('div');
        actions.className = 'actions';
        if (!d.bonded) {
          const btn = document.createElement('button');
          btn.textContent = 'Pair';
          btn.onclick = () => pairDevice(d.address, d.name);
          actions.appendChild(btn);
        } else {
          const btn = document.createElement('button');
          btn.className = 'warn';
          btn.textContent = 'Unpair';
          btn.onclick = () => unpairDevice(d.address, d.name);
          actions.appendChild(btn);
        }
        li.appendChild(left);
        li.appendChild(actions);
        elDevices.appendChild(li);
      }

      if (discoverableNamed.length) {
        addSectionHeader('✓ Discoverable (' + discoverableNamed.length + ')', 'var(--ok)');
        discoverableNamed.forEach(addNamedDeviceRow);
      }
      if (nonDiscoverableNamed.length) {
        addSectionHeader('⊘ Not Discoverable (' + nonDiscoverableNamed.length + ')', 'var(--warn)');
        nonDiscoverableNamed.forEach(addNamedDeviceRow);
      }
      if (otherDevices.length) {
        addSectionHeader('◇ Other Devices (' + otherDevices.length + ')', 'var(--muted)');
        otherDevices.forEach(addUnnamedDeviceRow);
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
        status('Paired with ' + name + '. Waiting for automatic reconnect...');
        await refreshState();
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
    let urlEditIndex = -1;
    let mappingEditOriginalKey = '';
    let currentSleepTimeoutMs = 10 * 60 * 1000;

    function renderSleepTimeout(timeoutMs) {
      const ms = Number(timeoutMs);
      if (!Number.isFinite(ms) || ms <= 0) return;
      currentSleepTimeoutMs = Math.round(ms);
      document.getElementById('sleepTimeoutMinInput').value = (currentSleepTimeoutMs / 60000).toFixed(1);
    }

    async function loadConfig() {
      const r = await fetch('/config');
      const c = await r.json();
      renderWifiNetworks(c.wifiNetworks || []);
      renderBaseUrls(c.baseUrls || [], c.selectedUrlIndex || 0);
      renderSleepTimeout(c.sleepTimeoutMs || (10 * 60 * 1000));
      renderMappings(c.mappings || []);
    }

    async function saveSleepTimeout() {
      const raw = document.getElementById('sleepTimeoutMinInput').value;
      const minutes = parseFloat(raw);
      if (!Number.isFinite(minutes) || minutes <= 0) {
        status('Enter a valid timeout in minutes');
        return;
      }
      const ms = Math.round(minutes * 60000);
      const r = await fetch('/config/setsleeptimeout?ms=' + encodeURIComponent(ms));
      const data = await r.json();
      if (!data.ok) {
        status(data.error || 'Failed to save timeout');
        return;
      }
      renderSleepTimeout(data.sleepTimeoutMs || ms);
      status('Sleep timeout saved');
    }

    let wifiNets = [];

    function renderWifiNetworks(nets) {
      wifiNets = nets;
      const el = document.getElementById('wifiNetworksList');
      if (!nets.length) {
        el.innerHTML = '<div style="color:var(--muted);font-size:0.9rem">No networks saved.</div>';
        return;
      }
      el.innerHTML = nets.map((n, i) =>
        `<div class="mapping-row"><span style="flex:1">${n.ssid}</span><button class="warn" style="padding:5px 10px;font-size:0.8rem" onclick="deleteWifi(${i})">Del</button></div>`
      ).join('');
    }

    async function addWifi() {
      const ssid = document.getElementById('wifiSsidInput').value.trim();
      const pwd = document.getElementById('wifiPwdInput').value;
      if (!ssid) { status('Enter WiFi SSID first'); return; }
      await fetch('/config/addwifi?ssid=' + encodeURIComponent(ssid) + '&pwd=' + encodeURIComponent(pwd));
      document.getElementById('wifiSsidInput').value = '';
      document.getElementById('wifiPwdInput').value = '';
      await loadConfig();
      status('WiFi network saved');
    }

    async function deleteWifi(idx) {
      const net = wifiNets[idx];
      if (!net) return;
      await fetch('/config/delwifi?ssid=' + encodeURIComponent(net.ssid));
      await loadConfig();
      status('WiFi network removed');
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
          <button class="alt" style="padding:5px 10px;font-size:0.8rem" onclick="beginEditMapping('${m.key}', '${encodeURIComponent(m.path)}')">Edit</button>
          <button class="warn" style="padding:5px 10px;font-size:0.8rem" onclick="deleteMapping('${m.key}')">Delete</button>
        </div>`
      ).join('');
    }

    let baseUrlsList = [];

    function renderBaseUrls(urls, selectedIdx) {
      baseUrlsList = urls;
      const el = document.getElementById('baseUrlsList');
      if (!urls.length) {
        el.innerHTML = '<div style="color:var(--muted);font-size:0.9rem">No URLs configured.</div>';
        return;
      }
      el.innerHTML = urls.map((u, i) => {
        const badge = (i === selectedIdx)
          ? '<span class="pill ok" style="margin-left:6px">Active</span>'
          : '';
        const activateBtn = (i === selectedIdx)
          ? '<button class="alt" style="padding:5px 10px;font-size:0.8rem" disabled>Active</button>'
          : '<button style="padding:5px 10px;font-size:0.8rem" onclick="activateUrl(' + i + ')">Activate</button>';
        return `<div class="mapping-row">
          <span style="flex:1" class="mono">${u}${badge}</span>
          ${activateBtn}
          <button class="alt" style="padding:5px 10px;font-size:0.8rem" onclick="beginEditUrl(${i})">Edit</button>
          <button class="warn" style="padding:5px 10px;font-size:0.8rem" onclick="deleteUrl(${i})">Del</button>
        </div>`;
      }).join('');
    }

    async function activateUrl(idx) {
      await fetch('/config/selecturl?idx=' + idx);
      await loadConfig();
      status('Active URL set to #' + (idx + 1));
    }

    async function addUrl() {
      const url = document.getElementById('newUrlInput').value.trim();
      if (!url) { status('Enter a URL first'); return; }
      if (urlEditIndex >= 0) {
        await fetch('/config/editurl?idx=' + urlEditIndex + '&url=' + encodeURIComponent(url));
        status('URL updated');
      } else {
        await fetch('/config/addurl?url=' + encodeURIComponent(url));
        status('URL added');
      }
      cancelUrlEdit();
      await loadConfig();
    }

    function beginEditUrl(idx) {
      if (idx < 0 || idx >= baseUrlsList.length) return;
      urlEditIndex = idx;
      document.getElementById('newUrlInput').value = baseUrlsList[idx];
      document.getElementById('urlActionBtn').textContent = 'Update URL';
      document.getElementById('urlCancelBtn').style.display = '';
      status('Editing URL #' + (idx + 1));
    }

    function cancelUrlEdit() {
      urlEditIndex = -1;
      document.getElementById('newUrlInput').value = '';
      document.getElementById('urlActionBtn').textContent = 'Add URL';
      document.getElementById('urlCancelBtn').style.display = 'none';
    }

    async function deleteUrl(idx) {
      await fetch('/config/delurl?idx=' + idx);
      if (urlEditIndex === idx) {
        cancelUrlEdit();
      } else if (urlEditIndex > idx) {
        urlEditIndex -= 1;
      }
      await loadConfig();
      status('URL removed');
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

    function beginEditMapping(key, encodedPath) {
      capturing = false;
      capturedKeyHex = key;
      mappingEditOriginalKey = key;
      document.getElementById('capturedKey').textContent = '0x' + key;
      document.getElementById('mappingPath').value = decodeURIComponent(encodedPath);
      document.getElementById('assignBtn').disabled = false;
      document.getElementById('assignBtn').textContent = 'Update';
      document.getElementById('mappingCancelBtn').style.display = '';
      document.getElementById('captureBtn').textContent = 'Capture Key';
      document.getElementById('captureBtn').disabled = false;
      status('Editing mapping 0x' + key);
    }

    function cancelMappingEdit() {
      capturing = false;
      capturedKeyHex = '';
      mappingEditOriginalKey = '';
      document.getElementById('capturedKey').innerHTML = '&mdash;';
      document.getElementById('mappingPath').value = '';
      document.getElementById('assignBtn').disabled = true;
      document.getElementById('assignBtn').textContent = 'Assign';
      document.getElementById('mappingCancelBtn').style.display = 'none';
      document.getElementById('captureBtn').textContent = 'Capture Key';
      document.getElementById('captureBtn').disabled = false;
    }

    async function saveMapping() {
      if (!capturedKeyHex) return;
      const path = document.getElementById('mappingPath').value.trim();
      if (!path) { status('Enter a path first'); return; }
      if (mappingEditOriginalKey && mappingEditOriginalKey !== capturedKeyHex) {
        await fetch('/config/delmapping?key=' + encodeURIComponent(mappingEditOriginalKey));
      }
      await fetch('/config/setmapping?key=' + encodeURIComponent(capturedKeyHex) + '&path=' + encodeURIComponent(path));
      status((mappingEditOriginalKey ? 'Updated ' : 'Mapped ') + '0x' + capturedKeyHex + ' \u2192 ' + path);
      cancelMappingEdit();
      await loadConfig();
    }

    async function deleteMapping(key) {
      await fetch('/config/delmapping?key=' + encodeURIComponent(key));
      if (mappingEditOriginalKey === key) {
        cancelMappingEdit();
      }
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
      currentBondedAddress = s.bondedAddress || '';
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
