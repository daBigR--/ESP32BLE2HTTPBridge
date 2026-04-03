# Copilot Instructions

This repository needs session-to-session continuity. Do not rely on prior chat memory.

At the start of every new chat in this repo:
- Read DEBUG_HANDOFF.md (this is the source of truth for current session state).
- Read /memories/repo/notes.md for durable project facts.
- For serial, config-mode, or BLE scan issues, inspect src/main.cpp, src/web_ble_api.cpp, src/ble_scanner.cpp, src/key_log.cpp first.

Working rules:
- Trust the current workspace files and repo memory over recollections.
- Before claiming a behavior exists or was removed, verify it in actual code.
- After substantive debugging, update DEBUG_HANDOFF.md with findings + exact next tests.
- Recording durable knowledge in /memories/repo/notes.md is user's call.

## Current Project State (April 2-3 2026)

**Goal**: V1 acceptance — all 4 BLE devices (MK321BT, Boox, D07, Kobo) pair/reconnect/report keys reliably without device-specific code hacks.

**Status**:
- ✓ 3 devices complete (pair, reconnect with bonded+encrypted, keypress delivery all verified)
- ✗ Kobo pairing blocked by GAP connection failure (rc=574)

**Latest completed work**:
- Commit 160ba09: "Bonded reconnect security restored: bounded async security upgrade on reconnect"
- Removed all device name checks from BLE flow (Phase 1 audit)
- Implemented generic `trySecurityUpgradeWithTimeout()` for safe bonded reconnect
- All working devices use one unified reconnect path (no hacks)

**Kobo status**:
- Device visible in scan (good RSSI, correct flags)
- Fails at GAP layer: rc=574 on all connection paths
- Evidence: worked in earlier intermediate version → user will investigate what changed
- Diagnostics added: detailed logging of scan results, flags, address type, per-attempt error codes

**Next session task**: Wait for user's investigation findings before applying fixes.