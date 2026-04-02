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

## Next Phase: Reconnection

Reconnect to bonded keyboard was unreliable before the BLE rebuild — this is the remaining known issue.
Key notes from prior work:
- Connect via fresh advertised device (preserves address type) rather than direct string address.
- RPA resolution requires stored IRK; NimBLE stores it during bonding.
- Do not touch the scan/pair/bond flow — it is working.

## If Starting a New Session

1. Read this file first.
2. Scan and pair are verified working — do not re-investigate them.
3. Focus is reconnect reliability.