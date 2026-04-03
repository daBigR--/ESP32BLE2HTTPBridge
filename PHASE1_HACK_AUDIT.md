# Phase 1 Hack Audit (V1 No-Hacks Baseline)

## Scope

Audit objective: identify and remove device-specific BLE behavior branches before
continuing broader pairing/reconnect fixes.

## Findings

### 1) Active device-specific reconnect branch exists

Location:
- src/ble_keyboard.cpp in connectToKeyboard()

Current behavior:
- If device name contains "D07", explicit secure reconnect is skipped.
- For all other devices, secureConnection() is attempted when not encrypted.

Why this violates v1 no-hacks goal:
- Behavior changes based on product name.
- Same protocol state (bonded + not encrypted) is handled differently by name.

Risk:
- Reconnect logic may become brittle as new devices are added.
- Name-based branching can hide underlying generic connection-state issues.

## Phase 1 Deliverable

Replace the current name-based reconnect path with a generic policy that applies
uniformly to all devices.

## Proposed Implementation Direction (Generic)

1. Remove all name-based checks from connectToKeyboard().
2. Keep one reconnect state machine for every bonded device:
   - Open link.
   - If already encrypted, proceed.
   - If not encrypted, attempt security establishment under a generic policy.
   - Subscribe to HID inputs.
3. If security establishment is not viable in practice for some devices,
   fallback handling must be protocol-state based (not name-based), such as:
   - connection encrypted/not encrypted
   - subscribe success/failure
   - descriptor/permission behavior

## Out of Scope for This Audit

- Solving Kobo pairing failure.
- Full multi-service report-source arbitration.
- Final reconnect tuning.

Those items proceed after this no-hacks baseline is in place.

## Immediate Next Step

Implement Phase 1 code change in src/ble_keyboard.cpp by removing the D07
special branch and replacing it with a unified reconnect flow, then validate on
all 4 devices using the lightweight v1 checklist.
