# V1 Acceptance Plan (4 Devices, No Hacks)

## V1 Goal

For the 4 target devices, achieve all three outcomes reliably:
1. Pairing
2. Connecting/Reconnecting
3. Key reporting

This v1 is complete only when all devices pass all criteria below.

## DIY Validation Mode (Practical Use)

For this project, strict statistical logging is optional. You can use this plan
as a practical checklist and make a subjective pass/fail decision per device,
as long as behavior is consistently reliable in normal use.

Recommended lightweight approach:
1. Run each phase multiple times per device (enough to build confidence).
2. Log only notable failures and regressions.
3. Use the scoreboard as a simple Pass/Fail tracker.
4. Keep the "no hacks" rule strict even when testing is informal.

## Target Devices

1. MK321BT
2. Boox RemoteControl
3. D07
4. Kobo Remote

## What "No Hacks" Means in This Project

The BLE flow must be generic and protocol-driven, not device-name-specific.

Not allowed:
- Device-name checks that change connect/pair behavior.
- Device-address hardcoding.
- One-off timing branches for a single model.
- Duplicate subscription suppressors keyed to one product.

Allowed:
- Standards-based HID service/characteristic selection logic.
- Generic retry/backoff used for all devices.
- Generic de-dup rules based on report source/handle and timing.
- Capability detection based on discovered services/descriptors.

## Acceptance Criteria

### A) Pairing

Per device:
1. Start from unpaired state (bond removed on ESP32 and device side if possible).
2. Device appears in scan while in pairing mode.
3. Pair succeeds without changing firmware between devices.
4. Bond persists across ESP32 reboot.

Pass threshold:
- 5/5 successful pair attempts per device.

### B) Connect/Reconnect

Per device:
1. Connect succeeds from bonded state.
2. After intentional disconnect, reconnect succeeds.
3. After ESP32 reboot, auto-connect or user connect succeeds.
4. After device power cycle, reconnect succeeds.

Pass threshold:
- 20 reconnect cycles with at least 95% success per device.
- No permanent stuck state requiring firmware reflash.

### C) Key Reporting Reliability

Per device:
1. No duplicate key events for single physical press.
2. No missed key events during normal operation.
3. Stable key mapping across reconnects/reboots.
4. For multi-service devices, only the correct HID input source is used.

Pass threshold:
- 200 keypress sample per device:
  - Duplicate rate <= 1%
  - Miss rate <= 1%

## Test Protocol (Run Order)

1. Flash current candidate firmware.
2. Run pairing tests for all 4 devices.
3. Run reconnect cycle test for all 4 devices.
4. Run 200-key reliability test for each device.
5. Record failures with logs and exact phase marker.
6. Fix one root cause at a time, then rerun only failed phase first.
7. After phase fix, rerun full per-device acceptance for regression safety.

## Execution Rules

1. Keep one stable fallback commit before each major BLE change.
2. Do not merge unverified behavior changes across multiple phases at once.
3. Every failure report must include:
   - Device name
   - Phase (Pair, Connect, Reconnect, Key)
   - Firmware commit hash
   - Relevant serial log snippet

## Scoreboard Template

Use this table and update it as tests run.

| Device | Pair 5/5 | Reconnect >=95% | Keys dup<=1% miss<=1% | Status |
| --- | --- | --- | --- | --- |
| MK321BT | TBD | TBD | TBD | Not started |
| Boox RemoteControl | TBD | TBD | TBD | Not started |
| D07 | TBD | TBD | TBD | Not started |
| Kobo Remote | TBD | TBD | TBD | Not started |

## Exit Criteria for V1

V1 is done when:
1. All 4 devices are Pass in scoreboard.
2. No device-specific BLE behavior branches remain.
3. Results are reproducible after at least one full clean reboot test day.
