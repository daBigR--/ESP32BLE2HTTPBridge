// =============================================================================
// ble_scanner.cpp — one-shot BLE scan with bond-state enrichment
// =============================================================================
//
// Performs a single active BLE scan and returns a JSON array describing all
// devices that are either currently advertising or previously bonded.
//
// Scan result enrichment strategy
// --------------------------------
// The raw scan only shows devices that are currently advertising.  But the
// web UI also needs to show bonded devices that are currently off or out of
// range so the user can unpair them.  The scan results are therefore merged
// with NimBLE's bond store:
//
//   • Scanned device is bonded     → include with seen=true, bonded=true.
//   • Scanned device is NOT bonded BUT is advertising in pairing mode
//                                  → include with seen=true, pairableNow=true.
//   • Scanned device is NOT bonded and NOT in pairing mode
//                                  → exclude (not relevant to the user).
//   • Bonded device not seen in scan → include with seen=false, rssi=-127
//                                    so the user can still unpair it.
//
// Active scan vs passive scan
// ---------------------------
// setActiveScan(true) sends scan request packets, which prompts devices to
// reply with their full advertising payload (including the device name).
// Without active scanning many BLE peripherals only send a minimal adv packet
// and we cannot read their name without connecting first.
// =============================================================================

#include "ble_scanner.h"

#include <NimBLEDevice.h>

#include <vector>

#include "ble_keyboard.h"
#include "json_util.h"

namespace {

// Represents one entry in the scan result list shown in the web UI.
struct DiscoveredDevice {
  String name;        // Friendly name from advertising data (or fallback)
  String address;     // Bluetooth address as lowercase hex string
  int    rssi;        // Signal strength in dBm; -127 = not currently seen
  bool   bonded;      // True if NimBLE has a stored bond for this address
  bool   seen;        // True if the device appeared in the current scan
  bool   pairableNow; // True if seen AND advertising as Limited Discoverable
};

// Results of the most recent scan.  Overwritten on each performScan() call.
static std::vector<DiscoveredDevice> gDevices;

} // namespace

namespace BleScanner {

// ---------------------------------------------------------------------------
// performScan — execute a 4-second active BLE scan
// ---------------------------------------------------------------------------
// Clears gDevices, performs the scan, then merges with the bond store.
void performScan() {
  gDevices.clear();

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setInterval(80);       // scan interval in units of 0.625 ms ≈ 50 ms
  scan->setWindow(40);         // scan window ≈ 25 ms (duty cycle = 50%)
  scan->setActiveScan(true);   // send scan-request to get full adv payload
  scan->clearResults();

  NimBLEScanResults results = scan->start(4, false); // 4 s, do not block BLE stack
  for (int i = 0; i < results.getCount(); i++) {
    NimBLEAdvertisedDevice d = results.getDevice(i);
    if (!d.haveName()) {
      continue; // skip anonymous devices — not keyboards
    }

    String address     = String(d.getAddress().toString().c_str());
    bool   bonded      = BLEKeyboard::isBondedAddress(address);
    bool   pairableNow = !bonded && BLEKeyboard::isAdvertisedAsPairingMode(d);

    // Only include devices that are relevant to the user.
    if (!bonded && !pairableNow) {
      continue;
    }

    DiscoveredDevice item;
    item.name        = String(d.getName().c_str());
    item.address     = address;
    item.rssi        = d.getRSSI();
    item.bonded      = bonded;
    item.seen        = true;
    item.pairableNow = pairableNow;
    gDevices.push_back(item);
  }

  // Merge bonded devices that were not seen in the scan.
  // This allows the user to unpair a keyboard that is currently off.
  const int bondCount = NimBLEDevice::getNumBonds();
  for (int i = 0; i < bondCount; i++) {
    String bondedAddr = String(NimBLEDevice::getBondedAddress(i).toString().c_str());
    bool alreadyListed = false;
    for (DiscoveredDevice& device : gDevices) {
      if (device.address.equalsIgnoreCase(bondedAddr)) {
        device.bonded = true;   // ensure the flag is set even if it was missed above
        alreadyListed = true;
        break;
      }
    }

    if (alreadyListed) { continue; }

    // Bond is in NVS but device was not advertising. Add a placeholder entry.
    DiscoveredDevice item;
    item.address     = bondedAddr;
    item.rssi        = -127; // sentinel: not seen
    item.bonded      = true;
    item.seen        = false;
    item.pairableNow = false;
    // Use the stored preferred name if available, otherwise show a generic label.
    if (bondedAddr.equalsIgnoreCase(BLEKeyboard::preferredBondedAddress()) &&
        BLEKeyboard::preferredBondedName().length() > 0) {
      item.name = BLEKeyboard::preferredBondedName();
    } else {
      item.name = "(bonded device)";
    }
    gDevices.push_back(item);
  }
}

// ---------------------------------------------------------------------------
// devicesJson — serialise gDevices to a JSON array
// ---------------------------------------------------------------------------
// Called immediately after performScan() by the /scan HTTP handler.
// All string values are escaped through JsonUtil to handle unusual characters.
String devicesJson() {
  String out = "[";
  for (size_t i = 0; i < gDevices.size(); i++) {
    if (i > 0) { out += ","; }
    out += "{\"name\":\""        + JsonUtil::escape(gDevices[i].name)    + "\",";
    out += "\"address\":\""     + JsonUtil::escape(gDevices[i].address) + "\",";
    out += "\"rssi\":"          + String(gDevices[i].rssi)              + ",";
    out += "\"bonded\":"        + (gDevices[i].bonded      ? "true" : "false") + ",";
    out += "\"seen\":"          + (gDevices[i].seen        ? "true" : "false") + ",";
    out += "\"pairableNow\":"   + (gDevices[i].pairableNow ? "true" : "false") + "}";
  }
  out += "]";
  return out;
}

} // namespace BleScanner