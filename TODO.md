# TODO

## High Priority

- [x] **Device validation sweep**: all devices pairing, connecting, and subscribing
      correctly as of 2026-05-05.

- [x] **Review subscribed BLE services**: the current code subscribes to nearly all
      services published by the keyboard. Audit which services are actually needed for
      key capture and limit subscriptions to only those — over-subscribing may cause
      instability or unexpected behavior on some devices.
      Note: not really multiple services but some multiple key presses on single event,
	  solved by burst signatures.

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

- [x] **Bonded-device panel**: show device name when available, not only MAC address.
      (Observed: Boox connected successfully but bonded panel showed MAC-only.)
	  Cause: name was never persisted, added NVS saving and recoverying.

- [x] **Scan list**: collapse "Other Devices" section by default; expand only on user tap.

- [x] **Restructure config page**: clean the current layout, separate general setup: SSID, pwd,
      timeout, from BLE setup: scan, pair, bond, from actions setup: keys -> event mapping and 
      from device control.   Use tabbed interface.  Keep bonded device at the top all the time.

- [ ] **Show alert no bonded device** when applicable do not lose that section when empty. Apply
      the same concept to all empty sections.
	  
- [ ] **Confirm action visuals**: for certain actions currently with no visual feedback
      add some kind of visual aid and alert for failures!  
      *Inline button state*. The button you just clicked momentarily changes: "Save" → "Saved ✓"
	  for ~1.5 seconds → back to "Save." Or "Add Network" → grays out with a spinner → returns
	  enabled with the list updated. Confined to the element the user actually clicked, no screen
	  real estate stolen. Probably the cleanest pattern for save/add actions.
      *Result-as-feedback*. When the user adds something, the new entry appears in the list with
	  a brief highlight (a half-second background flash, then fade to normal). The fact that the
	  list visibly changed is the confirmation. No separate notification needed. Works especially
	  well when paired with the card-style list redesign you already have on your todo.

- [ ] **Make saved data lists better looking**: saved networks (SSID list), saved URLs, assigned
      mappings listing look too flat plain not clear, better visual deign needed.  Card-style list rendering for saved networks, URLs, and mappings — primary text + secondary info + right-anchored Edit/Remove actions, hover state, optional left-side type icon.
	  
- [ ] **Unify button labels**: change add, save timeout, add irl, assign, etc labels to make them consistent,
      arrange carefully so context allows clear understanding of action.

- [ ] ***Add test mode in config**: allow the user to press buttons and send them to the configured
      url + action mappings without exiting config mode.  Requires complete configuration: ssid/pwd, base URL, at least one action map and keyboard connected.  Then switching mode to AP + STA.  Also a special secttion to show status of test and exit from test option.

- [ ] **Add read-parse /koreader/event page**: besides the user being able to enter 
      an event name directly, implement parsing of KOReader HTTP Inspector page and show
	  a list of available events to pair with a certain key.  Easier than typing, less
	  error prone, self validating.  I think still keep manual entry for copy/paste use
	  or advanced user.  Add search box and allow for reversed flow, instead of user
	  pressing key then entering string to pair with it, user selects event and then
	  enters key to pair that event with.  Instead of parsing on the ESP32, have the ESP32
	  act as a CORS proxy — fetch the KOReader page and forward it to the browser. Then
	  JavaScript in your config UI does the parsing.

## Cleanup / Tech Debt

- [x] Delete `BLEPairBasicDiagnostic/` and `BLEPairBasicDiagnosticBaseline/` directories —
      these were temporary debugging aids and are no longer needed.

## Firmware

- [ ] **Battery monitoring**: add ADC voltage divider circuit so firmware can read and
      report battery level, and support low-battery behavior (e.g. warning in UI,
      auto-sleep).

- [ ] **Multi-bond support**: remove the single-bond-slot limitation. Allow multiple
      stored bonds and update auto-reconnect logic to know which device to favor
      (e.g. last used, user-set priority, or first seen in scan).

- [x] **Config-mode button behavior**: D10 is currently ignored during CONFIG mode runtime.
      - Short press → reboot (lands in RUN mode when config is complete).
      - RUN-mode button behavior (short = cycle URL, double = save) stays unchanged.

- [ ] **Eliminate 0000000000 bursts**: it seems they can be completely filtered out
      since no HID device should generate that as valid identifier.

## Maybes

- [ ] **Config-mode button ghost-trigger**: if the user holds D10 long enough to
      force CONFIG mode at boot, the button may still be held when `loop()` starts.
      `handleConfigButton()` would then arm on that tail-end of the press and fire
      a spurious reboot on release. A one-time "wait for release" guard at first
      call would fix this. Not observed in practice yet — revisit if it surfaces.

- [ ] **Per-mapping** "Test this mapping" button — fires the URL on demand using test
      mode's STA path, shows response inline.