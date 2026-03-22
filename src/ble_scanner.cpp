#include "ble_scanner.h"

#include <NimBLEDevice.h>

#include <vector>

#include "ble_keyboard.h"
#include "json_util.h"

namespace {

struct DiscoveredDevice {
  String name;
  String address;
  int rssi;
  bool bonded;
  bool seen;
  bool pairableNow;
};

static std::vector<DiscoveredDevice> gDevices;

} // namespace

namespace BleScanner {

void performScan() {
  gDevices.clear();

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setInterval(80);
  scan->setWindow(40);
  scan->setActiveScan(true);
  scan->clearResults();

  NimBLEScanResults results = scan->start(4, false);
  for (int i = 0; i < results.getCount(); i++) {
    NimBLEAdvertisedDevice d = results.getDevice(i);
    if (!d.haveName()) {
      continue;
    }

    String address = String(d.getAddress().toString().c_str());
    bool bonded = BLEKeyboard::isBondedAddress(address);
    bool pairableNow = !bonded && BLEKeyboard::isAdvertisedAsPairingMode(d);

    if (!bonded && !pairableNow) {
      continue;
    }

    DiscoveredDevice item;
    item.name = String(d.getName().c_str());
    item.address = address;
    item.rssi = d.getRSSI();
    item.bonded = bonded;
    item.seen = true;
    item.pairableNow = pairableNow;
    gDevices.push_back(item);
  }

  const int bondCount = NimBLEDevice::getNumBonds();
  for (int i = 0; i < bondCount; i++) {
    String bondedAddr = String(NimBLEDevice::getBondedAddress(i).toString().c_str());
    bool alreadyListed = false;
    for (DiscoveredDevice& device : gDevices) {
      if (device.address.equalsIgnoreCase(bondedAddr)) {
        device.bonded = true;
        alreadyListed = true;
        break;
      }
    }

    if (alreadyListed) {
      continue;
    }

    DiscoveredDevice item;
    item.address = bondedAddr;
    item.rssi = -127;
    item.bonded = true;
    item.seen = false;
    item.pairableNow = false;
    if (bondedAddr.equalsIgnoreCase(BLEKeyboard::preferredBondedAddress()) && BLEKeyboard::preferredBondedName().length() > 0) {
      item.name = BLEKeyboard::preferredBondedName();
    } else {
      item.name = "(bonded device)";
    }
    gDevices.push_back(item);
  }
}

String devicesJson() {
  String out = "[";
  for (size_t i = 0; i < gDevices.size(); i++) {
    if (i > 0) {
      out += ",";
    }
    out += "{\"name\":\"" + JsonUtil::escape(gDevices[i].name) + "\",";
    out += "\"address\":\"" + JsonUtil::escape(gDevices[i].address) + "\",";
    out += "\"rssi\":" + String(gDevices[i].rssi) + ",";
    out += "\"bonded\":";
    out += gDevices[i].bonded ? "true" : "false";
    out += ",\"seen\":";
    out += gDevices[i].seen ? "true" : "false";
    out += ",\"pairableNow\":";
    out += gDevices[i].pairableNow ? "true" : "false";
    out += "}";
  }
  out += "]";
  return out;
}

} // namespace BleScanner