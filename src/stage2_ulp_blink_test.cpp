// === STAGE 2 ULP TEST - REMOVE AFTER VALIDATION ===
// src/stage2_ulp_blink_test.cpp
//
// Stage 2 of 4 in battery-monitoring work.
// Validates ULP-FSM macro-based programming (ulp_process_macros_and_load /
// ulp_run) and the RTC slow-memory control channel on the XIAO ESP32-S3.
//
// Hardware target: GPIO21 (RTC IO index 21) — built-in USER LED, active LOW.
// During deep sleep in RUN mode the ULP blinks this pin for ~101 ms every
// 20 s.  On button-press wake the main core stops the ULP via two mechanisms:
//   1. RTC_SLOW_MEM[STOP_FLAG_ADDR] = 1  (ULP checks flag and halts early)
//   2. ulp_timer_stop()                  (prevents the timer from re-arming)
//
// Remove this file (and include/stage2_ulp_blink_test.h) after validation.

#include "stage2_ulp_blink_test.h"
#include "esp32s3/ulp.h"
#include "driver/rtc_io.h"
#include "soc/rtc_io_reg.h"
#include "esp_sleep.h"
#include "key_log.h"

// ---------------------------------------------------------------------------
// RTC slow memory layout
// ---------------------------------------------------------------------------
// Word 0  (STOP_FLAG_ADDR):  Control flag — 0 = run blinks, non-zero = halt.
//                             Written by main core; read by ULP each cycle.
//                             Read with & 0xFFFF (ULP is 16-bit).
// Word 1+ (ULP_PROG_ADDR):   ULP program image, loaded by ulp_process_macros_and_load.
//
// Layout diagram:
//   RTC_SLOW_MEM[0]  →  STOP_FLAG
//   RTC_SLOW_MEM[1]  →  ulp_program[0]   (first loaded instruction)
//   RTC_SLOW_MEM[2]  →  ulp_program[1]
//   ...
// ---------------------------------------------------------------------------
enum {
    STOP_FLAG_ADDR = 0,  // 0 = run blinks, non-zero = exit early this cycle
    RUN_COUNT_ADDR,      // ULP increments on every timer trigger
    BLINK_SENTINEL_ADDR, // ULP writes 0xAB here just before GPIO HIGH;
                         // if RUN_COUNT>0 but SENTINEL==0: ULP halted before GPIO write
                         // if SENTINEL==0xAB: ULP reached GPIO write (pad may still be wrong)
    ULP_PROG_ADDR        // ULP program image starts at word 3
};

