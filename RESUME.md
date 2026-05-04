Resume note for Esp32BLETest

Saved state:
- Project files are persisted in the workspace under `Esp32BLETest`.
- This note is intentionally stored inside the workspace as `RESUME.md`.
- The baseline diagnostic project exists in `BLEPairBasicDiagnosticBaseline/`.

Current task:
- Verify Kobo BLE pairing using the clean `BLEPairBasicDiagnosticBaseline` project.
- Compare the baseline behavior against the app BLE flow in `src/ble_keyboard.cpp`.

Key state:
- Created `BLEPairBasicDiagnosticBaseline` with original NimBLE `BLEPairBasic` sample in `src/main.cpp`.
- Baseline project was built and uploaded from the workspace.
- Diagnostic focus is to confirm whether Kobo pairing fails in the clean sample or only in the app-specific flow.

Next step when resuming:
1. Check the serial monitor output from `BLEPairBasicDiagnosticBaseline`.
2. Note whether the Kobo device is discovered and whether connect/pair succeeds.
3. If baseline is successful, start isolating app-side BLE behavior in `src/ble_keyboard.cpp`.
4. If baseline also fails, collect the advertisement/connect logs and continue debugging the NimBLE connection sequence.
