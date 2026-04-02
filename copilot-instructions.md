# Copilot Instructions

This repository needs session-to-session continuity. Do not rely on prior chat memory.

At the start of every new chat in this repo:
- Read /memories/repo/notes.md.
- Read /memories/repo/continuity.md if it exists.
- Read DEBUG_HANDOFF.md if it exists.
- For serial, config-mode, or BLE scan issues, inspect src/main.cpp, src/web_ble_api.cpp, src/ble_scanner.cpp, src/key_log.cpp, and platformio.ini before making claims about current behavior.

Working rules:
- Trust the current workspace files and repo memory over recollections from previous chats.
- Before saying a behavior exists or was removed, verify it in the actual code.
- After any substantive debugging session, update DEBUG_HANDOFF.md with verified findings, remaining uncertainties, and exact next tests.
- If a conclusion is durable project knowledge, record it in repo memory.

Current project focus:
- ESP32-S3 native USB CDC serial behavior across reset and config-mode transitions.
- BLE scan, pair, and connect diagnostics must remain visible both in the web UI log and on Serial.