// ---------------------------------------------------------------------------
// ulpBlinkInit
// ---------------------------------------------------------------------------
void ulpBlinkInit() {
    // ---------------------------------------------------------------------------
    // ULP program — defined in local scope to avoid "initializer element is not
    // constant" compiler errors.  Some M_ macros expand to function calls which
    // are not valid in a global-scope initializer.  See Neurotech Hub tutorial.
    //
    // ESP32-S3 ULP header notes (arduino-esp32 2.0.17):
    //   - No I_BGE macro.  Available R0-comparison branches: I_BL, I_BG, I_BE.
    //   - M_BL / M_BG each expand to TWO source entries (M_BRANCH placeholder +
    //     the real I_BL/I_BG instruction).  Only M_LABEL and M_BRANCH are not
    //     written into ULP memory; the real instruction IS loaded.
    //   - Stage-counter macros exist but are not needed here; R0 countdown is
    //     simpler and avoids the 3-argument I_STAGE_RST/I_STAGE_INC quirk.
    //
    // Design: R0 used as working register. R1 used as address register.
    //   Step a0: ULP increments RUN_COUNT_ADDR on every timer trigger so the
    //            main CPU can verify the FSM actually ran (read in ulpBlinkStop).
    //   Step a1: Stop-flag check — halt without blinking if STOP_FLAG_ADDR != 0.
    //   I_LDH / I_ST both operate on bits[15:0] of the 32-bit RTC_SLOW_MEM word,
    //   which matches what the main CPU writes with RTC_SLOW_MEM[x] = value.
    //
    // Source-array → loaded-instruction map
    // (M_LABEL and M_BRANCH are NOT written to ULP memory; everything else IS):
    //
    //  src[ 0]  I_MOVI(R1, RUN_COUNT_ADDR)    → loaded[0]
    //  src[ 1]  I_LDH(R0, R1, 0)              → loaded[1]
    //  src[ 2]  I_ADDI(R0, R0, 1)             → loaded[2]
    //  src[ 3]  I_ST(R0, R1, 0)               → loaded[3]
    //  src[ 4]  I_MOVI(R1, STOP_FLAG_ADDR)    → loaded[4]
    //  src[ 5]  I_LDH(R0, R1, 0)              → loaded[5]
    //  src[ 6]  M_BRANCH(2)                   → NOT loaded
    //  src[ 7]  I_BG(0, 0)                    → loaded[6]  if R0>0: jump→label 2
    //  src[ 8]  I_MOVI(R1, BLINK_SENTINEL_ADDR) → loaded[7]  sentinel before GPIO
    //  src[ 9]  I_MOVI(R0, 0xAB)              → loaded[8]
    //  src[10]  I_ST(R0, R1, 0)               → loaded[9]
    //  src[11]  I_WR_REG HIGH                 → loaded[10]
    //  src[12]  I_MOVI(R0, 27)                → loaded[11]
    //  src[13]  M_LABEL(1)                    → NOT loaded; label 1 → loaded[12]
    //  src[14]  I_DELAY(0xFFFF)               → loaded[12]
    //  src[15]  I_SUBI(R0, R0, 1)             → loaded[13]
    //  src[16]  M_BRANCH(1)                   → NOT loaded
    //  src[17]  I_BG(0, 0)                    → loaded[14] if R0>0: loop→label 1
    //  src[18]  I_WR_REG LOW                  → loaded[15]
    //  src[19]  M_LABEL(2)                    → NOT loaded; label 2 → loaded[16]
    //  src[20]  I_HALT()                      → loaded[16]
    //
    // Total: 17 loaded instructions = 68 bytes + 3 data words = 80 bytes total.
    // ---------------------------------------------------------------------------
    const ulp_insn_t ulp_program[] = {
        // a0) Increment run counter on every ULP timer trigger.
        //     Diagnostic: main CPU reads this on wake to verify ULP actually ran.
        I_MOVI(R1, RUN_COUNT_ADDR),
        I_LDH(R0, R1, 0),
        I_ADDI(R0, R0, 1),
        I_ST(R0, R1, 0),

        // a1) Load stop flag. If set, jump to HALT without blinking.
        I_MOVI(R1, STOP_FLAG_ADDR),
        I_LDH(R0, R1, 0),
        M_BG(2, 0),                   // if R0 > 0, jump to label 2 (I_HALT)

        // b0) Write sentinel 0xAB to BLINK_SENTINEL_ADDR BEFORE touching GPIO.
        //     On wake: if RUN_COUNT>0 but sentinel==0 → ULP stopped before here
        //              (stop flag branch or program crash).
        //              if sentinel==0xAB → ULP reached GPIO write path.
        I_MOVI(R1, BLINK_SENTINEL_ADDR),
        I_MOVI(R0, 0xAB),
        I_ST(R0, R1, 0),

        // b1) Drive GPIO21 LOW — active-LOW LED: 0 = ON.
        I_WR_REG(RTC_GPIO_OUT_REG,
                 (21 + RTC_GPIO_OUT_DATA_S),
                 (21 + RTC_GPIO_OUT_DATA_S),
                 0),

        // c) ~101 ms delay: R0 counts down from 27, each iteration ≈ 3.745 ms.
        I_MOVI(R0, 27),
        M_LABEL(1),
        I_DELAY(0xFFFF),
        I_SUBI(R0, R0, 1),
        M_BG(1, 0),                   // if R0 > 0, loop back to label 1

        // d) Drive GPIO21 HIGH — active-LOW LED: 1 = OFF.
        I_WR_REG(RTC_GPIO_OUT_REG,
                 (21 + RTC_GPIO_OUT_DATA_S),
                 (21 + RTC_GPIO_OUT_DATA_S),
                 1),

        // e) HALT — re-triggers via timer after 20 s.
        M_LABEL(2),
        I_HALT(),
    };

    // Zero RTC slow memory before loading so no stale data interferes.
    memset(RTC_SLOW_MEM, 0, CONFIG_ESP32S3_ULP_COPROC_RESERVE_MEM);

    size_t size = sizeof(ulp_program) / sizeof(ulp_insn_t);
    esp_err_t err = ulp_process_macros_and_load(ULP_PROG_ADDR, ulp_program, &size);
    if (err == ESP_OK) {
        KeyLog::add("Stage2: ULP program loaded OK");
    } else if (err == ESP_ERR_NO_MEM) {
        KeyLog::add("Stage2: ULP load FAILED - no memory");
    } else {
        KeyLog::add(String("Stage2: ULP load FAILED err=") + String((int)err));
    }

    // GPIO6 is NOT configured as RTC GPIO here.
    // rtc_gpio_init() switches the pin out of normal GPIO mode, which would
    // break the Stage 1 web UI LED test (which uses digitalWrite) in CONFIG mode.
    // RTC GPIO configuration is deferred to ulpBlinkArm(), which only runs in
    // RUN mode immediately before deep sleep.

    // ULP timer period: re-trigger the program every 20 seconds.
    esp_err_t wpErr = ulp_set_wakeup_period(0, 20 * 1000 * 1000);
    if (wpErr != ESP_OK) {
        KeyLog::add(String("Stage2: ulp_set_wakeup_period FAILED err=") + String((int)wpErr));
    }

    KeyLog::add(String("Stage2: init OK — prog_addr=") + String(ULP_PROG_ADDR)
                + String(" prog_words=") + String(size)
                + String(" wp_err=") + String((int)wpErr));
}

