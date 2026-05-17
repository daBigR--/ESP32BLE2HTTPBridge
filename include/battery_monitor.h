#pragma once
// === STAGE 3a ULP BATTERY MONITOR ===
// include/battery_monitor.h
//
// ULP-FSM battery monitor — hardcoded policy, no UI (Stage 3a).
// Main core calibrates ADC thresholds at boot, loads the ULP program, and
// starts it.  The ULP runs every 10 s regardless of main-core sleep state,
// reads ADC1_CH0 (D0/GPIO1 battery divider), and drives LED_BUILTIN/GPIO21 LED:
//   > 3.6 V battery : no flash
//   3.4–3.6 V       : single 100 ms flash
//   ≤ 3.4 V         : double flash (100 ms ON, 100 ms gap, 100 ms ON)
// LED is active LOW (LOW=ON). VUSB sensing is hardcoded to "always on battery" in 3a.

#include <Arduino.h>

// Load ULP program, calibrate ADC thresholds, configure RTC GPIO for D5,
// write RTC slow memory slots, and start the ULP.
// Call once in setup() after the normal boot init sequence.
// Safe to call unconditionally (both CONFIG and RUN mode).
void batteryMonitorInit();

// Set stop flag and stop ULP timer.
// Diagnostic / clean-shutdown use — not normally called.
void batteryMonitorStop();

// === STAGE 3a TEMPORARY TRACE — remove after validation ===
// Print latest ULP ADC raw reading and LED pattern to Serial.
// Call periodically from loop().
void batteryMonitorDiag();
// === END STAGE 3a TEMPORARY TRACE ===

// === END STAGE 3a ULP BATTERY MONITOR ===
