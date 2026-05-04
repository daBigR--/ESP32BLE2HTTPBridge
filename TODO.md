# TODO

## UI Polish

- [ ] Bonded-device panel: show device name when available, not only MAC address.
      (Observed: Boox connected successfully but bonded panel showed MAC-only.)

- [ ] Scan list: collapse "Other Devices" section by default; expand only on user tap.

## Cleanup / Tech Debt

- [ ] Revert DIAG stay-connected-after-pair scaffold: remove the `subscribeToKeyboard()`
      call inside `pairKeyboard()` and restore the original disconnect-after-pair
      behavior. The DIAG was used to confirm the first encrypted session works;
      it's no longer needed now that auto-reconnect is fixed.

## Firmware

- [ ] Battery monitoring: add ADC voltage divider circuit so firmware can read and
      report battery level, and support low-battery behavior (e.g. warning in UI,
      auto-sleep).

- [ ] Multi-bond support: remove the single-bond-slot limitation. Allow multiple
      stored bonds and update auto-reconnect logic to know which device to favor
      (e.g. last used, user-set priority, or first seen in scan).
