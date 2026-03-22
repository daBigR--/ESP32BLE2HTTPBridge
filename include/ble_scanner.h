#pragma once

#include <Arduino.h>

namespace BleScanner {

void performScan();

String devicesJson();

} // namespace BleScanner