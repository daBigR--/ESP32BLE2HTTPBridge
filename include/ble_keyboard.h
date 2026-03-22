#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

namespace BLEKeyboard {

using LogFn = void (*)(const String& line);
using KeyPressFn = void (*)(uint8_t keyCode);

void begin(LogFn logFn, KeyPressFn keyPressFn);

bool isBondedAddress(const String& address);
bool isAdvertisedAsPairingMode(NimBLEAdvertisedDevice& d);

void refreshPreferredBondedDevice();
const String& preferredBondedAddress();
const String& preferredBondedName();
void clearPreferredBondedDevice();

bool pairKeyboard(const String& address, const String& nameHint);
bool unpairKeyboard(const String& address);
bool connectToKeyboard(const String& address, const String& nameHint);
void disconnectKeyboard();
void maybeAutoConnectBondedKeyboard();
void syncConnectionState();

bool isConnected();
const String& connectedName();
const String& connectedAddress();
uint8_t lastKeyCode();

} // namespace BLEKeyboard
