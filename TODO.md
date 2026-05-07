# TODO

## High Priority

- [x] **Device validation sweep**: all devices pairing, connecting, and subscribing
      correctly as of 2026-05-05.

- [x] **Review subscribed BLE services**: the current code subscribes to nearly all
      services published by the keyboard. Audit which services are actually needed for
      key capture and limit subscriptions to only those — over-subscribing may cause
      instability or unexpected behavior on some devices.
      Note: not really multiple services but some multiple key presses on single event solved by burst signatures.

- [x] **Mode-aware connection behavior**: differentiate reconnect logic by operating mode.
      - **CONFIG mode**:
        - (a) At boot, attempt auto-connect at most twice to the most-recently-bonded device.
        - (b) After pairing a new device, keep the resulting connection up — do not
              disconnect-then-reconnect.
        - (c) On any subsequent disconnect, do not auto-retry; wait for explicit user
              action via the "Connect" button.
      - **RUN mode**: retain auto-reconnect with a backoff schedule
        (1 s → 5 s → 15 s → 30 s, then 30 s repeating).
      Eliminates UI intrusion in config mode while preserving transparent recovery
      in run mode.

- [x] **Remove dead address-type fallback ladder in connect path**: after the existing
      "scan-record connect" and "direct address generic" attempts, the code currently
      cycles through BLE_ADDR_RANDOM, BLE_ADDR_PUBLIC, BLE_ADDR_RANDOM_ID, and
      BLE_ADDR_PUBLIC_ID. These have never produced a successful connection in any test
      and add 30+ seconds of noise per failed attempt. Replace the entire fallback ladder
      with a single failure log: `connect failed: scan miss + direct address timeout`.
      The mode-aware reconnect controller handles retry behavior from there.

## UI Polish

- [x] Bonded-device panel: show device name when available, not only MAC address.
      (Observed: Boox connected successfully but bonded panel showed MAC-only.)

- [x] Scan list: collapse "Other Devices" section by default; expand only on user tap.

## Cleanup / Tech Debt

- [x] Delete `BLEPairBasicDiagnostic/` and `BLEPairBasicDiagnosticBaseline/` directories —
      these were temporary debugging aids and are no longer needed.

## Firmware

- [ ] Battery monitoring: add ADC voltage divider circuit so firmware can read and
      report battery level, and support low-battery behavior (e.g. warning in UI,
      auto-sleep).

- [ ] Multi-bond support: remove the single-bond-slot limitation. Allow multiple
      stored bonds and update auto-reconnect logic to know which device to favor
      (e.g. last used, user-set priority, or first seen in scan).

- [ ] **Config-mode button behavior**: D10 is currently ignored during CONFIG mode runtime.
      - Short press → switch to RUN mode immediately (without reboot if possible).
      - Long press (≥800 ms) → reboot.
      - RUN-mode button behavior (short = cycle URL, long = save/sleep) stays unchanged.
