#include "web_page.h"

// =============================================================================
// web_page.cpp -- Single-page config UI served in CONFIG mode
// =============================================================================
//
// Layout: persistent sticky header + 4 tabs (General / BLE / Actions / System)
//   Header  -- bonded keyboard status dot + name, conditional Connect button,
//             Apply & Run primary action (opens confirmation modal -- /reboot)
//   General -- WiFi networks, Power & Sleep timeout
//   BLE     -- Keyboard scan, discovered devices, bonded device detail
//   Actions -- Base URLs, button capture + assignment, current mappings
//   System  -- Reboot, Factory Reset, Recent Bursts feed, Pressed Keys log
//
// All existing HTTP endpoints are unchanged.  This is a pure frontend file.
// =============================================================================

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
      --warn: #a44a3f;
      --accent: #1f6252;
      --header-h: 56px;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: "Trebuchet MS", "Segoe UI", sans-serif;
      background: radial-gradient(circle at top left, var(--bg-a), var(--bg-b));
      color: var(--ink);
      min-height: 100vh;
    }

    /* -- Header -- */
    #appHeader {
      position: sticky;
      top: 0;
      z-index: 100;
      background: var(--accent);
      color: #fff;
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 0 16px;
      height: var(--header-h);
      gap: 12px;
      box-shadow: 0 2px 8px rgba(0,0,0,0.18);
    }
    #bondedGroup {
      display: flex;
      align-items: center;
      gap: 10px;
      flex: 1;
      min-width: 0;
      overflow: hidden;
    }
    #bondedStatus {
      display: flex;
      align-items: center;
      gap: 8px;
      cursor: pointer;
      min-width: 0;
      overflow: hidden;
    }
    #bondedName {
      font-weight: 700;
      font-size: 0.95rem;
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
      color: #fff;
    }
    .dot {
      width: 11px;
      height: 11px;
      border-radius: 50%;
      flex-shrink: 0;
    }
    .dot-connected { background: #5fe89a; box-shadow: 0 0 6px #5fe89a; }
    .dot-bonded    { background: #a0a0a0; }
    .dot-none      { background: #555; border: 1px solid #888; }
    #headerActions {
      display: flex;
      align-items: center;
      gap: 8px;
      flex-shrink: 0;
    }
    #connectBtn {
      background: rgba(255,255,255,0.18);
      border: 1px solid rgba(255,255,255,0.45);
      color: #fff;
      border-radius: 8px;
      padding: 6px 12px;
      font-weight: 700;
      font-size: 0.85rem;
      cursor: pointer;
      font-family: inherit;
    }
    #applyRunBtn {
      background: #fff;
      color: var(--accent);
      border: none;
      border-radius: 8px;
      padding: 7px 14px;
      font-weight: 700;
      font-size: 0.88rem;
      cursor: pointer;
      white-space: nowrap;
      font-family: inherit;
    }

    /* -- Tab bar -- */
    #tabBar {
      position: sticky;
      top: var(--header-h);
      z-index: 99;
      background: #c2d5cb;
      border-bottom: 3px solid #9cb8b0;
      display: flex;
      padding: 6px 8px 0;
      gap: 3px;
    }
    .tab-btn {
      flex: 1;
      background: rgba(255,255,255,0.30);
      border: 1px solid rgba(0,0,0,0.10);
      border-bottom: none;
      border-radius: 8px 8px 0 0;
      padding: 9px 4px 8px;
      font-size: 0.88rem;
      font-weight: 700;
      color: var(--muted);
      cursor: pointer;
      text-align: center;
      font-family: inherit;
      transition: background .15s, color .15s;
    }
    .tab-btn:hover:not(.active) {
      background: rgba(255,255,255,0.55);
      color: var(--ink);
    }
    .tab-btn.active {
      background: var(--card);
      color: var(--accent);
      border-color: #9cb8b0;
      position: relative;
      /* cover the bar border so active tab appears to sit on top */
      margin-bottom: -3px;
      padding-bottom: 11px;
    }

    /* -- Status bar -- */
    #state {
      background: #eef4f1;
      color: var(--ink);
      font-size: 0.92rem;
      font-weight: 600;
      padding: 8px 12px;
      border-radius: 8px;
      border: 1px solid var(--line);
      border-left: 4px solid var(--accent);
      margin-top: 10px;
      min-height: 32px;
    }
    #state.state-ok   { background:#e6f4ec; border-left-color:var(--ok);   color:var(--ok); }
    #state.state-error{ background:#fdf0ef; border-left-color:var(--warn);  color:var(--warn); }
    #state.state-busy { background:#fff8e6; border-left-color:#c07a00; color:#c07a00; }

    /* -- Layout -- */
    .wrap { max-width: 980px; margin: 0 auto; }
    .tab-panel { display: none; padding: 16px; }
    .tab-panel.active { display: block; }

    /* -- Card -- */
    .card {
      background: var(--card);
      border: 1px solid var(--line);
      border-radius: 14px;
      padding: 14px;
      box-shadow: 0 8px 26px rgba(0,0,0,0.06);
      margin-bottom: 14px;
    }
    h2 { margin: 0 0 10px 0; font-size: 1.1rem; }
    .row { display: flex; gap: 10px; flex-wrap: wrap; align-items: center; }
    .actions { display: flex; gap: 8px; flex-wrap: wrap; }

    /* -- Buttons -- */
    button {
      border: 0;
      border-radius: 10px;
      padding: 9px 12px;
      background: var(--accent);
      color: #fff;
      font-weight: 700;
      cursor: pointer;
      font-family: inherit;
      font-size: 0.9rem;
      transition: background 0.15s;
    }
    button.alt  { background: #6b7f75; }
    button.warn { background: var(--warn); }
    button:disabled { opacity: 0.4; cursor: not-allowed; }
    .btn-flash-saved { background: var(--ok) !important; color: #fff !important; }
    .btn-flash-busy  { background: #fff8e6 !important; color: #c07a00 !important; }
    .btn-flash-error { background: var(--warn) !important; color: #fff !important; }

    /* -- Lists -- */
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
      to   { opacity: 1; transform: translateY(0); }
    }

    /* -- Misc -- */
    .mono { font-family: Consolas, monospace; font-size: 0.9rem; }
    .ok   { color: var(--ok); font-weight: 700; }
    .pill {
      display: inline-block;
      margin-left: 8px;
      padding: 2px 8px;
      border-radius: 999px;
      font-size: 0.78rem;
      font-weight: 700;
      vertical-align: middle;
    }
    .pill.ok   { background: #dceedd; color: var(--ok); }
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
    input[type=text], input[type=number], input[type=search] {
      border: 1px solid var(--line);
      border-radius: 8px;
      padding: 8px 10px;
      font-size: 0.95rem;
      background: #f8faf9;
      color: var(--ink);
      font-family: inherit;
    }
    .cfg-label { font-weight: 700; display: block; margin-bottom: 6px; }
    .cfg-section { margin-bottom: 16px; }
    .captured-box {
      background: #eef4f1;
      border: 1px solid var(--line);
      border-radius: 8px;
      padding: 8px 12px;
      font-family: Consolas, monospace;
      font-size: 0.95rem;
      min-width: 70px;
      text-align: center;
    }

    /* -- Modal -- */
    .empty-state {
      padding: 18px 4px;
      color: #888;
      font-size: 0.88rem;
      font-style: italic;
      text-align: left;
    }
    .inline-error {
      color: var(--warn);
      font-size: 0.85rem;
      margin-top: 8px;
      padding: 6px 10px;
      background: #fdf0ef;
      border-radius: 8px;
      border: 1px solid #e8c4c0;
      display: none;
    }
    .modal-error {
      color: var(--warn);
      font-size: 0.88rem;
      padding: 8px 10px;
      background: #fdf0ef;
      border-radius: 8px;
      border: 1px solid #e8c4c0;
      margin-bottom: 12px;
      display: none;
    }

    .modal-bg {
      display: none;
      position: fixed;
      inset: 0;
      background: rgba(0,0,0,0.45);
      z-index: 200;
      align-items: center;
      justify-content: center;
    }
    .modal-bg.open { display: flex; }
    .modal {
      background: var(--card);
      border-radius: 16px;
      padding: 24px;
      max-width: 380px;
      width: 90%;
      box-shadow: 0 16px 48px rgba(0,0,0,0.22);
    }
    .modal h3 { margin: 0 0 12px 0; }
    .modal p  { margin: 0 0 20px 0; font-size: 0.93rem; color: var(--muted); line-height: 1.5; }
    .modal .actions { justify-content: flex-end; }

    /* -- Responsive -- */
    @media (max-width: 480px) {
      .log { height: 200px; }
      li { flex-direction: column; align-items: flex-start; }
      #bondedName  { font-size: 0.85rem; }
      .tab-btn     { font-size: 0.78rem; padding: 8px 2px 7px; }
      #applyRunBtn { font-size: 0.8rem; padding: 6px 10px; }
      #connectBtn  { font-size: 0.8rem; padding: 6px 10px; }
    }

    /* -- KOReader event picker -------------------------------------------- */
    .picker-panel {
      border: 1px solid var(--line);
      border-radius: 10px;
      background: #f8faf9;
      padding: 10px;
      margin-top: 10px;
    }
    .picker-event-row {
      padding: 8px 10px;
      border-radius: 8px;
      cursor: pointer;
      border-bottom: 1px solid var(--line);
    }
    .picker-event-row:last-child { border-bottom: none; }
    .picker-event-row:hover { background: #e8f2ee; }
    .picker-section-hdr {
      display: flex;
      justify-content: space-between;
      align-items: center;
      font-weight: 700;
      font-size: 0.9rem;
      padding: 7px 10px;
      cursor: pointer;
      background: #eef4f1;
      border-radius: 8px;
      margin-top: 4px;
      user-select: none;
    }
    .reverse-flow-status {
      background: #fff8e6;
      border: 1px solid #e0c060;
      border-radius: 8px;
      padding: 10px 12px;
      font-size: 0.9rem;
      color: #7a5c00;
      margin-top: 8px;
      display: flex;
      align-items: center;
      gap: 10px;
    }

    /* -- Test button in header -------------------------------------------- */
    #testBtn {
      background: rgba(255,255,255,0.15);
      border: 1px solid rgba(255,255,255,0.40);
      color: #fff;
      border-radius: 8px;
      padding: 6px 12px;
      font-weight: 700;
      font-size: 0.85rem;
      cursor: pointer;
      font-family: inherit;
      transition: background 0.15s;
    }
    #testBtn.test-active {
      background: #fff8e6;
      border-color: #e0c060;
      color: #c07a00;
    }
    .hdr-divider {
      width: 1px;
      height: 26px;
      background: rgba(255,255,255,0.28);
      flex-shrink: 0;
    }

    /* -- Test Mode bottom panel ------------------------------------------- */
    #testPanel {
      position: fixed;
      bottom: 0;
      left: 0;
      right: 0;
      z-index: 150;
      background: #1a2e25;
      color: #d4ede1;
      border-top: 2px solid #3a6b55;
      transform: translateY(100%);
      transition: transform 0.22s cubic-bezier(0.4,0,0.2,1);
      box-shadow: 0 -4px 24px rgba(0,0,0,0.30);
      font-family: inherit;
    }
    #testPanel.tp-visible  { transform: translateY(0); }
    #testPanel.tp-compact  { height: 40px; overflow: hidden; }
    #testPanel.tp-expanded { height: 35vh; display: flex; flex-direction: column; }

    #testCompactBar {
      height: 40px;
      display: flex;
      align-items: center;
      padding: 0 12px;
      gap: 10px;
      font-size: 0.84rem;
      white-space: nowrap;
      overflow: hidden;
    }
    .tp-expanded #testCompactBar { display: none; }
    .tp-compact  #testExpandedView { display: none; }

    #testExpandedView {
      display: flex;
      flex-direction: column;
      flex: 1;
      overflow: hidden;
    }
    #testExpandedHdr {
      display: block;
      padding: 8px 12px 0;
      border-bottom: 1px solid rgba(255,255,255,0.15);
      flex-shrink: 0;
      font-size: 0.82rem;
    }
    #testFiresLog {
      flex: 1;
      overflow-y: auto;
      padding: 6px 10px;
      font-family: Consolas, monospace;
      font-size: 0.80rem;
      line-height: 1.5;
    }
    .tp-fire-entry { padding: 3px 0; border-bottom: 1px solid rgba(255,255,255,0.07); word-break: break-all; }
    .tp-fire-ok    { color: #6fe8a0; }
    .tp-fire-err   { color: #f08080; }
    .tp-fire-unmap { color: #a0a0a0; }
    #testCompactLast {
      flex: 1;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
      color: #a0c8b0;
      font-size: 0.80rem;
    }
    .dot-test-green { background: #5fe89a; box-shadow: 0 0 6px #5fe89a; }
    .dot-test-amber { background: #f0b840; }
    .dot-test-red   { background: #f07070; }
    .tp-btn {
      background: rgba(255,255,255,0.12);
      border: 1px solid rgba(255,255,255,0.25);
      color: #d4ede1;
      border-radius: 6px;
      padding: 3px 9px;
      cursor: pointer;
      font-family: inherit;
      font-size: 0.82rem;
      flex-shrink: 0;
    }
    .tp-btn:hover { background: rgba(255,255,255,0.22); }
  </style>
</head>
<body>

  <!-- -- Sticky header -- -->
  <div id="appHeader">
    <div id="bondedGroup">
      <div id="bondedStatus" onclick="switchTab('ble')" title="Go to BLE tab">
        <span id="statusDot" class="dot dot-none"></span>
        <span id="bondedName">No device bonded</span>
      </div>
      <button id="connectBtn" onclick="connectBonded()" style="display:none">Connect</button>
    </div>
    <div id="headerActions">
      <button id="testBtn" onclick="toggleTestMode()">Test</button>
      <div class="hdr-divider"></div>
      <button id="applyRunBtn" onclick="openApplyModal()">Exit &amp; Run</button>
    </div>
  </div>

  <!-- -- Tab bar -- -->
  <div id="tabBar">
    <button class="tab-btn" data-tab="general" onclick="switchTab('general')">WiFi</button>
    <button class="tab-btn" data-tab="ble"     onclick="switchTab('ble')">Bluetooth</button>
    <button class="tab-btn" data-tab="actions" onclick="switchTab('actions')">Actions</button>
    <button class="tab-btn" data-tab="system"  onclick="switchTab('system')">System</button>
  </div>

  <!-- -- Tab panels -- -->
  <div class="wrap">

    <!-- GENERAL -- -->
    <div id="tab-general" class="tab-panel">
      <div class="card">
        <h2>WiFi Networks</h2>
        <div class="cfg-section">
          <div class="row" style="gap:8px;margin-bottom:8px">
            <input type="text" id="wifiSsidInput" placeholder="SSID" style="flex:1" autocapitalize="none" autocorrect="off" />
            <input type="text" id="wifiPwdInput" placeholder="Password" style="flex:1" autocapitalize="none" autocorrect="off" />
            <button onclick="addWifi()">Add</button>
          </div>
          <div class="row" style="margin-bottom:8px">
            <button id="wifiScanBtn" onclick="scanWifi()">Scan Networks</button>
          </div>
          <div id="wifiScanSection" style="display:none;margin-top:20px">
            <div style="font-weight:700;font-size:0.95rem;margin-bottom:6px">Available Networks</div>
            <div id="wifiScanResults" style="background:#eef5f1;border:1.5px dashed var(--line);border-radius:8px;padding:8px 4px"></div>
          </div>
          <div style="margin-top:28px;font-weight:700;font-size:0.95rem;margin-bottom:6px">Saved Networks</div>
          <div id="wifiNetworksList"></div>
          <div class="inline-error" id="wifiError"></div>
        </div>
      </div>
    </div>

    <!-- BLE -- -->
    <div id="tab-ble" class="tab-panel">
      <div class="card">
        <h2>Keyboard Scan</h2>
        <div class="row">
          <button onclick="scan()">Scan Devices</button>
        </div>
        <div id="state">Idle</div>
        <div id="bleDiscoveredSection" style="display:none">
          <h2 style="margin-top:16px">Discovered Devices</h2>
          <div id="bleScanSection" style="background:#eef5f1;border:1.5px dashed var(--line);border-radius:8px;padding:4px">
            <ul id="devices"></ul>
          </div>
        </div>
      </div>
      <div class="card" id="bondedCard" style="display:none">
        <h2>Bonded Device</h2>
        <div id="bondedInfo"></div>
      </div>
    </div>

    <!-- ACTIONS -- -->
    <div id="tab-actions" class="tab-panel">
      <div class="card">
        <h2>Base URLs</h2>
        <div class="cfg-section" style="margin-bottom:0">
          <div style="font-size:0.82rem;color:var(--muted);margin-bottom:6px">In run mode button short press cycles base URLs, double press saves the selection.</div>
          <div class="row" style="gap:8px;margin-bottom:8px">
            <input type="text" id="newUrlInput" placeholder="http://192.168.x.x:8080" style="flex:1" autocapitalize="none" autocorrect="off" />
            <button id="urlActionBtn" onclick="addUrl()">Add</button>
            <button id="urlCancelBtn" class="alt" onclick="cancelUrlEdit()" style="display:none">Cancel</button>
          </div>
          <div id="baseUrlsList"></div>
          <div class="inline-error" id="urlError"></div>
        </div>
      </div>
      <div class="card">
        <h2>Assign a Button</h2>
        <div class="cfg-section">
          <div class="row" style="gap:8px;margin-bottom:6px;flex-wrap:wrap">
            <button id="captureBtn" onclick="startCapture()">Capture Key</button>
            <span class="captured-box" id="capturedKey">&mdash;</span>
            <input type="text" id="mappingUrl" placeholder="/event/1" style="flex:2;min-width:120px" autocapitalize="none" autocorrect="off" />
            <input type="text" id="mappingLabel" placeholder="Label (optional)" style="flex:1;min-width:100px" autocapitalize="none" autocorrect="off" />
            <button id="assignBtn" onclick="saveMapping()" disabled>Assign</button>
            <button id="mappingCancelBtn" class="alt" onclick="cancelMappingEdit()" style="display:none">Cancel</button>
          </div>
          <div class="row" style="margin-bottom:8px">
            <button onclick="openEventPicker('reverse')">Scan Events</button>
          </div>
          <div id="reverseFlowStatus" class="reverse-flow-status" style="display:none">
            <span style="flex:1">Press a button on the connected BLE device to bind to <strong><span id="reverseFlowEventName"></span></strong>.</span>
            <button class="alt" onclick="cancelReverseFlow()" style="white-space:nowrap">Cancel</button>
          </div>
          <div id="eventPickerPanel" class="picker-panel" style="display:none">
            <div class="row" style="gap:8px;margin-bottom:8px;align-items:center;flex-wrap:wrap">
              <input type="search" id="pickerSearch" placeholder="Search events&hellip;" style="flex:1;min-width:140px" oninput="renderPickerEvents(this.value)" />
              <button id="pickerRefreshBtn" class="alt" onclick="fetchPickerEvents()">Refresh</button>
              <span id="pickerCachedBadge" class="pill info" style="display:none">cached</span>
              <button class="alt" onclick="closeEventPicker()">Close</button>
            </div>
            <div id="eventPickerList" style="max-height:320px;overflow-y:auto"></div>
          </div>
        </div>
        <div class="cfg-section" style="margin-bottom:0">
          <label class="cfg-label">Current Mappings</label>
          <div id="mappingsList"></div>
          <div class="inline-error" id="mappingError"></div>
        </div>
      </div>
    </div>

    <!-- SYSTEM -- -->
    <div id="tab-system" class="tab-panel">
      <div class="card">
        <h2>Power &amp; Sleep</h2>
        <div class="cfg-section" style="margin-bottom:0">
          <div style="font-size:0.82rem;color:var(--muted);margin-bottom:6px">Inactivity timeout before sleep, only in normal mode and battery use. Long press in normal mode sleeps immediately.</div>
          <div class="row" style="gap:8px">
            <input type="number" id="sleepTimeoutMinInput" min="0.5" step="0.5" placeholder="10" style="width:140px" />
            <span style="color:var(--muted)">minutes</span>
            <button id="sleepTimeoutBtn" onclick="saveSleepTimeout()">Save</button>
          </div>
          <div class="inline-error" id="sleepError"></div>
        </div>
      </div>
      <div class="card">
        <h2>Network Diagnostics</h2>
        <div style="font-size:0.82rem;color:var(--muted);margin-bottom:10px">Tests outbound connectivity using saved WiFi credentials. Only works in config mode.</div>
        <div class="cfg-section" style="margin-bottom:12px">
          <label class="cfg-label">STA Status</label>
          <div id="diagStaStatus" style="font-family:monospace;font-size:0.85rem;padding:6px 8px;background:#f8faf9;border:1px solid var(--line);border-radius:6px;color:var(--muted)">Not connected</div>
          <div style="margin-top:8px">
            <button id="diagConnectBtn" onclick="diagTestConnection()">Test Connection</button>
          </div>
        </div>
        <div class="cfg-section" style="margin-bottom:0">
          <label class="cfg-label">Test Fetch</label>
          <div class="row" style="gap:8px;margin-bottom:8px">
            <input type="url" id="diagFetchUrl" placeholder="http://example.com/" style="flex:1;min-width:0" />
            <button id="diagFetchBtn" onclick="diagTestFetch()">Fetch</button>
          </div>
          <div id="diagFetchResult" style="display:none;font-family:monospace;font-size:0.82rem;padding:8px;background:#f8faf9;border:1px solid var(--line);border-radius:6px;max-height:160px;overflow-y:auto;word-break:break-all;white-space:pre-wrap"></div>
        </div>
      </div>
      <!-- === STAGE 1 BATTERY HW TEST - REMOVE AFTER VALIDATION === -->
      <div class="card">
        <h2>Battery Hardware Test <span style="font-size:0.75rem;color:var(--warn);font-weight:400">(temporary)</span></h2>
        <div style="font-size:0.82rem;color:var(--muted);margin-bottom:10px">Smoke test only. Confirms voltage divider (D0/GPIO1) and LED (D5/GPIO6) wiring.</div>
        <div class="cfg-section" style="margin-bottom:12px">
          <label class="cfg-label">ADC Reading</label>
          <div class="row" style="gap:8px;align-items:center">
            <label><input type="checkbox" id="hwAdcLive" onchange="hwAdcLiveChanged()"> Live</label>
          </div>
          <div id="hwAdcResult" style="font-family:monospace;font-size:0.85rem;padding:6px 8px;background:#f8faf9;border:1px solid var(--line);border-radius:6px;color:var(--muted);margin-top:8px">--</div>
        </div>
        <div class="cfg-section" style="margin-bottom:0">
          <label class="cfg-label">LED</label>
          <div class="row" style="gap:8px;align-items:center">
            <button id="hwLedBlinkBtn" onclick="hwLedBlink()">Blink LED</button>
            <span id="hwLedStatus" style="font-size:0.85rem;color:var(--muted)"></span>
          </div>
        </div>
      </div>
      <!-- === END STAGE 1 BATTERY HW TEST === -->
      <div class="card">
        <h2>Device Control</h2>
        <div style="display:flex;gap:12px;flex-wrap:wrap;margin-bottom:10px">
          <button onclick="showModal('rebootModal')">&#x21BA; Reboot</button>
          <button class="warn" onclick="showModal('resetModal')">&#x26A0; Factory Reset</button>
        </div>
        <div style="color:var(--muted);font-size:0.85rem">
          <b>Reboot</b> restarts into run mode if config is complete.
          <b>Factory Reset</b> clears all settings &amp; unpairs the keyboard.
        </div>
      </div>
      <div class="card">
        <h2>Recent Bursts</h2>
        <div style="font-size:0.82rem;color:var(--muted);margin-bottom:6px">Live feed of the last 10 button presses seen by the device (newest at bottom).</div>
        <div id="recentBurstsList" style="font-family:monospace;font-size:0.82rem;max-height:180px;overflow-y:auto;background:#f8faf9;padding:8px;border-radius:6px;border:1px solid var(--line)"></div>
      </div>
      <div class="card">
        <h2>Log</h2>
        <div id="keys" class="log"></div>
      </div>
    </div>

  </div><!-- /.wrap -->

  <!-- -- Apply & Run confirmation modal -- -->
  <div id="applyModal" class="modal-bg" onclick="modalBackdropClick(event,'applyModal')">
    <div class="modal">
      <h3>Exit &amp; Run</h3>
      <p>This will exit configuration mode and reboot into run mode. The web UI will become unreachable. To return to configuration, hold the boot button (D10) for &ge;&nbsp;0.8&nbsp;s at power-on.</p>
      <div class="modal-error" id="applyModalError"></div>
      <div class="actions">
        <button class="alt" onclick="closeModal('applyModal')">Cancel</button>
        <button onclick="confirmApplyRun()">Exit &amp; Run</button>
      </div>
    </div>
  </div>
  <div id="rebootModal" class="modal-bg" onclick="modalBackdropClick(event,'rebootModal')">
    <div class="modal">
      <h3>Reboot</h3>
      <p>Reboot the ESP32? Configuration will be retained. The web UI will be briefly unavailable while the device restarts.</p>
      <div class="modal-error" id="rebootModalError"></div>
      <div class="actions">
        <button class="alt" onclick="closeModal('rebootModal')">Cancel</button>
        <button onclick="confirmReboot()">Reboot</button>
      </div>
    </div>
  </div>
  <div id="resetModal" class="modal-bg" onclick="modalBackdropClick(event,'resetModal')">
    <div class="modal">
      <h3>Factory Reset</h3>
      <p>Erase all bonds, mappings, and saved networks? This cannot be undone.</p>
      <div class="modal-error" id="resetModalError"></div>
      <div class="actions">
        <button class="alt" onclick="closeModal('resetModal')">Cancel</button>
        <button class="warn" onclick="confirmReset()">Erase Everything</button>
      </div>
    </div>
  </div>

  <!-- Test Mode panel (fixed bottom, slides up when active) -->
  <div id="testPanel" class="tp-compact">
    <!-- Compact view: single status bar -->
    <div id="testCompactBar">
      <span id="testDotC" class="dot dot-none"></span>
      <span style="font-weight:700">Test Mode</span>
      <span id="testStaStatusC" style="color:#a0c8b0;font-size:0.80rem;flex-shrink:0"></span>
      <span id="testCompactLast"></span>
      <button class="tp-btn" onclick="setTestExpanded(true)" title="Expand">&#x2191;</button>
      <button class="tp-btn" onclick="exitTestMode()" title="Exit Test Mode">&#x2715;</button>
    </div>
    <!-- Expanded view: header bar + fires log -->
    <div id="testExpandedView">
      <div id="testExpandedHdr">
        <div style="display:flex;align-items:center;gap:8px;margin-bottom:5px">
          <span style="font-weight:700;font-size:0.9rem;flex:1">Test Mode</span>
          <button class="tp-btn" onclick="setTestExpanded(false)" title="Collapse">&#x2193;</button>
          <button class="tp-btn" onclick="exitTestMode()" title="Exit Test Mode">&#x2715;</button>
        </div>
        <div style="color:#a0c8b0">Status: <span id="testStaStatusE"></span></div>
        <div style="color:#a0c8b0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">Target: <span id="testTargetUrlE" style="font-family:Consolas,monospace;font-size:0.78rem"></span></div>
        <hr style="border:none;border-top:1px solid rgba(255,255,255,0.15);margin:6px 0 0 0">
      </div>
      <div id="testFiresLog"></div>
    </div>
  </div>

  <script>
    // -- Tab routing --
    var TABS = ['general', 'ble', 'actions', 'system'];

    function switchTab(name) {
      if (TABS.indexOf(name) < 0) name = 'general';
      TABS.forEach(function(t) {
        document.getElementById('tab-' + t).classList.toggle('active', t === name);
        document.querySelector('.tab-btn[data-tab="' + t + '"]').classList.toggle('active', t === name);
      });
      location.hash = name;
    }

    // Restore active tab from URL hash on load.
    (function() {
      var hash = location.hash.replace('#', '');
      switchTab(TABS.indexOf(hash) >= 0 ? hash : 'general');
    })();

    // -- DOM refs & shared state --
    var elState   = document.getElementById('state');
    var elDevices = document.getElementById('devices');
    var elKeys    = document.getElementById('keys');
    var currentBondedAddress = '';
    var currentBondedName    = '';

    function status(text, type) {
      elState.textContent = text ? text.charAt(0).toUpperCase() + text.slice(1) : text;
      elState.classList.remove('state-ok', 'state-error', 'state-busy');
      if (type) elState.classList.add('state-' + type);
    }

    // -- BLE scan --
    var scanRan = false;

    async function scan() {
      scanRan = true;
      status('Scanning for 4 seconds...', 'busy');
      try {
        var r = await fetch('/scan');
        var data = await r.json();
        renderDevices(data.devices || [], currentBondedAddress);
        document.getElementById('bleDiscoveredSection').style.display = '';
        status('Scan complete.', 'ok');
      } catch (e) {
        status('Scan failed. Check device connection.', 'error');
      }
    }

    function renderDevices(devices, selectedBondedAddress) {
      elDevices.innerHTML = '';
      var sel = (selectedBondedAddress || '').toUpperCase();
      var seen = devices.filter(function(d) { return d.seen && d.address.toUpperCase() !== sel; });
      if (!seen.length) {
        var msg = scanRan
          ? 'No devices found in this scan.'
          : 'No devices visible \u2014 click Scan to search.';
        elDevices.innerHTML = '<li style="border:none;padding:0"><div class="empty-state">' + msg + '</div></li>';
        return;
      }

      function byRssi(a, b) { return b.rssi - a.rssi; }
      var isUnnamed = function(d) { return d.name === '(unnamed)' || d.name === ''; };
      var named   = seen.filter(function(d) { return !isUnnamed(d); });
      var unnamed = seen.filter(function(d) { return  isUnnamed(d); });
      var discoverableNamed    = named.filter(function(d) { return  d.pairableNow; }).sort(byRssi);
      var nonDiscoverableNamed = named.filter(function(d) { return !d.pairableNow; }).sort(byRssi);
      var otherDevices         = unnamed.sort(byRssi);

      function addSectionHeader(label, color) {
        var li = document.createElement('li');
        li.style.cssText = 'border:none;padding:8px 0 4px;background:none;display:block;margin-top:12px';
        li.innerHTML = '<span style="font-size:1.1em;font-weight:700;color:' + color + '">' + label + '</span>';
        elDevices.appendChild(li);
      }

      function makeDeviceRow(d, container) {
        var li   = document.createElement('li');
        var left = document.createElement('div');
        var bondPill = d.bonded
          ? '<span class="pill info">Bonded</span>'
          : '<span class="pill warn">Unbonded</span>';
        var discPill = isUnnamed(d)
          ? (d.pairableNow
              ? '<span class="pill ok">Discoverable</span>'
              : '<span class="pill warn">Not discoverable</span>')
          : '';
        var displayName = isUnnamed(d) ? '(no name)' : d.name;
        left.innerHTML = '<strong>' + displayName + '</strong>' + bondPill + discPill +
          '<div class="mono">' + d.address + ' | RSSI ' + d.rssi + '</div>';
        var acts = document.createElement('div');
        acts.className = 'actions';
        var btn = document.createElement('button');
        if (!d.bonded) {
          btn.textContent = 'Pair';
          btn.onclick = (function(addr, nm) { return function() { pairDevice(addr, nm); }; })(d.address, d.name);
        } else {
          btn.className = 'warn';
          btn.textContent = 'Unpair';
          btn.onclick = (function(addr, nm) { return function() { unpairDevice(addr, nm); }; })(d.address, d.name);
        }
        acts.appendChild(btn);
        li.appendChild(left);
        li.appendChild(acts);
        container.appendChild(li);
      }

      if (discoverableNamed.length) {
        addSectionHeader('&#x2714; Discoverable (' + discoverableNamed.length + ')', 'var(--ok)');
        discoverableNamed.forEach(function(d) { makeDeviceRow(d, elDevices); });
      }
      if (nonDiscoverableNamed.length) {
        addSectionHeader('&#x2298; Not Discoverable (' + nonDiscoverableNamed.length + ')', 'var(--warn)');
        nonDiscoverableNamed.forEach(function(d) { makeDeviceRow(d, elDevices); });
      }
      if (otherDevices.length) {
        var detailsLi = document.createElement('li');
        detailsLi.style.cssText = 'border:none;padding:0;background:none;display:block;margin-top:12px';
        var details = document.createElement('details');
        var summary = document.createElement('summary');
        summary.style.cssText = 'font-size:1.1em;font-weight:700;color:var(--muted);cursor:pointer;user-select:none;list-style:none;display:flex;align-items:center;gap:6px';
        summary.innerHTML = '&#9656; Other Devices (' + otherDevices.length + ')';
        details.appendChild(summary);
        details.addEventListener('toggle', function() {
          summary.innerHTML = (details.open ? '&#9662;' : '&#9656;') + ' Other Devices (' + otherDevices.length + ')';
        });
        var innerUl = document.createElement('ul');
        innerUl.style.cssText = 'list-style:none;padding:0;margin:4px 0 0 0';
        otherDevices.forEach(function(d) { makeDeviceRow(d, innerUl); });
        details.appendChild(innerUl);
        detailsLi.appendChild(details);
        elDevices.appendChild(detailsLi);
      }
    }

    // -- Bonded device panel (BLE tab) --
    function renderBondedPanel(s) {
      var card = document.getElementById('bondedCard');
      var info = document.getElementById('bondedInfo');
      if (!s.bondedAddress) {
        card.style.display = '';
        info.innerHTML = '<div class="empty-state">No device bonded \u2014 start a scan above to find one.</div>';
        return;
      }
      card.style.display = '';
      var name = s.bondedName || s.bondedAddress;
      var connBadge = s.connected
        ? '<span class="pill ok">Connected</span>'
        : '<span class="pill warn">Disconnected</span>';
      info.innerHTML =
        '<div style="display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap">' +
          '<div><strong>' + name + '</strong>' + connBadge +
            '<div class="mono">' + s.bondedAddress + '</div></div>' +
          '<div class="actions" id="bondedActions"></div>' +
        '</div>';
      var acts = document.getElementById('bondedActions');
      if (s.connected) {
        var btnD = document.createElement('button');
        btnD.className = 'alt';
        btnD.textContent = 'Disconnect';
        btnD.onclick = disconnectNow;
        acts.appendChild(btnD);
      } else {
        var btnC = document.createElement('button');
        btnC.textContent = 'Connect';
        btnC.onclick = (function(addr, nm) { return function() { connectDevice(addr, nm); }; })(s.bondedAddress, s.bondedName);
        acts.appendChild(btnC);
      }
      var btnU = document.createElement('button');
      btnU.className = 'warn';
      btnU.textContent = 'Unpair';
      btnU.onclick = (function(addr, nm) { return function() { unpairDevice(addr, nm); }; })(s.bondedAddress, s.bondedName);
      acts.appendChild(btnU);
    }

    // -- Header update --
    var gConnectFlashing = false;

    function updateHeader(s) {
      var dot    = document.getElementById('statusDot');
      var nameEl = document.getElementById('bondedName');
      var btn    = document.getElementById('connectBtn');
      if (!s.bondedAddress) {
        dot.className    = 'dot dot-none';
        nameEl.textContent = 'No device bonded';
        if (!gConnectFlashing) btn.style.display  = 'none';
        return;
      }
      nameEl.textContent = s.bondedName || s.bondedAddress;
      if (s.connected) {
        dot.className  = 'dot dot-connected';
        if (!gConnectFlashing) btn.style.display = 'none';
      } else {
        dot.className  = 'dot dot-bonded';
        if (!gConnectFlashing) btn.style.display = '';
      }
    }

    async function connectBonded() {
      if (!currentBondedAddress) return;
      var btn = document.getElementById('connectBtn');
      gConnectFlashing = true;
      btn.textContent = 'Connecting--';
      btn.classList.add('btn-flash-busy');
      btn.style.display = '';
      btn.disabled = true;
      var r = await fetch('/connect?addr=' + encodeURIComponent(currentBondedAddress) + '&name=' + encodeURIComponent(currentBondedName));
      var data = await r.json();
      btn.classList.remove('btn-flash-busy');
      if (data.ok) {
        btn.textContent = 'Connected';
        btn.style.display = '';
        btn.classList.add('btn-flash-saved');
        setTimeout(function() {
          btn.textContent = 'Connect';
          btn.classList.remove('btn-flash-saved');
          btn.disabled = false;
          gConnectFlashing = false;
          refreshState();
        }, 2000);
      } else {
        btn.textContent = 'Failed';
        btn.style.display = '';
        btn.classList.add('btn-flash-error');
        setTimeout(function() {
          btn.textContent = 'Connect';
          btn.classList.remove('btn-flash-error');
          btn.disabled = false;
          gConnectFlashing = false;
        }, 2000);
      }
    }

    // -- BLE device actions --
    async function pairDevice(address, name) {
      document.getElementById('bleDiscoveredSection').style.display = 'none';
      status('Pairing with ' + address + ' ...', 'busy');
      var r = await fetch('/pair?addr=' + encodeURIComponent(address) + '&name=' + encodeURIComponent(name));
      var data = await r.json();
      if (data.ok) {
        status('Paired with ' + name + '.', 'ok');
        await refreshState();
      } else {
        document.getElementById('bleDiscoveredSection').style.display = '';
        status(data.error || 'Pair failed', 'error');
      }
    }

    async function connectDevice(address, name) {
      status('Connecting to ' + address + ' ...', 'busy');
      var r = await fetch('/connect?addr=' + encodeURIComponent(address) + '&name=' + encodeURIComponent(name));
      var data = await r.json();
      if (data.ok) {
        status('Connected to ' + name, 'ok');
      } else {
        status(data.error || 'Connect failed', 'error');
      }
    }

    async function unpairDevice(address, name) {
      status('Unpairing ' + address + ' ...', 'busy');
      var r = await fetch('/unpair?addr=' + encodeURIComponent(address));
      var data = await r.json();
      if (data.ok) {
        status('Unpaired ' + (name || address), 'ok');
        await scan();
      } else {
        status(data.error || 'Unpair failed', 'error');
      }
    }

    async function disconnectNow() {
      await fetch('/disconnect');
      status('Disconnected');
    }

    // -- Sleep timeout --
    var currentSleepTimeoutMs = 10 * 60 * 1000;

    function renderSleepTimeout(timeoutMs) {
      var ms = Number(timeoutMs);
      if (!Number.isFinite(ms) || ms <= 0) return;
      currentSleepTimeoutMs = Math.round(ms);
      document.getElementById('sleepTimeoutMinInput').value = (currentSleepTimeoutMs / 60000).toFixed(1);
    }

    async function saveSleepTimeout() {
      var raw = document.getElementById('sleepTimeoutMinInput').value;
      var minutes = parseFloat(raw);
      if (!Number.isFinite(minutes) || minutes <= 0) { showInlineError('sleepError', 'Enter a valid timeout in minutes.'); return; }
      clearInlineError('sleepError');
      var ms = Math.round(minutes * 60000);
      var btn = document.getElementById('sleepTimeoutBtn');
      btn.disabled = true;
      try {
        var r = await fetch('/config/setsleeptimeout?ms=' + encodeURIComponent(ms));
        var data = await r.json();
        if (!data.ok) { showInlineError('sleepError', data.error || 'Save failed.'); btn.disabled = false; return; }
        renderSleepTimeout(data.sleepTimeoutMs || ms);
        flashSaved(btn, 'Save');
      } catch (e) {
        showInlineError('sleepError', 'Save failed. Check connection.');
        btn.disabled = false;
      }
    }

    // -- WiFi networks --
    var wifiNets = [];

    function renderWifiNetworks(nets) {
      wifiNets = nets;
      var el = document.getElementById('wifiNetworksList');
      if (!nets.length) {
        el.innerHTML = '<div class="empty-state">No networks saved \u2014 fill in the SSID and password above to add one.</div>';
        return;
      }
      el.innerHTML = '<ul>' + nets.map(function(n, i) {
        return '<li><div style="min-width:0;flex:1"><strong>' + n.ssid + '</strong></div>' +
               '<div class="actions"><button class="warn" onclick="deleteWifi(' + i + ')">Remove</button></div></li>';
      }).join('') + '</ul>';
    }

    async function addWifi() {
      var ssid = document.getElementById('wifiSsidInput').value.trim();
      var pwd  = document.getElementById('wifiPwdInput').value;
      if (!ssid) { showInlineError('wifiError', 'Enter an SSID first.'); return; }
      clearInlineError('wifiError');
      try {
        var r = await fetch('/config/addwifi?ssid=' + encodeURIComponent(ssid) + '&pwd=' + encodeURIComponent(pwd));
        var data = await r.json();
        if (!data.ok) { showInlineError('wifiError', data.error || 'Save failed.'); return; }
        document.getElementById('wifiSsidInput').value = '';
        document.getElementById('wifiPwdInput').value  = '';
        await loadConfig();
      } catch (e) {
        showInlineError('wifiError', 'Save failed. Check connection.');
      }
    }

    async function deleteWifi(idx) {
      var net = wifiNets[idx];
      if (!net) return;
      clearInlineError('wifiError');
      try {
        var r = await fetch('/config/delwifi?ssid=' + encodeURIComponent(net.ssid));
        var data = await r.json();
        if (!data.ok) { showInlineError('wifiError', data.error || 'Remove failed.'); return; }
        await loadConfig();
      } catch (e) {
        showInlineError('wifiError', 'Remove failed. Check connection.');
      }
    }

    // -- WiFi network scan --------------------------------------------------
    var wifiScanNets = [];

    async function scanWifi() {
      var btn = document.getElementById('wifiScanBtn');
      btn.textContent = 'Scanning\u2026';
      btn.disabled = true;
      clearInlineError('wifiError');
      try {
        var r = await fetch('/wifi/scan');
        var data = await r.json();
        renderWifiScan(Array.isArray(data) ? data : []);
      } catch (e) {
        showInlineError('wifiError', 'Scan failed. Check connection.');
        document.getElementById('wifiScanSection').style.display = 'none';
      } finally {
        btn.textContent = 'Scan Networks';
        btn.disabled = false;
      }
    }

    function wifiSignalBars(rssi) {
      if (rssi >= -55) return '\u2582\u2584\u2586\u2588';  // \u25a1 = empty bar
      if (rssi >= -65) return '\u2582\u2584\u2586\u25a1';
      if (rssi >= -75) return '\u2582\u2584\u25a1\u25a1';
      return '\u2582\u25a1\u25a1\u25a1';
    }

    function renderWifiScan(nets) {
      wifiScanNets = nets;
      var el = document.getElementById('wifiScanResults');
      var section = document.getElementById('wifiScanSection');
      section.style.display = 'block';
      if (!nets.length) {
        el.innerHTML = '<div class="empty-state">No networks found \u2014 try again or enter SSID manually.</div>';
        return;
      }
      el.innerHTML = '<ul>' + nets.map(function(n, i) {
        var lock = n.secure ? '<span class="pill info">Secured</span>' : '<span class="pill ok">Open</span>';
        var bars = '<span class="mono">' + wifiSignalBars(n.rssi) + ' ' + n.rssi + '\u202FdBm</span>';
        return '<li onclick="selectWifiNetwork(' + i + ')" style="cursor:pointer">' +
               '<div style="min-width:0;flex:1"><strong>' + n.ssid + '</strong>&nbsp;' + lock +
               '<div>' + bars + '</div></div>' +
               '<div class="actions"><button onclick="event.stopPropagation();selectWifiNetwork(' + i + ')">Use</button></div></li>';
      }).join('') + '</ul>';
    }

    function selectWifiNetwork(idx) {
      var net = wifiScanNets[idx];
      if (!net) return;
      document.getElementById('wifiSsidInput').value = net.ssid;
      document.getElementById('wifiPwdInput').focus();
      document.getElementById('wifiScanSection').style.display = 'none';
    }

    // -- Base URLs --
    var baseUrlsList = [];
    var urlEditIndex = -1;

    function renderBaseUrls(urls, selectedIdx) {
      baseUrlsList = urls;
      selectedUrlIndex = selectedIdx || 0;
      var el = document.getElementById('baseUrlsList');
      if (!urls.length) {
        el.innerHTML = '<div class="empty-state">No URLs configured \u2014 enter a URL above to add one.</div>';
        return;
      }
      el.innerHTML = '<ul>' + urls.map(function(u, i) {
        var isActive = (i === selectedIdx);
        var multi = urls.length > 1;
        var badge = isActive ? '<span class="pill ok">Active</span>' : '';
        var activateBtn = multi && !isActive ? '<button onclick="activateUrl(' + i + ')">Activate</button>' : '';
        return '<li><div style="min-width:0;flex:1"><span class="mono">' + u + '</span>' +
               (badge ? '&nbsp;' + badge : '') + '</div>' +
               '<div class="actions">' + activateBtn +
               '<button class="alt" onclick="beginEditUrl(' + i + ')">Edit</button>' +
               '<button class="warn" onclick="deleteUrl(' + i + ')">Remove</button>' +
               '</div></li>';
      }).join('') + '</ul>';
    }

    async function activateUrl(idx) {
      clearInlineError('urlError');
      try {
        var r = await fetch('/config/selecturl?idx=' + idx);
        var data = await r.json();
        if (!data.ok) { showInlineError('urlError', data.error || 'Failed to activate URL.'); return; }
        koreaderEvents = null;
        console.log('[events] cache: cleared (base URL changed)');
        await loadConfig();
      } catch (e) {
        showInlineError('urlError', 'Failed to activate URL. Check connection.');
      }
    }

    async function addUrl() {
      var url = document.getElementById('newUrlInput').value.trim();
      if (!url) { showInlineError('urlError', 'Enter a URL first.'); return; }
      clearInlineError('urlError');
      try {
        var r;
        if (urlEditIndex >= 0) {
          r = await fetch('/config/editurl?idx=' + urlEditIndex + '&url=' + encodeURIComponent(url));
        } else {
          r = await fetch('/config/addurl?url=' + encodeURIComponent(url));
        }
        var data = await r.json();
        if (!data.ok) { showInlineError('urlError', data.error || 'Save failed.'); return; }
        if (urlEditIndex >= 0) {
          koreaderEvents = null;
          console.log('[events] cache: cleared (base URL changed)');
        }
        cancelUrlEdit();
        await loadConfig();
      } catch (e) {
        showInlineError('urlError', 'Save failed. Check connection.');
      }
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
      document.getElementById('urlActionBtn').textContent = 'Add';
      document.getElementById('urlCancelBtn').style.display = 'none';
    }

    async function deleteUrl(idx) {
      clearInlineError('urlError');
      try {
        var r = await fetch('/config/delurl?idx=' + idx);
        var data = await r.json();
        if (!data.ok) { showInlineError('urlError', data.error || 'Remove failed.'); return; }
        if (urlEditIndex === idx) cancelUrlEdit();
        else if (urlEditIndex > idx) urlEditIndex--;
        await loadConfig();
      } catch (e) {
        showInlineError('urlError', 'Remove failed. Check connection.');
      }
    }

    // -- Button mappings --
    var capturing = false;
    var capturedKeyHex = '';
    var lastSeenKey = '';
    var lastSeenBurstSeq = 0;
    var mappingEditOriginalKey = '';

    function renderMappings(mappings) {
      currentMappings = mappings;
      var el = document.getElementById('mappingsList');
      if (!mappings.length) {
        el.innerHTML = '<div class="empty-state">No mappings yet \u2014 connect a BLE device, press a button, then assign a URL to the captured signature.</div>';
        return;
      }
      el.innerHTML = '<ul>' + mappings.map(function(m) {
        var primary = m.label
          ? '<strong>' + m.label + '</strong>'
          : '<span class="mono">' + m.sig + '</span>';
        var secondary = m.label
          ? '<div class="mono" style="color:var(--muted);font-size:0.82rem;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">' + m.url + '</div>' +
            '<div class="mono" style="color:var(--muted);font-size:0.78rem">' + m.sig + '</div>'
          : '<div class="mono" style="color:var(--muted);font-size:0.82rem;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">' + m.url + '</div>';
        return '<li><div style="min-width:0;flex:1">' + primary + secondary + '</div>' +
          '<div class="actions">' +
          '<button class="alt" onclick="beginEditMapping(\'' + m.sig + '\',\'' + encodeURIComponent(m.url) + '\',\'' + encodeURIComponent(m.label || '') + '\')">' + 'Edit</button>' +
          '<button class="warn" onclick="deleteMapping(\'' + m.sig + '\')">' + 'Delete</button>' +
          '</div></li>';
      }).join('') + '</ul>';
    }

    function startCapture() {
      capturing = true;
      capturedKeyHex = '';
      document.getElementById('capturedKey').textContent = '...';
      document.getElementById('assignBtn').disabled = true;
      document.getElementById('captureBtn').textContent = 'Waiting...';
      document.getElementById('captureBtn').disabled = true;
    }

    function checkCapture(lastSig) {
      if (!lastSig) return;
      if (reverseFlowPendingEvent) {
        var ev = reverseFlowPendingEvent;
        reverseFlowPendingEvent = null;
        hideReverseFlowStatus();
        capturing = false;
        capturedKeyHex = lastSig;
        document.getElementById('capturedKey').textContent = lastSig;
        document.getElementById('mappingUrl').value = ev.path;
        document.getElementById('mappingLabel').value = ev.label || '';
        document.getElementById('assignBtn').disabled = false;
        saveMapping();
        return;
      }
      if (!capturing) return;
      capturing = false;
      capturedKeyHex = lastSig;
      document.getElementById('capturedKey').textContent = lastSig;
      document.getElementById('assignBtn').disabled = false;
      document.getElementById('captureBtn').textContent = 'Capture Key';
      document.getElementById('captureBtn').disabled = false;
    }

    function beginEditMapping(sig, encodedUrl, encodedLabel) {
      capturing = false;
      capturedKeyHex = sig;
      mappingEditOriginalKey = sig;
      document.getElementById('capturedKey').textContent = sig;
      document.getElementById('mappingUrl').value = decodeURIComponent(encodedUrl);
      document.getElementById('mappingLabel').value = decodeURIComponent(encodedLabel || '');
      document.getElementById('assignBtn').disabled = false;
      document.getElementById('assignBtn').textContent = 'Update';
      document.getElementById('mappingCancelBtn').style.display = '';
      document.getElementById('captureBtn').textContent = 'Capture Key';
      document.getElementById('captureBtn').disabled = false;
      status('Editing mapping ' + sig);
    }

    function cancelMappingEdit() {
      capturing = false;
      capturedKeyHex = '';
      mappingEditOriginalKey = '';
      document.getElementById('capturedKey').innerHTML = '&mdash;';
      document.getElementById('mappingUrl').value = '';
      document.getElementById('mappingLabel').value = '';
      document.getElementById('assignBtn').disabled = true;
      document.getElementById('assignBtn').textContent = 'Assign';
      document.getElementById('mappingCancelBtn').style.display = 'none';
      document.getElementById('captureBtn').textContent = 'Capture Key';
      document.getElementById('captureBtn').disabled = false;
    }

    async function saveMapping() {
      if (!capturedKeyHex) return;
      var url   = document.getElementById('mappingUrl').value.trim();
      var label = document.getElementById('mappingLabel').value.trim();
      if (!url) { showInlineError('mappingError', 'Enter a URL path first.'); return; }
      clearInlineError('mappingError');
      try {
        if (mappingEditOriginalKey && mappingEditOriginalKey !== capturedKeyHex) {
          await fetch('/config/delmapping?sig=' + encodeURIComponent(mappingEditOriginalKey));
        }
        var r = await fetch('/config/setmapping?sig=' + encodeURIComponent(capturedKeyHex) +
                    '&url=' + encodeURIComponent(url) + '&label=' + encodeURIComponent(label));
        var data = await r.json();
        if (!data.ok) { showInlineError('mappingError', data.error || 'Save failed.'); return; }
        cancelMappingEdit();
        await loadConfig();
      } catch (e) {
        showInlineError('mappingError', 'Save failed. Check connection.');
      }
    }

    async function deleteMapping(sig) {
      clearInlineError('mappingError');
      try {
        var r = await fetch('/config/delmapping?sig=' + encodeURIComponent(sig));
        var data = await r.json();
        if (!data.ok) { showInlineError('mappingError', data.error || 'Remove failed.'); return; }
        if (mappingEditOriginalKey === sig) cancelMappingEdit();
        await loadConfig();
      } catch (e) {
        showInlineError('mappingError', 'Remove failed. Check connection.');
      }
    }

    // -- Load all config --
    // -- KOReader Event Picker ----------------------------------------------
    var koreaderEvents = null;
    var pickerMode = null;
    var reverseFlowPendingEvent = null;
    var currentBleConnected = false;

    // -- Test Mode state ---------------------------------------------------
    var testModeActive    = false;
    var testPanelExpanded = true;   // resets to expanded on each entry
    var selectedUrlIndex  = 0;      // kept in sync by renderBaseUrls
    var currentMappings   = [];     // kept in sync by renderMappings
    var testFiresLog      = [];     // last 20 fires, newest first

    function openEventPicker(mode) {
      if (testModeActive) {
        alert('Exit Test Mode before scanning events.');
        return;
      }
      if (mode === 'reverse' && !currentBleConnected) {
        showInlineError('mappingError', 'Connect a BLE device first.');
        return;
      }
      pickerMode = mode;
      document.getElementById('eventPickerPanel').style.display = '';
      if (koreaderEvents && koreaderEvents.length) {
        console.log('[events] cache: hit (' + koreaderEvents.length + ' events)');
        document.getElementById('pickerCachedBadge').style.display = '';
        renderPickerEvents('');
      } else {
        console.log('[events] cache: miss, fetching');
        fetchPickerEvents();
      }
    }

    function closeEventPicker() {
      document.getElementById('eventPickerPanel').style.display = 'none';
      var s = document.getElementById('pickerSearch');
      if (s) s.value = '';
      pickerMode = null;
    }

    async function fetchPickerEvents() {
      if (testModeActive) {
        alert('Exit Test Mode before refreshing events.');
        return;
      }
      var refreshBtn = document.getElementById('pickerRefreshBtn');
      var list = document.getElementById('eventPickerList');
      if (koreaderEvents) {
        console.log('[events] cache: refresh requested');
        koreaderEvents = null;
      }
      refreshBtn.disabled = true;
      list.textContent = 'Fetching events from KOReader\u2026';
      document.getElementById('pickerCachedBadge').style.display = 'none';
      try {
        var r = await fetch('/koreader/events_page');
        if (!r.ok) {
          var errData = await r.json().catch(function() { return {}; });
          list.textContent = 'Error: ' + (errData.error || ('HTTP ' + r.status));
          refreshBtn.disabled = false;
          return;
        }
        var html = await r.text();
        var events = parseKoreaderEvents(html);
        console.log('[events] parsed ' + events.length + ' events from ' + html.length + 'B HTML');
        if (events.length) {
          koreaderEvents = events;
        }
        renderPickerEvents(document.getElementById('pickerSearch').value || '');
      } catch(e) {
        list.textContent = 'Fetch error: ' + e.message;
      }
      refreshBtn.disabled = false;
    }

    function parseKoreaderEvents(html) {
      const doc = new DOMParser().parseFromString(html, 'text/html');
      const root = doc.querySelector('pre') || doc.body;

      const events = [];
      let section = '';
      let label = '';
      let contextual = false;
      let lastWasA = false;
      let lastAContextual = false;

      for (const node of root.childNodes) {
        if (node.nodeType === Node.TEXT_NODE) {
          if (node.textContent.includes('\n')) lastWasA = false;
          continue;
        }
        if (node.nodeType !== Node.ELEMENT_NODE) continue;

        const tag = node.tagName.toLowerCase();

        if (tag === 'big') {
          section = node.textContent.trim();
          lastWasA = false;
          continue;
        }

        if (tag === 'b') {
          if (lastWasA && !lastAContextual && events.length > 0) {
            events[events.length - 1].modifier = node.textContent.trim();
          } else if (!lastWasA) {
            contextual = /color:\s*dimgray/i.test(node.innerHTML || '');
            label = node.textContent.replace(/\s+/g, ' ').trim();
          }
          lastWasA = false;
          continue;
        }

        if (tag === 'a') {
          const href = node.getAttribute('href') || '';
          if (href.includes('/koreader/event/')) {
            if (!contextual) {
              const fullPath = (href.match(/\/koreader\/event\/.*$/) || [href])[0];
              const path = fullPath.replace(/^\/koreader\/event\//, '');
              events.push({ section, label, modifier: '', path });
              lastAContextual = false;
            } else {
              lastAContextual = true;
            }
            lastWasA = true;
          } else {
            lastWasA = false;
          }
          continue;
        }

        lastWasA = false;
      }

      return events;
    }

    function renderPickerEvents(query) {
      var list = document.getElementById('eventPickerList');
      if (!koreaderEvents || !koreaderEvents.length) {
        list.innerHTML = '<div style="padding:12px;color:var(--muted);font-size:0.9rem">No events found.</div>';
        return;
      }
      var q = (query || '').toLowerCase().trim();
      var html = '';
      if (q) {
        var filtered = koreaderEvents.filter(function(ev) {
          return (ev.label + ' ' + ev.modifier + ' ' + ev.section + ' ' + ev.path).toLowerCase().indexOf(q) >= 0;
        });
        if (!filtered.length) {
          list.innerHTML = '<div style="padding:12px;color:var(--muted);font-size:0.9rem">No matches.</div>';
          return;
        }
        html = filtered.map(function(ev) {
          var modPart = ev.modifier ? ' \u2014 ' + ev.modifier : '';
          return '<div class="picker-event-row" onclick="pickerSelectEvent(\'' +
            encodeURIComponent(ev.path) + '\',\'' + encodeURIComponent(ev.label) + '\',\'' + encodeURIComponent(ev.modifier) + '\')">'+
            '<div><strong>' + htmlEsc(ev.label) + htmlEsc(modPart) + '</strong>' +
            ' <span style="font-size:0.75rem;color:var(--muted)">[' + htmlEsc(ev.section) + ']</span></div>' +
            '<div class="mono" style="font-size:0.78rem;color:var(--muted)">' + htmlEsc(ev.path) + '</div>' +
            '</div>';
        }).join('');
      } else {
        var sections = [];
        var sectionMap = {};
        koreaderEvents.forEach(function(ev) {
          if (!sectionMap[ev.section]) { sectionMap[ev.section] = []; sections.push(ev.section); }
          sectionMap[ev.section].push(ev);
        });
        var defaultExpanded = ['General', 'Device'];
        html = sections.map(function(sec) {
          var evs = sectionMap[sec];
          var expanded = defaultExpanded.indexOf(sec) >= 0;
          var bodyId = 'pks_' + sec.replace(/\W+/g, '_');
          var rows = evs.map(function(ev) {
            var modPart = ev.modifier ? ' \u2014 ' + ev.modifier : '';
            return '<div class="picker-event-row" onclick="pickerSelectEvent(\'' +
              encodeURIComponent(ev.path) + '\',\'' + encodeURIComponent(ev.label) + '\',\'' + encodeURIComponent(ev.modifier) + '\')">'+
              '<div><strong>' + htmlEsc(ev.label) + htmlEsc(modPart) + '</strong></div>' +
              '<div class="mono" style="font-size:0.78rem;color:var(--muted)">' + htmlEsc(ev.path) + '</div>' +
              '</div>';
          }).join('');
          return '<div class="picker-section-hdr" onclick="togglePickerSection(\'' + bodyId + '\')">'+
            '<span>' + htmlEsc(sec) + ' <span style="color:var(--muted);font-weight:500;font-size:0.82rem">(' + evs.length + ')</span></span>' +
            '<span id="' + bodyId + '-arrow">' + (expanded ? '\u25BE' : '\u25B8') + '</span>' +
            '</div>' +
            '<div id="' + bodyId + '" style="display:' + (expanded ? 'block' : 'none') + '">' + rows + '</div>';
        }).join('');
      }
      list.innerHTML = html;
    }

    function togglePickerSection(bodyId) {
      var el = document.getElementById(bodyId);
      var arrow = document.getElementById(bodyId + '-arrow');
      if (!el) return;
      var open = el.style.display !== 'none';
      el.style.display = open ? 'none' : 'block';
      if (arrow) arrow.textContent = open ? '\u25B8' : '\u25BE';
    }

    function pickerSelectEvent(encodedPath, encodedLabel, encodedModifier) {
      var path = decodeURIComponent(encodedPath);
      var label = decodeURIComponent(encodedLabel);
      var modifier = decodeURIComponent(encodedModifier);
      var displayLabel = label + (modifier ? ' \u2014 ' + modifier : '');
      if (pickerMode === 'reverse') {
        reverseFlowPendingEvent = { path: path, label: displayLabel };
        closeEventPicker();
        showReverseFlowStatus(displayLabel);
      }
    }

    function showReverseFlowStatus(eventLabel) {
      document.getElementById('reverseFlowEventName').textContent = eventLabel;
      document.getElementById('reverseFlowStatus').style.display = '';
    }

    function hideReverseFlowStatus() {
      document.getElementById('reverseFlowStatus').style.display = 'none';
    }

    function cancelReverseFlow() {
      reverseFlowPendingEvent = null;
      hideReverseFlowStatus();
    }

    function htmlEsc(s) {
      return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
    }

    async function loadConfig() {
      var r = await fetch('/config');
      var c = await r.json();
      renderWifiNetworks(c.wifiNetworks || []);
      renderBaseUrls(c.baseUrls || [], c.selectedUrlIndex || 0);
      renderSleepTimeout(c.sleepTimeoutMs || (10 * 60 * 1000));
      renderMappings(c.mappings || []);
    }

    // -- System: reboot / factory reset --
    async function confirmReboot() {
      try {
        var r = await fetch('/reboot');
        var data = await r.json();
        if (!data.ok) { showModalError('rebootModal', data.error || 'Reboot failed.'); return; }
        closeModal('rebootModal');
        status('Rebooting...');
      } catch (e) {
        showModalError('rebootModal', 'Reboot failed. Check connection.');
      }
    }

    async function confirmReset() {
      try {
        var r = await fetch('/factory-reset');
        var data = await r.json();
        if (!data.ok) { showModalError('resetModal', data.error || 'Factory reset failed.'); return; }
        closeModal('resetModal');
        status('Erasing everything and rebooting...');
      } catch (e) {
        showModalError('resetModal', 'Factory reset failed. Check connection.');
      }
    }

    // -- Network Diagnostics (System tab) ------------------------------------
    // diagTestConnection: briefly enters APSTA + connects STA, auto-disconnects
    // after 4 s so the user can see the connected state before it cleans up.
    async function diagTestConnection() {
      var btn    = document.getElementById('diagConnectBtn');
      var stEl   = document.getElementById('diagStaStatus');
      btn.disabled = true;
      stEl.style.color = 'var(--muted)';
      stEl.textContent = 'Connecting\u2026';
      try {
        var r    = await fetch('/diag/sta/connect');
        var data = await r.json();
        if (data.ok) {
          stEl.style.color = 'var(--ok)';
          stEl.textContent = 'Connected to ' + data.ssid + ', IP ' + data.ip;
          // Auto-disconnect after 4 s so we don't leave APSTA mode running.
          // Skip if test mode was entered in the meantime -- it owns the STA.
          setTimeout(async function() {
            if (testModeActive) { return; }
            try { await fetch('/diag/sta/disconnect'); } catch (_) {}
            stEl.style.color = 'var(--muted)';
            stEl.textContent = 'Not connected';
            btn.disabled = false;
          }, 4000);
        } else {
          stEl.style.color = 'var(--warn)';
          stEl.textContent = 'Connection failed: ' + (data.error || 'unknown error');
          btn.disabled = false;
        }
      } catch (e) {
        stEl.style.color = 'var(--warn)';
        stEl.textContent = 'Request error: ' + e.message;
        btn.disabled = false;
      }
    }

    // diagTestFetch: enter APSTA + HTTP GET + exit APSTA, display result inline.
    async function diagTestFetch() {
      var urlInput = document.getElementById('diagFetchUrl');
      var fetchBtn = document.getElementById('diagFetchBtn');
      var result   = document.getElementById('diagFetchResult');
      var url = (urlInput.value || '').trim();
      if (!url) { url = 'http://example.com/'; urlInput.value = url; }
      fetchBtn.disabled = true;
      result.style.display = '';
      result.style.color = 'var(--muted)';
      result.textContent = 'Fetching ' + url + '\u2026';
      var t0 = Date.now();
      try {
        var r    = await fetch('/diag/fetch?url=' + encodeURIComponent(url));
        var data = await r.json();
        var ms   = Date.now() - t0;
        if (data.ok) {
          var body = data.body || '';
          var trailer = data.truncated ? '\n\u2026 [truncated]' : '';
          if (body.length > 500) { body = body.substring(0, 500); trailer = '\n\u2026 [truncated]'; }
          result.style.color = 'var(--ink)';
          result.textContent = 'HTTP ' + data.status + ' \u2014 ' + ms + ' ms\n' + body + trailer;
        } else {
          result.style.color = 'var(--warn)';
          result.textContent = 'Error (' + ms + ' ms): ' + (data.error || 'unknown');
        }
      } catch (e) {
        result.style.color = 'var(--warn)';
        result.textContent = 'Request error: ' + e.message;
      }
      fetchBtn.disabled = false;
    }
    // === STAGE 1 BATTERY HW TEST - REMOVE AFTER VALIDATION ===
    var hwAdcTimer = null;
    function hwAdcLiveChanged() {
      var cb = document.getElementById('hwAdcLive');
      if (cb.checked) {
        hwAdcPoll();
        hwAdcTimer = setInterval(hwAdcPoll, 1000);
      } else {
        if (hwAdcTimer) { clearInterval(hwAdcTimer); hwAdcTimer = null; }
      }
    }
    async function hwAdcPoll() {
      var el = document.getElementById('hwAdcResult');
      try {
        var r = await fetch('/check/adc');
        var d = await r.json();
        el.style.color = 'var(--ink)';
        el.textContent = 'raw: ' + d.raw + '  \u2192  ' + d.millivolts + ' mV';
      } catch (e) {
        el.style.color = 'var(--warn)';
        el.textContent = 'Error: ' + e.message;
      }
    }
    async function hwLedBlink() {
      var btn = document.getElementById('hwLedBlinkBtn');
      var st  = document.getElementById('hwLedStatus');
      btn.disabled = true;
      st.style.color = 'var(--muted)';
      st.textContent = 'Blinking\u2026';
      try {
        var r = await fetch('/check/led-blink');
        var d = await r.json();
        st.style.color = d.ok ? 'var(--ok)' : 'var(--warn)';
        st.textContent = d.ok ? 'Done' : 'Error';
      } catch (e) {
        st.style.color = 'var(--warn)';
        st.textContent = 'Error: ' + e.message;
      }
      btn.disabled = false;
    }
    // === END STAGE 1 BATTERY HW TEST ===


    // -- Apply & Run modal --
    // -- Modal helpers
    function showModal(id) {
      document.getElementById(id).classList.add('open');
      var errEl = document.getElementById(id + 'Error');
      if (errEl) { errEl.style.display = 'none'; errEl.textContent = ''; }
      var cancelBtn = document.querySelector('#' + id + ' button.alt');
      if (cancelBtn) cancelBtn.textContent = 'Cancel';
    }
    function closeModal(id) { document.getElementById(id).classList.remove('open'); }
    function modalBackdropClick(e, id) { if (e.target === e.currentTarget) closeModal(id); }
    document.addEventListener('keydown', function(e) {
      if (e.key === 'Escape') { ['applyModal','rebootModal','resetModal'].forEach(closeModal); closeEventPicker(); }
    });
    function openApplyModal() { showModal('applyModal'); }
    function closeApplyModal() { closeModal('applyModal'); }

    // -- Inline / modal error helpers
    function showInlineError(id, msg) {
      var el = document.getElementById(id);
      if (!el) return;
      el.textContent = msg;
      el.style.display = '';
      clearTimeout(el._t);
      el._t = setTimeout(function() { el.style.display = 'none'; }, 10000);
    }
    function clearInlineError(id) {
      var el = document.getElementById(id);
      if (el) { el.style.display = 'none'; el.textContent = ''; clearTimeout(el._t); }
    }
    function showModalError(modalId, msg) {
      var err = document.getElementById(modalId + 'Error');
      if (!err) return;
      err.textContent = msg;
      err.style.display = '';
      var cancelBtn = document.querySelector('#' + modalId + ' button.alt');
      if (cancelBtn) cancelBtn.textContent = 'Close';
    }

    // -- Button flash (Save Timeout only)
    function flashSaved(btn, origText) {
      btn.textContent = 'Saved!';
      btn.classList.add('btn-flash-saved');
      btn.disabled = true;
      setTimeout(function() {
        btn.textContent = origText || 'Save';
        btn.classList.remove('btn-flash-saved');
        btn.disabled = false;
      }, 1500);
    }

    async function confirmApplyRun() {
      try {
        var r = await fetch('/reboot');
        var data = await r.json();
        if (!data.ok) { showModalError('applyModal', data.error || 'Apply failed.'); return; }
        closeModal('applyModal');
        status('Applying config and rebooting to run mode...');
      } catch (e) {
        showModalError('applyModal', 'Apply failed. Check connection.');
      }
    }

    // -- Poll: refreshState --
    async function refreshState() {
      var r = await fetch('/state');
      var s = await r.json();
      currentBondedAddress = s.bondedAddress || '';
      currentBondedName    = s.bondedName    || '';
      currentBleConnected  = !!s.connected;

      updateHeader(s);
      renderBondedPanel(s);

      elKeys.innerHTML = (s.keys || []).map(function(k) { return '<div>' + k + '</div>'; }).join('');
      elKeys.scrollTop = elKeys.scrollHeight;

      if (s.lastSig && s.burstSeq !== lastSeenBurstSeq) {
        checkCapture(s.lastSig);
        if (testModeActive) { testFireSig(s.lastSig); } // fire-and-forget
        lastSeenKey = s.lastSig;
        lastSeenBurstSeq = s.burstSeq;
      }

      var rb = document.getElementById('recentBurstsList');
      if (rb && s.recentSigs) {
        rb.innerHTML = s.recentSigs.map(function(e) {
          return '<div>' + e.sig +
                 ' &nbsp;<span style="color:var(--muted)">' + (e.dev || '') + '</span>' +
                 ' &nbsp;<span style="color:var(--muted);font-size:0.78rem">+' + e.ms + 'ms</span></div>';
        }).join('');
        rb.scrollTop = rb.scrollHeight;
      }
    }

    // =========================================================================
    // Test Mode
    // =========================================================================

    async function toggleTestMode() {
      if (testModeActive) { exitTestMode(); } else { enterTestMode(); }
    }

    async function enterTestMode() {
      var btn = document.getElementById('testBtn');
      btn.textContent = 'Entering\u2026';
      btn.disabled = true;
      setDiagButtonsDisabled(true);
      // Show panel immediately in amber/expanded state while connecting.
      testFiresLog = [];
      var p = document.getElementById('testPanel');
      p.classList.remove('tp-compact');
      p.classList.add('tp-expanded', 'tp-visible');
      document.body.style.paddingBottom = '35vh';
      setTestStaDot('amber', 'Connecting\u2026', 'Connecting\u2026');
      renderTestFiresLog();
      try {
        var r    = await fetch('/test/enter');
        var data = await r.json();
        if (!data.ok) {
          // Roll back: hide panel.
          p.classList.remove('tp-visible', 'tp-expanded');
          p.classList.add('tp-compact');
          document.body.style.paddingBottom = '';
          alert('Cannot enter Test Mode: ' + (data.error || 'unknown error'));
          btn.textContent = 'Test';
          btn.disabled = false;
          setDiagButtonsDisabled(false);
          return;
        }
        testModeActive    = true;
        testPanelExpanded = true;
        var staMsg = 'Connected to ' + data.ssid + '  (' + data.ip + ')';
        setTestStaDot('green', staMsg, staMsg);
        var targetUrl = (baseUrlsList.length > 0)
          ? (baseUrlsList[selectedUrlIndex] || baseUrlsList[0]) : '(no base URL)';
        document.getElementById('testTargetUrlE').textContent = targetUrl;
        renderTestFiresLog();
        btn.textContent = 'Exit Test';
        btn.classList.add('test-active');
        btn.disabled = false;
        console.log('[test] entered');
      } catch (e) {
        p.classList.remove('tp-visible', 'tp-expanded');
        p.classList.add('tp-compact');
        document.body.style.paddingBottom = '';
        alert('Test Mode entry failed: ' + e.message);
        btn.textContent = 'Test';
        btn.disabled = false;
        setDiagButtonsDisabled(false);
      }
    }

    async function exitTestMode() {
      testModeActive = false;
      var p = document.getElementById('testPanel');
      p.classList.remove('tp-visible', 'tp-expanded');
      p.classList.add('tp-compact');
      document.body.style.paddingBottom = '';
      var btn = document.getElementById('testBtn');
      btn.textContent = 'Test';
      btn.classList.remove('test-active');
      btn.disabled = false;
      testFiresLog = [];
      setDiagButtonsDisabled(false);
      console.log('[test] exited');
      try { await fetch('/test/exit'); } catch (_) {}
    }

    function setTestExpanded(expanded) {
      testPanelExpanded = expanded;
      var p = document.getElementById('testPanel');
      p.classList.toggle('tp-compact',  !expanded);
      p.classList.toggle('tp-expanded',  expanded);
      document.body.style.paddingBottom = expanded ? '35vh' : '40px';
    }

    function setTestStaDot(color, statusC, statusE) {
      var dotC = document.getElementById('testDotC');
      dotC.className = 'dot dot-test-' + color;
      document.getElementById('testStaStatusC').textContent = statusC || '';
      document.getElementById('testStaStatusE').textContent = statusE || '';
    }

    async function testFireSig(sig) {
      // Look up in current mappings.
      var mapping = null;
      for (var i = 0; i < currentMappings.length; i++) {
        if (currentMappings[i].sig === sig) { mapping = currentMappings[i]; break; }
      }
      if (!mapping) {
        addTestFire({ type: 'unmap', sig: sig });
        console.log('[test] unmapped signature=' + sig);
        return;
      }
      var suffix = mapping.url;
      try {
        var r    = await fetch('/test/fire?sig=' + encodeURIComponent(sig)
                              + '&suffix=' + encodeURIComponent(suffix));
        var data = await r.json();
        if (data.ok) {
          addTestFire({ type: 'ok', sig: sig, url: data.url, status: data.status,
                        elapsed: data.elapsed_ms, body: data.body_excerpt });
          console.log('[test] fire signature=' + sig + ' -> ' + data.url
                      + ' status=' + data.status + ' elapsed=' + data.elapsed_ms + 'ms');
        } else {
          addTestFire({ type: 'err', sig: sig, url: data.url || suffix, error: data.error });
          console.log('[test] fire failed signature=' + sig
                      + ' -> ' + (data.url || suffix) + ' reason=' + (data.error || ''));
        }
      } catch (e) {
        addTestFire({ type: 'err', sig: sig, url: suffix, error: e.message });
        console.log('[test] fire failed signature=' + sig + ' reason=' + e.message);
      }
    }

    function addTestFire(entry) {
      var now = new Date();
      entry.ts = now.getHours().toString().padStart(2,'0') + ':'
               + now.getMinutes().toString().padStart(2,'0') + ':'
               + now.getSeconds().toString().padStart(2,'0');
      testFiresLog.unshift(entry);
      if (testFiresLog.length > 20) testFiresLog.length = 20;
      renderTestFiresLog();
      updateTestCompactLast(entry);
    }

    function renderTestFiresLog() {
      var el = document.getElementById('testFiresLog');
      if (!el) return;
      if (!testFiresLog.length) {
        el.innerHTML = '<div style="padding:10px;color:#6a8f7a;font-size:0.82rem">'
                     + 'Press a button on your BLE device to see results here.</div>';
        return;
      }
      el.innerHTML = testFiresLog.map(function(e) {
        var ts = '<span style="color:#6a8f7a">' + htmlEsc(e.ts) + '</span>';
        if (e.type === 'unmap') {
          return '<div class="tp-fire-entry tp-fire-unmap">' + ts
               + ' &nbsp; <em>unmapped</em> &nbsp; ' + htmlEsc(e.sig) + '</div>';
        }
        if (e.type === 'ok') {
          var urlSuffix = e.url ? e.url.replace(/^https?:\/\/[^/]+/, '') : e.sig;
          var bodyPart  = e.body
            ? ' &nbsp;<span style="color:#8fc8a8">' + htmlEsc(e.body) + '</span>'
            : '';
          return '<div class="tp-fire-entry tp-fire-ok">' + ts
               + ' &nbsp; ' + htmlEsc(urlSuffix)
               + ' &rarr; ' + e.status + ' OK &nbsp; ' + e.elapsed + 'ms'
               + bodyPart + '</div>';
        }
        return '<div class="tp-fire-entry tp-fire-err">' + ts
             + ' &nbsp; ' + htmlEsc(e.url || e.sig)
             + ' &rarr; ERROR: ' + htmlEsc(e.error || '') + '</div>';
      }).join('');
    }

    function updateTestCompactLast(entry) {
      var el = document.getElementById('testCompactLast');
      if (!el) return;
      if (entry.type === 'unmap') {
        el.textContent = 'Unmapped: ' + entry.sig;
      } else if (entry.type === 'ok') {
        var urlSuffix = entry.url ? entry.url.replace(/^https?:\/\/[^/]+/, '') : entry.sig;
        el.textContent = 'Last: ' + urlSuffix + ' \u2192 ' + entry.status + ' OK ' + entry.elapsed + 'ms';
      } else {
        el.textContent = 'Error: ' + (entry.error || entry.sig);
      }
    }

    function setDiagButtonsDisabled(disabled) {
      var ids = ['diagConnectBtn'];
      ids.forEach(function(id) {
        var el = document.getElementById(id);
        if (el) el.disabled = disabled;
      });
    }

    window.addEventListener('beforeunload', function() {
      if (testModeActive) { navigator.sendBeacon('/test/exit'); }
    });

    setInterval(refreshState, 500);
    refreshState();
    loadConfig();
  </script>
</body>
</html>
)HTML";
