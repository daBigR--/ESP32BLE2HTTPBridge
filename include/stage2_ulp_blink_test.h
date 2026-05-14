#pragma once
// === STAGE 2 ULP TEST - REMOVE AFTER VALIDATION ===
// include/stage2_ulp_blink_test.h
//
// Stage 2 of 4 in battery-monitoring work.
// Validates ULP-FSM macro-based programming and the RTC slow-memory
// control channel between main core and ULP.
//
// During deep sleep in RUN mode, the ULP blinks D5 (GPIO6) for ~100 ms
// approximately every 20 s.  The main core stops the ULP on button-press
// wake via a stop flag in RTC slow memory and ulp_timer_stop().
//
// Remove this file (and src/stage2_ulp_blink_test.cpp) after validation.

#include <Arduino.h>

// Load the ULP program and configure D5 as RTC GPIO output.
// Sets the 20 s wakeup timer period.  Does NOT start the ULP.
// Safe to call unconditionally on every boot (cold boot and wake alike).
void ulpBlinkInit();

// Clear the stop flag and start the ULP (ulp_run).
// Call immediately before esp_deep_sleep_start() in RUN mode.
void ulpBlinkArm();

// Set the stop flag (RTC_SLOW_MEM[STOP_FLAG_ADDR] = 1) and stop the timer.
// Call as early as possible on wake from deep sleep.
void ulpBlinkStop();
// === END STAGE 2 ULP TEST ===
