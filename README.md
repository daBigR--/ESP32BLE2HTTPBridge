# ESP32 BLE Keyboard to HTTP Bridge

ESP32 firmware for a BLE keyboard hub with a built-in web GUI.

Current behavior:
- ESP32 acts as a BLE central/client for one keyboard.
- Device supports secure pairing and stores bonds in NVS.
- Keypresses are mapped to HTTP GET paths and sent to the currently selected base URL.
- Browser GUI is used for pairing, reconnect, WiFi config, multi-URL config, and key mapping.
- Two status LEDs are implemented:
   - D1: keyboard connection status with keypress double blink-off.
   - D3: WiFi connection status with HTTP 200 blink-off acknowledgment and URL-selection feedback.

## Current Project Status

This guide reflects the current code in this repository.

Implemented and working:
- Config mode and run mode split at boot.
- Persistent config store for WiFi networks, base URLs, selected URL index, and key mappings.
- Bonded-device aware scanner, pair/connect/unpair flows.
- Auto-connect to preferred bonded keyboard in run mode.
- HID input subscription on service 0x1812 and input report chars 0x2A22/0x2A4D.
- HTTP key dispatch queue with repeat filtering.
- LED signaling logic on D1 and D3, driven by a dedicated FreeRTOS task on Core 1.
- Add/edit/delete flows for both base URLs and key mappings in the web UI.

## Hardware

- Board: Seeed XIAO ESP32S3 (target environment in PlatformIO).
- BLE keyboard that supports BLE HID.
- One runtime/config button:
   - Connected to D10.
   - Active LOW to GND, using internal pull-up.
   - Used both for boot-time forced config mode and run-time short/double/long press actions.
- USB 5V presence sensing (battery vs USB power source):
   - D7 is fed by an external voltage divider from VBUS (5V).
   - Expected divider is 100k/100k so D7 sees about 2.5V when USB is present.
   - USB present -> D7 HIGH, USB absent -> D7 LOW.
- Optional status LEDs:
   - D1 for BLE status.
   - D3 for HTTP status.

### Hardware Wiring

| Signal | ESP32 Pin | Direction | Electrical Notes |
| --- | --- | --- | --- |
| Runtime/Config button | D10 | Input | Active LOW to GND, internal pull-up enabled (`INPUT_PULLUP`). |
| USB 5V sense (VBUS) | D7 | Input | Read through external divider from VBUS; expected 100k/100k, about 2.5V when USB is present. |
| BLE status LED | D1 | Output | Active HIGH. |
| WiFi/HTTP status LED | D3 | Output | Active HIGH. |

USB power detect logic used by firmware:
- D7 HIGH -> USB present.
- D7 LOW -> battery-only operation.

## Build and Upload

From project root:

```bash
platformio run --environment seeed_xiao_esp32s3
platformio run --target upload --environment seeed_xiao_esp32s3
```

If PlatformIO is not in PATH on Windows:

```powershell
& "C:\Users\<user>\.platformio\penv\Scripts\platformio.exe" run --environment seeed_xiao_esp32s3
& "C:\Users\<user>\.platformio\penv\Scripts\platformio.exe" run --target upload --environment seeed_xiao_esp32s3
```

Serial monitor:

```bash
platformio device monitor --baud 115200
```

## Boot Modes

Mode selection happens in startup:

- Config mode:
   - Entered if button D10 is held low for about 800 ms during boot, or if required run config is incomplete.
   - ESP32 starts SoftAP:
      - SSID: ESP32-Keyboard-Hub
      - Password: 12345678
   - GUI hosted on http://192.168.4.1

- Run mode:
   - Entered when saved config is valid.
   - ESP32 joins configured WiFi network(s) and processes mapped keypresses into HTTP GET calls.

Run config is considered valid only when all are present:
- At least one WiFi network.
- At least one base URL.
- At least one key mapping.
- Preferred bonded keyboard address.

## Runtime Button Behavior

The D10 button is active low and uses the internal pull-up.

In run mode:

- Short press:
   - Cycles to the next configured base URL.
   - URL change takes effect immediately for subsequent HTTP GETs.
   - D3 blinks `N` times, where `N = selectedUrlIndex + 1`.

- Double press:
   - Saves the currently selected URL index to NVS.
   - D3 performs a longer save-confirmation pulse.

- Long press (about 800 ms):
   - Puts the device into deep sleep regardless of power source.

- While sleeping:
   - Pressing the button wakes the device back into normal startup flow.

- Battery-only inactivity timeout:
   - Still sends the device to deep sleep automatically after the configured timeout.

