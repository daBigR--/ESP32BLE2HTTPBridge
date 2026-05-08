#include "web_page.h"

// =============================================================================
// web_page.cpp Ã¢â‚¬â€ Single-page config UI served in CONFIG mode
// =============================================================================
//
// Layout: persistent sticky header + 4 tabs (General / BLE / Actions / System)
//   Header  Ã¢â‚¬â€ bonded keyboard status dot + name, conditional Connect button,
//             Apply & Run primary action (opens confirmation modal Ã¢â€ â€™ /reboot)
//   General Ã¢â‚¬â€ WiFi networks, Power & Sleep timeout
//   BLE     Ã¢â‚¬â€ Keyboard scan, discovered devices, bonded device detail
//   Actions Ã¢â‚¬â€ Base URLs, button capture + assignment, current mappings
//   System  Ã¢â‚¬â€ Reboot, Factory Reset, Recent Bursts feed, Pressed Keys log
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

    /* Ã¢â€â‚¬Ã¢â€â‚¬ Header Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ */
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
    #bondedStatus {
      display: flex;
      align-items: center;
      gap: 8px;
      cursor: pointer;
      flex: 1;
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

    /* Ã¢â€â‚¬Ã¢â€â‚¬ Tab bar Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ */
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

    /* Ã¢â€â‚¬Ã¢â€â‚¬ Status bar Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ */
    #state {
      background: #eef4f1;
      color: var(--muted);
      font-size: 0.85rem;
      padding: 6px 10px;
      border-radius: 8px;
      border: 1px solid var(--line);
      margin-top: 10px;
      min-height: 28px;
    }

    /* Ã¢â€â‚¬Ã¢â€â‚¬ Layout Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ */
    .wrap { max-width: 980px; margin: 0 auto; }
    .tab-panel { display: none; padding: 16px; }
    .tab-panel.active { display: block; }

    /* Ã¢â€â‚¬Ã¢â€â‚¬ Card Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ */
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

    /* Ã¢â€â‚¬Ã¢â€â‚¬ Buttons Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ */
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
    }
    button.alt  { background: #6b7f75; }
    button.warn { background: var(--warn); }

    /* Ã¢â€â‚¬Ã¢â€â‚¬ Lists Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ */
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

    /* Ã¢â€â‚¬Ã¢â€â‚¬ Misc Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ */
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
    input[type=text], input[type=number] {
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
    .mapping-row {
      display: flex;
      align-items: center;
      gap: 10px;
      padding: 7px 0;
      border-bottom: 1px solid var(--line);
    }
    .mapping-row:last-child { border-bottom: none; }
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

    /* Ã¢â€â‚¬Ã¢â€â‚¬ Modal Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ */
    .empty-state {
      padding: 18px 4px;
      color: #888;
      font-size: 0.88rem;
      font-style: italic;
      text-align: left;
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

    /* Ã¢â€â‚¬Ã¢â€â‚¬ Responsive Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ */
    @media (max-width: 480px) {
      .log { height: 200px; }
      li { flex-direction: column; align-items: flex-start; }
      #bondedName  { font-size: 0.85rem; }
      .tab-btn     { font-size: 0.78rem; padding: 8px 2px 7px; }
      #applyRunBtn { font-size: 0.8rem; padding: 6px 10px; }
      #connectBtn  { font-size: 0.8rem; padding: 6px 10px; }
    }
  </style>
</head>
<body>

  <!-- Ã¢â€â‚¬Ã¢â€â‚¬ Sticky header Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ -->
  <div id="appHeader">
    <div id="bondedStatus" onclick="switchTab('ble')" title="Go to BLE tab">
      <span id="statusDot" class="dot dot-none"></span>
      <span id="bondedName">No device bonded</span>
    </div>
    <div id="headerActions">
      <button id="connectBtn" onclick="connectBonded()" style="display:none">Connect</button>
      <button id="applyRunBtn" onclick="openApplyModal()">Apply &amp; Run</button>
    </div>
  </div>

  <!-- Ã¢â€â‚¬Ã¢â€â‚¬ Tab bar Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ -->
  <div id="tabBar">
    <button class="tab-btn" data-tab="general" onclick="switchTab('general')">General</button>
    <button class="tab-btn" data-tab="ble"     onclick="switchTab('ble')">BLE</button>
    <button class="tab-btn" data-tab="actions" onclick="switchTab('actions')">Actions</button>
    <button class="tab-btn" data-tab="system"  onclick="switchTab('system')">System</button>
  </div>

  <!-- Ã¢â€â‚¬Ã¢â€â‚¬ Tab panels Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ -->
  <div class="wrap">

    <!-- GENERAL Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ -->
    <div id="tab-general" class="tab-panel">
      <div class="card">
        <h2>WiFi Networks</h2>
        <div class="cfg-section">
          <div class="row" style="gap:8px;margin-bottom:8px">
            <input type="text" id="wifiSsidInput" placeholder="SSID" style="flex:1" />
            <input type="text" id="wifiPwdInput" placeholder="Password" style="flex:1" />
            <button onclick="addWifi()">Add</button>
          </div>
          <div id="wifiNetworksList"></div>
        </div>
      </div>
      <div class="card">
        <h2>Power &amp; Sleep</h2>
        <div class="cfg-section" style="margin-bottom:0">
          <div style="font-size:0.82rem;color:var(--muted);margin-bottom:6px">Inactivity timeout before sleep, only in normal mode and battery use. Long press in normal mode sleeps immediately.</div>
          <div class="row" style="gap:8px">
            <input type="number" id="sleepTimeoutMinInput" min="0.5" step="0.5" placeholder="10" style="width:140px" />
            <span style="color:var(--muted)">minutes</span>
            <button onclick="saveSleepTimeout()">Save Timeout</button>
          </div>
        </div>
      </div>
    </div>

    <!-- BLE Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ -->
    <div id="tab-ble" class="tab-panel">
      <div class="card">
        <h2>Keyboard Scan</h2>
        <div class="row">
          <button onclick="scan()">Scan Keyboards</button>
        </div>
        <div id="state">Idle</div>
        <h2 style="margin-top:16px">Discovered Devices</h2>
        <ul id="devices"></ul>
      </div>
      <div class="card" id="bondedCard" style="display:none">
        <h2>Bonded Device</h2>
        <div id="bondedInfo"></div>
      </div>
    </div>

    <!-- ACTIONS Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ -->
    <div id="tab-actions" class="tab-panel">
      <div class="card">
        <h2>Base URLs</h2>
        <div class="cfg-section" style="margin-bottom:0">
          <div style="font-size:0.82rem;color:var(--muted);margin-bottom:6px">In run mode button short press cycles base URLs, double press saves the selection.</div>
          <div class="row" style="gap:8px;margin-bottom:8px">
            <input type="text" id="newUrlInput" placeholder="http://192.168.x.x:8080" style="flex:1" />
            <button id="urlActionBtn" onclick="addUrl()">Add URL</button>
            <button id="urlCancelBtn" class="alt" onclick="cancelUrlEdit()" style="display:none">Cancel</button>
          </div>
          <div id="baseUrlsList"></div>
        </div>
      </div>
      <div class="card">
        <h2>Assign a Button</h2>
        <div class="cfg-section">
          <div class="row" style="margin-bottom:8px;gap:8px">
            <button id="captureBtn" onclick="startCapture()">Capture Burst</button>
            <span class="captured-box" id="capturedKey">&mdash;</span>
          </div>
          <div class="row" style="gap:8px;margin-bottom:6px">
            <input type="text" id="mappingUrl" placeholder="/event/1" style="flex:2" />
            <input type="text" id="mappingLabel" placeholder="Label (optional)" style="flex:1" />
            <button id="assignBtn" onclick="saveMapping()" disabled>Assign</button>
            <button id="mappingCancelBtn" class="alt" onclick="cancelMappingEdit()" style="display:none">Cancel</button>
          </div>
        </div>
        <div class="cfg-section" style="margin-bottom:0">
          <label class="cfg-label">Current Mappings</label>
          <div id="mappingsList"></div>
        </div>
      </div>
    </div>

    <!-- SYSTEM Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ -->
    <div id="tab-system" class="tab-panel">
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
        <h2>Recent Bursts</h2>
        <div style="font-size:0.82rem;color:var(--muted);margin-bottom:6px">Live feed of the last 20 button presses seen by the device (newest at bottom).</div>
        <div id="recentBurstsList" style="font-family:monospace;font-size:0.82rem;max-height:180px;overflow-y:auto;background:#f8faf9;padding:8px;border-radius:6px;border:1px solid var(--line)"></div>
      </div>
      <div class="card">
        <h2>Pressed Keys</h2>
        <div id="keys" class="log"></div>
      </div>
    </div>

  </div><!-- /.wrap -->

  <!-- Ã¢â€â‚¬Ã¢â€â‚¬ Apply & Run confirmation modal Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ -->
  <div id="applyModal" class="modal-bg">
    <div class="modal">
      <h3>Apply &amp; Run</h3>
      <p>This will exit configuration mode and reboot into run mode. The web UI will become unreachable.<br><br>
         To return to configuration, hold the boot button (D10) on the ESP32 for &ge;&nbsp;0.8&nbsp;s at power-on.</p>
      <div class="actions">
        <button class="alt" onclick="closeApplyModal()" style="margin-right:8px">Cancel</button>
        <button onclick="confirmApplyRun()">Apply &amp; Run</button>
      </div>
    </div>
  </div>

  <script>
    // Ã¢â€â‚¬Ã¢â€â‚¬ Tab routing Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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

    // Ã¢â€â‚¬Ã¢â€â‚¬ DOM refs & shared state Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    var elState   = document.getElementById('state');
    var elDevices = document.getElementById('devices');
    var elKeys    = document.getElementById('keys');
    var currentBondedAddress = '';
    var currentBondedName    = '';

    function status(text) {
      elState.textContent = text;
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ BLE scan Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    var scanRan = false;

    async function scan() {
      scanRan = true;
      status('Scanning for 4 seconds...');
      var r = await fetch('/scan');
      var data = await r.json();
      renderDevices(data.devices || [], currentBondedAddress);
      status('Scan complete');
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

    // Ã¢â€â‚¬Ã¢â€â‚¬ Bonded device panel (BLE tab) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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

    // Ã¢â€â‚¬Ã¢â€â‚¬ Header update Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    function updateHeader(s) {
      var dot    = document.getElementById('statusDot');
      var nameEl = document.getElementById('bondedName');
      var btn    = document.getElementById('connectBtn');
      if (!s.bondedAddress) {
        dot.className    = 'dot dot-none';
        nameEl.textContent = 'No device bonded';
        btn.style.display  = 'none';
        return;
      }
      nameEl.textContent = s.bondedName || s.bondedAddress;
      if (s.connected) {
        dot.className  = 'dot dot-connected';
        btn.style.display = 'none';
      } else {
        dot.className  = 'dot dot-bonded';
        btn.style.display = '';
      }
    }

    function connectBonded() {
      if (currentBondedAddress) connectDevice(currentBondedAddress, currentBondedName);
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ BLE device actions Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    async function pairDevice(address, name) {
      status('Pairing with ' + address + ' ...');
      var r = await fetch('/pair?addr=' + encodeURIComponent(address) + '&name=' + encodeURIComponent(name));
      var data = await r.json();
      if (data.ok) {
        status('Paired with ' + name + '. Waiting for automatic reconnect...');
        await refreshState();
      } else {
        status(data.error || 'Pair failed');
      }
    }

    async function connectDevice(address, name) {
      status('Connecting to ' + address + ' ...');
      var r = await fetch('/connect?addr=' + encodeURIComponent(address) + '&name=' + encodeURIComponent(name));
      var data = await r.json();
      if (data.ok) {
        status('Connected to ' + name);
      } else {
        status(data.error || 'Connect failed');
      }
    }

    async function unpairDevice(address, name) {
      status('Unpairing ' + address + ' ...');
      var r = await fetch('/unpair?addr=' + encodeURIComponent(address));
      var data = await r.json();
      if (data.ok) {
        status('Unpaired ' + (name || address));
        await scan();
      } else {
        status(data.error || 'Unpair failed');
      }
    }

    async function disconnectNow() {
      await fetch('/disconnect');
      status('Disconnected');
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Sleep timeout Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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
      if (!Number.isFinite(minutes) || minutes <= 0) { status('Enter a valid timeout in minutes'); return; }
      var ms = Math.round(minutes * 60000);
      var r = await fetch('/config/setsleeptimeout?ms=' + encodeURIComponent(ms));
      var data = await r.json();
      if (!data.ok) { status(data.error || 'Failed to save timeout'); return; }
      renderSleepTimeout(data.sleepTimeoutMs || ms);
      status('Sleep timeout saved');
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ WiFi networks Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    var wifiNets = [];

    function renderWifiNetworks(nets) {
      wifiNets = nets;
      var el = document.getElementById('wifiNetworksList');
      if (!nets.length) {
        el.innerHTML = '<div class="empty-state">No networks saved \u2014 fill in the SSID and password above to add one.</div>';
        return;
      }
      el.innerHTML = nets.map(function(n, i) {
        return '<div class="mapping-row"><span style="flex:1">' + n.ssid + '</span>' +
               '<button class="warn" style="padding:5px 10px;font-size:0.8rem" onclick="deleteWifi(' + i + ')">Del</button></div>';
      }).join('');
    }

    async function addWifi() {
      var ssid = document.getElementById('wifiSsidInput').value.trim();
      var pwd  = document.getElementById('wifiPwdInput').value;
      if (!ssid) { status('Enter WiFi SSID first'); return; }
      await fetch('/config/addwifi?ssid=' + encodeURIComponent(ssid) + '&pwd=' + encodeURIComponent(pwd));
      document.getElementById('wifiSsidInput').value = '';
      document.getElementById('wifiPwdInput').value  = '';
      await loadConfig();
      status('WiFi network saved');
    }

    async function deleteWifi(idx) {
      var net = wifiNets[idx];
      if (!net) return;
      await fetch('/config/delwifi?ssid=' + encodeURIComponent(net.ssid));
      await loadConfig();
      status('WiFi network removed');
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Base URLs Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    var baseUrlsList = [];
    var urlEditIndex = -1;

    function renderBaseUrls(urls, selectedIdx) {
      baseUrlsList = urls;
      var el = document.getElementById('baseUrlsList');
      if (!urls.length) {
        el.innerHTML = '<div class="empty-state">No URLs configured \u2014 enter a URL above to add one.</div>';
        return;
      }
      el.innerHTML = urls.map(function(u, i) {
        var badge = (i === selectedIdx)
          ? '<span class="pill ok" style="margin-left:6px">Active</span>' : '';
        var activateBtn = (i === selectedIdx)
          ? '<button class="alt" style="padding:5px 10px;font-size:0.8rem" disabled>Active</button>'
          : '<button style="padding:5px 10px;font-size:0.8rem" onclick="activateUrl(' + i + ')">Activate</button>';
        return '<div class="mapping-row">' +
               '<span style="flex:1" class="mono">' + u + badge + '</span>' +
               activateBtn +
               '<button class="alt" style="padding:5px 10px;font-size:0.8rem" onclick="beginEditUrl(' + i + ')">Edit</button>' +
               '<button class="warn" style="padding:5px 10px;font-size:0.8rem" onclick="deleteUrl(' + i + ')">Del</button>' +
               '</div>';
      }).join('');
    }

    async function activateUrl(idx) {
      await fetch('/config/selecturl?idx=' + idx);
      await loadConfig();
      status('Active URL set to #' + (idx + 1));
    }

    async function addUrl() {
      var url = document.getElementById('newUrlInput').value.trim();
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
      if (urlEditIndex === idx) cancelUrlEdit();
      else if (urlEditIndex > idx) urlEditIndex--;
      await loadConfig();
      status('URL removed');
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Button mappings Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    var capturing = false;
    var capturedKeyHex = '';
    var lastSeenKey = '';
    var mappingEditOriginalKey = '';

    function renderMappings(mappings) {
      var el = document.getElementById('mappingsList');
      if (!mappings.length) {
        el.innerHTML = '<div class="empty-state">No mappings yet \u2014 connect a BLE device, press a button, then assign a URL to the captured signature.</div>';
        return;
      }
      el.innerHTML = mappings.map(function(m) {
        return '<div class="mapping-row">' +
          '<span class="mono" style="min-width:72px">' + m.sig + '</span>' +
          '<span style="flex:1">' + m.url + '</span>' +
          '<span style="color:var(--muted);font-size:0.85rem;min-width:60px">' + (m.label || '') + '</span>' +
          '<button class="alt" style="padding:5px 10px;font-size:0.8rem" ' +
            'onclick="beginEditMapping(\'' + m.sig + '\',\'' + encodeURIComponent(m.url) + '\',\'' + encodeURIComponent(m.label || '') + '\')">Edit</button>' +
          '<button class="warn" style="padding:5px 10px;font-size:0.8rem" onclick="deleteMapping(\'' + m.sig + '\')">Delete</button>' +
          '</div>';
      }).join('');
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
      if (!capturing || !lastSig || lastSig === lastSeenKey) return;
      capturing = false;
      capturedKeyHex = lastSig;
      document.getElementById('capturedKey').textContent = lastSig;
      document.getElementById('assignBtn').disabled = false;
      document.getElementById('captureBtn').textContent = 'Capture Burst';
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
      document.getElementById('captureBtn').textContent = 'Capture Burst';
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
      document.getElementById('captureBtn').textContent = 'Capture Burst';
      document.getElementById('captureBtn').disabled = false;
    }

    async function saveMapping() {
      if (!capturedKeyHex) return;
      var url   = document.getElementById('mappingUrl').value.trim();
      var label = document.getElementById('mappingLabel').value.trim();
      if (!url) { status('Enter a URL path first'); return; }
      if (mappingEditOriginalKey && mappingEditOriginalKey !== capturedKeyHex) {
        await fetch('/config/delmapping?sig=' + encodeURIComponent(mappingEditOriginalKey));
      }
      await fetch('/config/setmapping?sig=' + encodeURIComponent(capturedKeyHex) +
                  '&url=' + encodeURIComponent(url) + '&label=' + encodeURIComponent(label));
      status((mappingEditOriginalKey ? 'Updated ' : 'Mapped ') + capturedKeyHex + ' \u2192 ' + url);
      cancelMappingEdit();
      await loadConfig();
    }

    async function deleteMapping(sig) {
      await fetch('/config/delmapping?sig=' + encodeURIComponent(sig));
      if (mappingEditOriginalKey === sig) cancelMappingEdit();
      await loadConfig();
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Load all config Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    async function loadConfig() {
      var r = await fetch('/config');
      var c = await r.json();
      renderWifiNetworks(c.wifiNetworks || []);
      renderBaseUrls(c.baseUrls || [], c.selectedUrlIndex || 0);
      renderSleepTimeout(c.sleepTimeoutMs || (10 * 60 * 1000));
      renderMappings(c.mappings || []);
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ System: reboot / factory reset Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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

    // Ã¢â€â‚¬Ã¢â€â‚¬ Apply & Run modal Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    function openApplyModal() {
      document.getElementById('applyModal').classList.add('open');
    }

    function closeApplyModal() {
      document.getElementById('applyModal').classList.remove('open');
    }

    async function confirmApplyRun() {
      closeApplyModal();
      status('Applying config and rebooting to run mode...');
      await fetch('/reboot');
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Poll: refreshState Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    async function refreshState() {
      var r = await fetch('/state');
      var s = await r.json();
      currentBondedAddress = s.bondedAddress || '';
      currentBondedName    = s.bondedName    || '';

      updateHeader(s);
      renderBondedPanel(s);

      elKeys.innerHTML = (s.keys || []).map(function(k) { return '<div>' + k + '</div>'; }).join('');
      elKeys.scrollTop = elKeys.scrollHeight;

      if (s.lastSig && s.lastSig !== lastSeenKey) {
        checkCapture(s.lastSig);
        lastSeenKey = s.lastSig;
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

    setInterval(refreshState, 500);
    refreshState();
    loadConfig();
  </script>
</body>
</html>
)HTML";