// ---------------------------------------------------------------------------
// ulpBlinkArm
// ---------------------------------------------------------------------------
// Clears the stop flag and starts the ULP.
// Call immediately before esp_deep_sleep_start() in RUN mode.
// ---------------------------------------------------------------------------
void ulpBlinkArm() {
    // Configure GPIO6 as RTC GPIO output now — deferred from ulpBlinkInit() to
    // avoid taking the pin out of normal GPIO mode in CONFIG mode.
    // Do NOT call rtc_gpio_hold_en: hold latches the pad physically and prevents
    // subsequent I_WR_REG writes from reaching the pad during deep sleep.
    int rtcIoNum = rtc_io_number_get(GPIO_NUM_21);
    KeyLog::add(String("Stage2: GPIO_NUM_21 rtc_io_num=") + String(rtcIoNum)
                + String(" RTC_GPIO_OUT_DATA_S=") + String((int)RTC_GPIO_OUT_DATA_S)
                + String(" bit=") + String(rtcIoNum + (int)RTC_GPIO_OUT_DATA_S));
    rtc_gpio_init(GPIO_NUM_21);
    rtc_gpio_set_direction(GPIO_NUM_21, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(GPIO_NUM_21, 1);  // start HIGH = LED OFF (active LOW)

    RTC_SLOW_MEM[STOP_FLAG_ADDR] = 0;
    esp_err_t runErr = ulp_run(ULP_PROG_ADDR);
    if (runErr != ESP_OK) {
        KeyLog::add(String("Stage2: ulp_run FAILED err=") + String((int)runErr));
    } else {
        KeyLog::add(String("Stage2: armed — stop_flag=")
                    + String(RTC_SLOW_MEM[STOP_FLAG_ADDR] & 0xFFFF)
                    + String(" run_count=")
                    + String(RTC_SLOW_MEM[RUN_COUNT_ADDR] & 0xFFFF));
    }
}

// ---------------------------------------------------------------------------
// ulpBlinkStop
// ---------------------------------------------------------------------------
// Sets the stop flag (so the ULP halts on its next cycle without blinking)
// and stops the wakeup timer (prevents further re-triggers).
// Call as early as possible on wake from deep sleep.
// ---------------------------------------------------------------------------
void ulpBlinkStop() {
    // Read diagnostics BEFORE touching RTC_SLOW_MEM (ulpBlinkInit zeros it).
    uint16_t runCount     = (uint16_t)(RTC_SLOW_MEM[RUN_COUNT_ADDR]     & 0xFFFF);
    uint16_t stopFlag     = (uint16_t)(RTC_SLOW_MEM[STOP_FLAG_ADDR]     & 0xFFFF);
    uint16_t sentinel     = (uint16_t)(RTC_SLOW_MEM[BLINK_SENTINEL_ADDR] & 0xFFFF);
    KeyLog::add(String("Stage2: wake diag — ulp_run_count=") + String(runCount)
                + String(" stop_flag=") + String(stopFlag)
                + String(" sentinel=0x") + String(sentinel, HEX));
    if (runCount == 0) {
        KeyLog::add("Stage2: WARNING — run_count=0, ULP may not have fired");
    } else if (sentinel != 0xAB) {
        KeyLog::add("Stage2: WARNING — ULP ran but did not reach GPIO write path (sentinel missing)");
    } else {
        KeyLog::add("Stage2: ULP reached GPIO write path — if no blink, fault is in I_WR_REG or RTC IO mux");
    }
    RTC_SLOW_MEM[STOP_FLAG_ADDR] = 1;
    ulp_timer_stop();
    KeyLog::add("Stage2: timer stopped");
}
// === END STAGE 2 ULP TEST ===
