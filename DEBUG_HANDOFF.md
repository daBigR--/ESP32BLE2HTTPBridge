# Debug Handoff

## Background / What Happened

- There was a working version (committed) tested with one BLE device.
- Then multi-device / multi-HID work was done for several days: scan, pair, bond all worked, but reconnect was unreliable.
- A rebuild of the BLE layer was started to follow standard BLE practice more closely.
- After that rebuild, all devices disappeared from scan results.
- **All day was spent trying to solve it. The actual root cause was found at end of day.**

## Root Cause (CONFIRMED, FIXED)

`maybeAutoConnectBondedKeyboard()` in `src/main.cpp` `loop()` was called **outside** the
`if (!gConfigMode)` block. That function fires its own 2-second BLE scan every 8 seconds
using the same `NimBLEDevice::getScan()` scanner object. When the user clicks Scan in the
web UI, the config-mode `/scan` handler starts a 10-second scan on that same scanner.
If the auto-connect scan fires concurrently or immediately before, NimBLE returns 0 results
to whichever scan started second, making it look like no BLE devices are in range.

**Fix applied:** scanner contention is now prevented by **suspending auto-connect during `/scan`**
in `src/web_ble_api.cpp` (`setAutoConnectEnabled(false)`) rather than globally disabling
auto-connect in config mode. Auto-connect is re-enabled after successful pairing and on reboot.

## Current Code State — VERIFIED WORKING ON HARDWARE (April 2 2026)

- **Scan**: all devices show correctly. Root cause of empty scan (auto-connect competing with scanner) fixed.
- **Grouping**: Discoverable → Not Discoverable → Other Devices (unnamed). Sorted by RSSI. Named devices show only Bonded/Unbonded pill. Other Devices show both discoverable + bonded pills.
- **Bonded device hiding**: selected bonded device is excluded from scan list; appears only in its own bonded panel.
- **Pairing**: works correctly. Devices that require manual pairing mode are expected behavior, not a bug.
- Scan duration: 4 s.
- Auto-connect behavior: active in both config and run modes, suspended during explicit web scan, resumed after successful pairing.
- Latest multi-device test result (end of day):
	- MK321BT: pair/connect/reconnect works.
	- Boox RemoteControl: pair/connect/reconnect works.
	- D07: pair/connect/reconnect works with the D07 reconnect path.
	- Kobo Remote: still fails to pair at link-open (`Connect failed`) even when visible in scan.
- Last attempted Kobo experiments (same-name alternate connect + HID subscription dedupe) were reverted after causing regressions in other devices.

## V1 Acceptance Summary (April 2-3 2026)

**Phase: All 4 devices pair/reconnect/key-report (NO HACKS)**

### ✓ COMPLETED (3 of 4 devices)

**MK321BT**: pair (5/5) → reconnect (reboot/reset cycles all work) → keypress (perfect, no dupes/misses)
**Boox RemoteControl**: pair (5/5) → reconnect (identical to MK321) → keypress (perfect)
**D07**: pair (5/5) → reconnect (identical to others; no hangs with bounded async security) → keypress (delivers keys; multicode is device design)

All use encrypted+bonded reconnect via generic `trySecurityUpgradeWithTimeout()` (commit 160ba09)
No device-specific BLE branches remain

### ❌ BLOCKED (1 of 4 devices)

**Kobo Remote**: 
- Visible in scan (flags=0x6, addr_type=0, good RSSI)
- GAP connection fails with rc=574 on all paths: advertised-device, generic, RANDOM, PUBLIC, extended 20s timeout
- **Critical clue**: worked in earlier intermediate uncommitted version → fix is targeted/small, not major refactor
- Unpaired from Sage, still fails identically

### Code Changes This Session

1. Phase 1 Audit: removed D07-specific SMP bypass (no device names in BLE logic now)
2. Implemented bounded async security upgrade (avoids blocking hangs)
3. Added Kobo diagnostics logging to show scan details + per-attempt error codes

**Next step**: User will investigate what changed in pairing between earlier working version and current

## If Starting a New Session

1. Read this file first.
2. V1 goal: 4/4 devices pair/reconnect/keys without hacks.
3. Status: 3/4 working perfectly. Kobo blocks; GAP connection rc=574.
4. Kobo history: worked before → user will debug what changed.
5. Do not apply more generic tuning; wait for user's investigation.