## Pairing and Connection Model

Pairing and connecting are intentionally separate:

- Pair:
   - Allowed only when device advertises LE Limited Discoverable Mode (adv flag bit 0x01).
   - This avoids trying to pair when keyboard is not explicitly in pairing mode.

- Connect:
   - Allowed only for already bonded devices.

- Unpair:
   - Removes bond and clears preferred bonded device when needed.

- Auto-connect:
   - In run mode, firmware periodically attempts reconnect to preferred bonded keyboard.

## Key to HTTP Flow

1. Keyboard notification arrives.
2. First non-zero keycode from bytes 2..7 is used.
3. Keycode is queued.
4. Queue is drained in main loop with repeat filter (120 ms for same key).
5. If key has mapping and WiFi is connected, GET is sent to:

```
<selectedBaseUrl>/<mappedPath>
```

Slash handling is normalized so duplicate/missing slash combinations are corrected.

## LED Behavior (Current)

LED updates are handled in a dedicated 5 ms FreeRTOS task pinned to Core 1 so patterns remain stable even during blocking HTTP requests.

### Run Mode

#### D1 (BLE / keyboard)
- OFF when keyboard is disconnected.
- Steady ON when keyboard is connected.
- On each keypress, performs a double blink-off pattern:
   - 80 ms OFF, 80 ms ON, repeated 2 times.

#### D3 (WiFi / HTTP result)
- OFF when WiFi is not connected.
- Steady ON when WiFi is connected.
- On HTTP status 200 only, performs a single blink-off pulse:
   - 180 ms OFF.
- On URL selection change, performs `N` blink-off/on cycles where `N = selectedUrlIndex + 1`:
   - 150 ms OFF, 150 ms ON per cycle.
- On URL selection save via double press, performs a longer OFF pulse:
   - 600 ms OFF.
- No blink on GET start.
- No success blink for non-200 responses.

### Config Mode

- D1 and D3 alternate in anti-phase at 1 Hz (500 ms half-cycle).
- This indicates the device is in configuration mode.

## Web API Endpoints

BLE endpoints:
- GET /scan
- GET /pair?addr=<mac>&name=<name>
- GET /connect?addr=<mac>&name=<name>
- GET /unpair?addr=<mac>
- GET /disconnect
- GET /state

Config endpoints:
- GET /config
- GET /config/addurl?url=<baseUrl>
- GET /config/editurl?idx=<index>&url=<baseUrl>
- GET /config/delurl?idx=<index>
- GET /config/addwifi?ssid=<ssid>&pwd=<password>
- GET /config/delwifi?ssid=<ssid>
- GET /config/setmapping?key=<hexWithout0x>&path=<path>
- GET /config/delmapping?key=<hexWithout0x>
- GET /reboot
- GET /factory-reset

## GUI Workflow

1. Connect to AP ESP32-Keyboard-Hub in config mode.
2. Open http://192.168.4.1.
3. Add WiFi credentials.
4. Add one or more base URLs.
5. Scan and pair keyboard while keyboard is in pairing mode.
6. Capture key and assign path mappings.
7. Edit or delete saved URLs and mappings if needed.
8. Reboot to run mode.

## Data Persistence

Stored in Preferences namespace ble_cfg:
- WiFi networks (up to 8)
- Base URLs (up to 8)
- Selected base URL index
- Key mappings (up to 32)
- BLE bonds (managed by NimBLE stack)

Factory reset clears saved config and BLE bonds, then reboots.

## Main Source Map

- [src/main.cpp](src/main.cpp): app startup, mode selection, WiFi/server loop, LED logic.
- [src/ble_keyboard.cpp](src/ble_keyboard.cpp): secure BLE pairing/connect/subscription logic.
- [src/ble_scanner.cpp](src/ble_scanner.cpp): scan filtering and device list.
- [src/http_bridge.cpp](src/http_bridge.cpp): key queue and HTTP GET dispatch.
- [src/web_ble_api.cpp](src/web_ble_api.cpp): BLE control routes.
- [src/web_config_api.cpp](src/web_config_api.cpp): config routes and reboot/reset.
- [src/web_page.cpp](src/web_page.cpp): embedded web UI.
- [src/config_store.cpp](src/config_store.cpp): persistent configuration.

## Notes and Limits

- Transport uses HTTP GET only.
- One active keyboard connection model.
- One active base URL is used at a time, selected from the saved URL list.
- Pairing visibility depends on keyboard advertising pairing mode (flag 0x01).
- If WiFi is down, mapped HTTP requests are skipped and logged.

## License

Use and adapt as needed for your project.