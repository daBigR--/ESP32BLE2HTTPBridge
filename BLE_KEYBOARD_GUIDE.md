# ESP32 BLE Keyboard Hub (Web GUI)

This project implements a **BLE Keyboard Hub with a browser GUI**. The ESP32 hosts a WiFi access point and web page where you can scan nearby BLE keyboards, pair/connect with one click, and see pressed keys in real time.

## Implementation: ESP32 as BLE Central + Web App

**What this app does**
- Hosts WiFi AP: `ESP32-Keyboard-Hub`
- Serves a local GUI at `http://192.168.4.1`
- Scans BLE devices and lets you choose which one to pair/connect
- Subscribes to HID input notifications
- Streams decoded key presses into the GUI

---

## How It Works

### How It Works

1. **Initialization**
   - ESP32 starts in AP mode (`ESP32-Keyboard-Hub`, password `12345678`)
   - Starts HTTP server on port 80
   - Initializes NimBLE as BLE central/client

2. **Scanning**
   - GUI button triggers active BLE scan (4 seconds)
   - Lists named BLE devices with RSSI and MAC address

3. **Connection & Pairing**
   - User clicks **Pair / Connect** in GUI
   - ESP32 connects as BLE central to selected keyboard
   - Keyboard pairing/bonding handled by BLE stack if required

4. **Service Discovery**
   - Looks for HID Service (UUID: 1812)
   - Finds input report characteristic (UUID: 2A4D, 2A4C, or custom)

5. **Input Handling**
   - Subscribes to keyboard input notifications
   - Parses HID keyboard reports (8-byte format)
   - Decodes modifiers and key codes
   - Shows live key log in web UI

### HID Keyboard Report Format

Every keyboard input sends an 8-byte report:

```
Byte 0: Modifier Keys
  Bit 0: Left Ctrl
  Bit 1: Left Shift
  Bit 2: Left Alt
  Bit 3: Left GUI (Windows/Command)
  Bit 4: Right Ctrl
  Bit 5: Right Shift
  Bit 6: Right Alt
  Bit 7: Right GUI

Byte 1: Reserved (always 0)

Bytes 2-7: Key Codes (up to 6 simultaneous keys)
  0x04 = A, 0x05 = B, etc.
  0x28 = ENTER, 0x29 = ESCAPE, etc.
  0x00 = No key
```

### Web GUI Flow

```
1) Connect phone/laptop to WiFi: ESP32-Keyboard-Hub
2) Open browser: http://192.168.4.1
3) Click "Scan Keyboards"
4) Click "Pair / Connect" for your keyboard
5) Press keys and watch live key log panel
```

---

## Setup & Configuration

### 1. Hardware Requirements
- ESP32 board (tested on Seeed XIAO ESP32S3, works on all ESP32 variants)
- BLE Keyboard (any HID-compliant Bluetooth keyboard)
- USB cable for programming and serial monitoring
- Phone or laptop with WiFi + browser to open the local GUI

### 2. Dependencies
Already configured in `platformio.ini`:
```ini
lib_deps =
    h2zero/NimBLE-Arduino @ ^1.4.0
```

### 3. Building & Uploading

**Build:**
```bash
platformio run
```

**Upload:**
```bash
platformio run --target upload
```

**Monitor Serial Output:**
```bash
platformio device monitor --baud 115200
```

After boot, serial prints the GUI URL.

Or use PlatformIO's built-in serial monitor in VS Code.

---

## Pairing Process

### First-Time Pairing
1. Put your BLE keyboard in pairing mode
2. Upload and boot ESP32
3. Connect your phone/laptop to WiFi `ESP32-Keyboard-Hub` (password `12345678`)
4. Open `http://192.168.4.1`
5. Click **Scan Keyboards**
6. Click **Pair / Connect** on the correct device
7. Press keys and verify they appear in the **Pressed Keys** panel

### Bonding and Reconnection
- Bonding information is stored in ESP32 NVS if keyboard uses bonding
- You can reconnect from the GUI at any time
- To forget bonded devices, add this to `setup()`:
  ```cpp
  NimBLEDevice::deleteAllBonds();  // Clears all stored bonds
  ```

---

## Customization Options

### Filter by Specific Keyboard
Modify the `MyScanCallback::onResult()` function to match your keyboard name:

```cpp
// Current: matches "keyboard" or "kb"
String lowerName = name;
lowerName.toLowerCase();
if (lowerName.indexOf("keyboard") >= 0 || 
   lowerName.indexOf("kb") >= 0) {

// Change to: match specific model
if (name.indexOf("Logitech K270") >= 0) {

// Or: match by MAC address
if (advertisedDevice->getAddress().toString() == "aa:bb:cc:dd:ee:ff") {
```

### Add Custom Key Processing
Modify `processKeyboardReport()` to respond to specific keys:

```cpp
for (int i = 0; i < 6; i++) {
    if (report->keyCodes[i] == 0x04) {  // 'A' key
        Serial.println(">>> A key pressed! Trigger action here");
        // digitalWrite(LED_PIN, HIGH);
    }
}
```

### Enable Power Saving
Uncomment in `setup()`:
```cpp
NimBLEDevice::setPower(ESP_PWR_LVL_P4, ESP_BLE_PWR_TYPE_DEFAULT);
```

---

## Troubleshooting

### Issue: "HID Service not found!"
**Cause:** The keyboard doesn't expose a standard HID service
**Solution:** 
- Add custom UUID to `possibleUUIDs[]` array
- Some gaming keyboards use custom GUIDs (check keyboard docs)

### Issue: Connects but doesn't receive input
**Cause:** Characteristic not found or notifications not enabled
**Solution:**
- Verify keyboard is actually a HID device
- Try reconnecting from GUI after putting keyboard in pairing mode again
- Add Serial.printf() debug statements to list all available characteristics

### Issue: Connection drops frequently
**Cause:** Signal interference or distance
**Solution:**
- Move ESP32 closer to keyboard
- Check WiFi isn't interfering on 2.4 GHz
- Increase scan interval/window for better connection stability

### Issue: Cannot pair with keyboard
**Cause:** Device doesn't support BLE (some keyboards are 2.4GHz RF only)
**Solution:**
- Verify keyboard is specifically BLE (check manual for "Bluetooth Low Energy")
- Some gaming keyboards are proprietary USB receivers, not BLE

---

## Key Files

- **[platformio.ini](platformio.ini)** - Project configuration with NimBLE dependency
- **[src/main.cpp](src/main.cpp)** - Complete BLE keyboard receiver implementation
- **[BLE_KEYBOARD_GUIDE.md](BLE_KEYBOARD_GUIDE.md)** - This guide

---

## Resources

- **NimBLE-Arduino Library**: https://github.com/h2zero/NimBLE-Arduino
- **HID Keyboard Report Format**: https://en.wikipedia.org/wiki/Human_interface_device
- **Bluetooth SIG HID Spec**: https://www.bluetooth.org/docman/handlers/downloaddoc.ashx?doc_id=295042
- **ESP32 Official Docs**: https://docs.espressif.com/projects/esp-idf/

---

## Next Steps

1. **Compile & Upload** the code using PlatformIO
2. **Pair with your BLE keyboard** following the pairing process above
3. **Monitor serial output** at 115200 baud to see key presses
4. **Customize** for your use case (add actions, store commands, trigger events based on keys, etc.)
5. **Enhance** with WiFi/MQTT to send keyboard input to cloud or other devices