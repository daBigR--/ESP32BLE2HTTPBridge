# TODO

## High Priority

- [x] **Device validation sweep**: all devices pairing, connecting, and subscribing
      correctly as of 2026-05-05.

- [x] **Review subscribed BLE services**: the current code subscribes to nearly all
      services published by the keyboard. Audit which services are actually needed for
      key capture and limit subscriptions to only those — over-subscribing may cause
      instability or unexpected behavior on some devices.
      Note: not really multiple services but some multiple key presses on single event solved by burst signatures.

- [ ] **Mode-aware connection behavior**: differentiate reconnect logic by operating mode.
      - **CONFIG mode**:
        - (a) At boot, attempt auto-connect at most twice to the most-recently-bonded device.
        - (b) After pairing a new device, keep the resulting connection up — do not
              disconnect-then-reconnect.
        - (c) On any subsequent disconnect, do not auto-retry; wait for explicit user
              action via the "Connect" button.
      - **RUN mode**: retain auto-reconnect with a backoff schedule
        (1 s → 5 s → 15 s → 30 s → 60 s, then 60 s repeating).
      Eliminates UI intrusion in config mode while preserving transparent recovery
      in run mode.

- [ ] **Config-mode button behavior**: D10 is currently ignored during CONFIG mode runtime.
      - Short press → switch to RUN mode immediately (without reboot if possible).
      - Long press (≥800 ms) → reboot.
      - RUN-mode button behavior (short = cycle URL, long = save/sleep) stays unchanged.

## UI Polish

- [ ] Bonded-device panel: show device name when available, not only MAC address.
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
