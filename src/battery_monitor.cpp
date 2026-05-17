// === STAGE 3a ULP BATTERY MONITOR ===
// src/battery_monitor.cpp
//
// ULP-FSM battery monitor for XIAO ESP32-S3.
//
// Hardware:
//   D0 / GPIO1  — battery voltage divider tap (2:1, 220k/220k).
//                 ADC1_CH0, RTC-capable.
//   LED_BUILTIN / GPIO21 — battery status LED, active LOW, RTC-capable.
//                 Sole owner: ULP (do NOT drive from main core after init).
//
// The main core calibrates ADC→mV conversion at boot, converts the fixed
// mV thresholds (3.6 V, 3.4 V) to per-chip raw counts, and writes them to
// RTC slow memory so the ULP program can compare against them without
// floating-point.
//
// Comparison trick (unsigned 16-bit ULP arithmetic):
//   I_SUBR R0, R3, R2   →  R0 = ADC_reading − threshold  (both ≤ 12-bit)
//   If ADC ≥ threshold : R0 is a small positive value, ≤ 32767
//   If ADC <  threshold : R0 wraps around → > 32767
//   M_BG(label, 32767) therefore branches iff ADC < threshold.
//
// Note on "not implemented yet" comments in esp32s3/ulp.h:
//   OPCODE_WR_REG, OPCODE_RD_REG and OPCODE_ADC all carry that comment, yet
//   I_WR_REG was confirmed to work on this chip in Stage 2 (GPIO21 blink).
//   The comment reflects stale IDF documentation — the hardware opcodes are
//   present and functional on the ESP32-S3 ULP-FSM.
//
// Note on rtc_gpio_hold_en:
//   hold_en latches the pad state and prevents I_WR_REG from reaching the
//   pad during deep sleep.  It is therefore intentionally NOT called here.
//   Stage 2 verified this finding.  The ULP drives the LED via I_WR_REG
//   every 10 s cycle without needing hold.

#include "battery_monitor.h"
#include "key_log.h"
#include "esp32s3/ulp.h"       // ULP FSM macros (I_ADC, I_WR_REG, M_BG, …)
#include "driver/rtc_io.h"     // rtc_gpio_init / set_direction / set_level
#include "driver/adc.h"        // adc1_config_width, adc1_config_channel_atten,
                               //   adc1_get_raw, adc1_ulp_enable
#include "esp_adc_cal.h"       // esp_adc_cal_characterize, raw_to_voltage
#include "soc/rtc_io_reg.h"    // RTC_GPIO_OUT_REG, RTC_GPIO_OUT_DATA_S
#include "soc/sens_reg.h"      // SENS_* (I_ADC uses internally)

// ---------------------------------------------------------------------------
// RTC slow memory layout
// Each slot is a 32-bit word; ULP reads/writes the lower 16 bits via I_LDH.
// ---------------------------------------------------------------------------
enum {
    RTC_STOP_FLAG = 0,       // 0 = run, non-zero = halt immediately
    RTC_VUSB_FLAG,           // 1 = on battery (hardcoded 3a), 0 = on USB
    RTC_RAW_THRESHOLD_LOW,   // raw ADC count for 3.6 V battery
    RTC_RAW_THRESHOLD_CRIT,  // raw ADC count for 3.4 V battery
    RTC_LATEST_RAW,          // ULP writes its last reading here
    RTC_LATEST_PATTERN,      // 0=none, 1=single, 2=double (ULP writes)
    RTC_ULP_PROG_ADDR        // ULP program image starts here
};

// ---------------------------------------------------------------------------
// LED constants — LED_BUILTIN / GPIO21, active LOW (LOW=ON, HIGH=OFF)
// On ESP32-S3, RTC IO index == GPIO number for GPIO0-GPIO21.
// ---------------------------------------------------------------------------
static constexpr gpio_num_t LED_GPIO    = GPIO_NUM_21;
static constexpr int        LED_RTC_IDX = 21;

// ---------------------------------------------------------------------------
// batteryMonitorInit
// ---------------------------------------------------------------------------
void batteryMonitorInit() {

    // -----------------------------------------------------------------------
    // 1. Main-core ADC calibration
    //    Configure ADC1_CH0, take one reading, convert to mV using the chip's
    //    built-in calibration efuse so K (mV-per-raw-count) is accurate.
    // -----------------------------------------------------------------------
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_12);

    int raw = adc1_get_raw(ADC1_CHANNEL_0);

    esp_adc_cal_characteristics_t adc_chars;
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12,
                             ADC_WIDTH_BIT_12, 1100 /* vref hint, overridden by efuse */,
                             &adc_chars);
    uint32_t pin_mV = esp_adc_cal_raw_to_voltage((uint32_t)raw, &adc_chars);

    // Guard: if the battery is disconnected or ADC is faulted, use safe defaults.
    // Default assumes roughly 0.806 mV/count (1800 mV at pin / 2234 raw ≈ 3.6 V).
    float K = 0.806f;
    if (raw > 10 && pin_mV > 10) {
        K = (float)pin_mV / (float)raw;
    }

    // Battery divider is 2:1 → pin voltage = battery voltage / 2.
    // 3.6 V battery → 1800 mV at pin.  3.4 V → 1700 mV at pin.
    int raw_3_6V = (int)(1800.0f / K);
    int raw_3_4V = (int)(1700.0f / K);

    Serial.printf("[BatMon] init: raw=%d pin_mV=%u K=%.4f raw_3.6V=%d raw_3.4V=%d\n",
                  raw, (unsigned)pin_mV, K, raw_3_6V, raw_3_4V);
    KeyLog::add(String("[BatMon] K=") + String(K, 4)
                + String(" raw_3.6V=") + String(raw_3_6V)
                + String(" raw_3.4V=") + String(raw_3_4V));

    // -----------------------------------------------------------------------
    // 2. Write RTC slow memory configuration slots
    // -----------------------------------------------------------------------
    RTC_SLOW_MEM[RTC_STOP_FLAG]          = 0;
    RTC_SLOW_MEM[RTC_VUSB_FLAG]          = 1;  // 3a: always "on battery"
    RTC_SLOW_MEM[RTC_RAW_THRESHOLD_LOW]  = (uint32_t)raw_3_6V;
    RTC_SLOW_MEM[RTC_RAW_THRESHOLD_CRIT] = (uint32_t)raw_3_4V;
    RTC_SLOW_MEM[RTC_LATEST_RAW]         = 0;
    RTC_SLOW_MEM[RTC_LATEST_PATTERN]     = 0;

    // -----------------------------------------------------------------------
    // 3. RTC GPIO setup for LED (LED_BUILTIN / GPIO21, active LOW)
    //    rtc_gpio_hold_en intentionally omitted — see file-level note.
    // -----------------------------------------------------------------------
    rtc_gpio_init(LED_GPIO);
    rtc_gpio_set_direction(LED_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(LED_GPIO, 1);   // LED OFF at init (active LOW: HIGH=OFF)

    // -----------------------------------------------------------------------
    // 4. Hand ADC1 control to ULP
    //    Must be called after adc1_config_* above.  After this point the main
    //    core must not call adc1_get_raw / analogRead on ADC1 channels.
    // -----------------------------------------------------------------------
    adc1_ulp_enable();

    // -----------------------------------------------------------------------
    // 5. ULP program — defined in local scope to avoid "initializer element is
    //    not constant" errors from macros that expand to function calls.
    //
    //    Registers:
    //      R0 — working / ADC result / delay countdown
    //      R1 — address register (RTC_SLOW_MEM word index)
    //      R2 — threshold value (copy from R0 after I_LDH)
    //      R3 — saved ADC reading (preserved across all comparisons/loops)
    //
    //    Label map:
    //      1 = HALT  (placed at end, branched to from multiple paths)
    //      2 = CHECK_CRIT  (compare against 3.4 V threshold)
    //      3 = DO_DOUBLE   (battery critical: double flash)
    //      4 = delay loop — single flash ON
    //      5 = delay loop — double flash first pulse ON
    //      6 = delay loop — double flash gap (OFF between pulses)
    //      7 = delay loop — double flash second pulse ON
    //
    //    Unconditional-jump idiom: after delay loop R0=0; M_BL(1,1) always
    //    branches (0 < 1) → HALT.  Used wherever a forward goto is needed.
    // -----------------------------------------------------------------------
    const ulp_insn_t ulp_program[] = {

        // ---- a) Stop flag check ----------------------------------------
        I_MOVI(R1, RTC_STOP_FLAG),
        I_LD(R0, R1, 0),
        M_BG(1, 0),                    // stop_flag > 0 → HALT

        // ---- b) VUSB flag: 1=battery (continue), 0=USB (halt) ----------
        I_MOVI(R1, RTC_VUSB_FLAG),
        I_LD(R0, R1, 0),
        M_BL(1, 1),                    // vusb_flag < 1 (== 0) → HALT

        // ---- c) ADC read: ADC1, channel 0 (GPIO1) → R0 ----------------
        I_ADC(R0, 0, 0),

        // ---- d) Store latest raw reading --------------------------------
        I_MOVI(R1, RTC_LATEST_RAW),
        I_ST(R0, R1, 0),

        // Copy ADC result to R3; R0 is now free for arithmetic.
        I_ADDI(R3, R0, 0),             // R3 = ADC reading

        // ---- e) Compare vs LOW threshold (3.6 V) -----------------------
        //  R0 = ADC - LOW_THRESHOLD (unsigned 16-bit wrap)
        //  ADC >= threshold → R0 ≤ 32767 → M_BG(2,32767) does NOT branch
        //  ADC <  threshold → R0 wraps > 32767 → M_BG(2,32767) branches
        I_MOVI(R1, RTC_RAW_THRESHOLD_LOW),
        I_LD(R0, R1, 0),              // R0 = LOW threshold
        I_ADDI(R2, R0, 0),            // R2 = LOW threshold (R0 free)
        I_SUBR(R0, R3, R2),           // R0 = ADC - LOW_THRESHOLD
        M_BG(2, 32767),               // ADC < LOW → CHECK_CRIT

        // Battery OK (≥ 3.6 V): write pattern 0, unconditional jump to HALT
        I_MOVI(R1, RTC_LATEST_PATTERN),
        I_MOVI(R0, 0),
        I_ST(R0, R1, 0),
        M_BL(1, 1),                   // R0=0, 0 < 1 → HALT

        // ---- CHECK_CRIT (label 2): compare vs CRIT threshold (3.4 V) --
        M_LABEL(2),
        I_MOVI(R1, RTC_RAW_THRESHOLD_CRIT),
        I_LD(R0, R1, 0),              // R0 = CRIT threshold
        I_ADDI(R2, R0, 0),            // R2 = CRIT threshold
        I_SUBR(R0, R3, R2),           // R0 = ADC - CRIT_THRESHOLD
        M_BG(3, 32767),               // ADC < CRIT → DO_DOUBLE

        // Battery LOW (3.4–3.6 V): pattern 1, single flash --------------
        I_MOVI(R1, RTC_LATEST_PATTERN),
        I_MOVI(R0, 1),
        I_ST(R0, R1, 0),
        // LED ON (active LOW: drive 0)
        I_WR_REG(RTC_GPIO_OUT_REG,
                 LED_RTC_IDX + RTC_GPIO_OUT_DATA_S,
                 LED_RTC_IDX + RTC_GPIO_OUT_DATA_S, 0),
        // ~101 ms ON (27 × I_DELAY(0xFFFF) ≈ 27 × 3.74 ms at 17.5 MHz ULP clock)
        I_MOVI(R0, 27),
        M_LABEL(4),
        I_DELAY(0xFFFF),
        I_SUBI(R0, R0, 1),
        M_BG(4, 0),
        // LED OFF (active LOW: drive 1)
        I_WR_REG(RTC_GPIO_OUT_REG,
                 LED_RTC_IDX + RTC_GPIO_OUT_DATA_S,
                 LED_RTC_IDX + RTC_GPIO_OUT_DATA_S, 1),
        // R0=0 after loop → unconditional jump to HALT
        M_BL(1, 1),

        // ---- DO_DOUBLE (label 3): pattern 2, double flash --------------
        M_LABEL(3),
        I_MOVI(R1, RTC_LATEST_PATTERN),
        I_MOVI(R0, 2),
        I_ST(R0, R1, 0),
        // Pulse 1: LED ON ~101 ms (active LOW: drive 0)
        I_WR_REG(RTC_GPIO_OUT_REG,
                 LED_RTC_IDX + RTC_GPIO_OUT_DATA_S,
                 LED_RTC_IDX + RTC_GPIO_OUT_DATA_S, 0),
        I_MOVI(R0, 27),
        M_LABEL(5),
        I_DELAY(0xFFFF),
        I_SUBI(R0, R0, 1),
        M_BG(5, 0),
        // Gap: LED OFF ~101 ms (active LOW: drive 1)
        I_WR_REG(RTC_GPIO_OUT_REG,
                 LED_RTC_IDX + RTC_GPIO_OUT_DATA_S,
                 LED_RTC_IDX + RTC_GPIO_OUT_DATA_S, 1),
        I_MOVI(R0, 27),
        M_LABEL(6),
        I_DELAY(0xFFFF),
        I_SUBI(R0, R0, 1),
        M_BG(6, 0),
        // Pulse 2: LED ON ~101 ms (active LOW: drive 0)
        I_WR_REG(RTC_GPIO_OUT_REG,
                 LED_RTC_IDX + RTC_GPIO_OUT_DATA_S,
                 LED_RTC_IDX + RTC_GPIO_OUT_DATA_S, 0),
        I_MOVI(R0, 27),
        M_LABEL(7),
        I_DELAY(0xFFFF),
        I_SUBI(R0, R0, 1),
        M_BG(7, 0),
        // LED OFF — fall through to HALT (active LOW: drive 1)
        I_WR_REG(RTC_GPIO_OUT_REG,
                 LED_RTC_IDX + RTC_GPIO_OUT_DATA_S,
                 LED_RTC_IDX + RTC_GPIO_OUT_DATA_S, 1),

        // ---- HALT (label 1) — re-triggers via timer after 10 s --------
        M_LABEL(1),
        I_HALT(),
    };

    // -----------------------------------------------------------------------
    // 6. Load ULP program, set wakeup period, start ULP
    // -----------------------------------------------------------------------
    size_t size = sizeof(ulp_program) / sizeof(ulp_insn_t);
    esp_err_t err = ulp_process_macros_and_load(RTC_ULP_PROG_ADDR,
                                                ulp_program, &size);
    if (err != ESP_OK) {
        KeyLog::add(String("[BatMon] ULP load FAILED err=") + String((int)err));
        return;
    }
    KeyLog::add(String("[BatMon] ULP loaded OK words=") + String((int)size)
                + String(" prog_addr=") + String(RTC_ULP_PROG_ADDR));

    esp_err_t wpErr = ulp_set_wakeup_period(0, 10UL * 1000UL * 1000UL);  // 10 s
    if (wpErr != ESP_OK) {
        KeyLog::add(String("[BatMon] wakeup period FAILED err=") + String((int)wpErr));
    }

    ulp_run(RTC_ULP_PROG_ADDR);
    KeyLog::add("[BatMon] ULP started");
}

// ---------------------------------------------------------------------------
// === STAGE 3a TEMPORARY TRACE — remove after validation ===
// batteryMonitorDiag — print latest ULP readings to Serial
// ---------------------------------------------------------------------------
void batteryMonitorDiag() {
    uint32_t stop       = RTC_SLOW_MEM[RTC_STOP_FLAG]           & 0xFFFF;
    uint32_t vusb       = RTC_SLOW_MEM[RTC_VUSB_FLAG]           & 0xFFFF;
    uint32_t thr_low    = RTC_SLOW_MEM[RTC_RAW_THRESHOLD_LOW]   & 0xFFFF;
    uint32_t thr_crit   = RTC_SLOW_MEM[RTC_RAW_THRESHOLD_CRIT]  & 0xFFFF;
    uint32_t latest_raw     = RTC_SLOW_MEM[RTC_LATEST_RAW]      & 0xFFFF;
    uint32_t latest_pattern = RTC_SLOW_MEM[RTC_LATEST_PATTERN]  & 0xFFFF;
    Serial.printf("[BatMon] stop=%u vusb=%u thr_low=%u thr_crit=%u raw=%u pat=%u\n",
                  (unsigned)stop, (unsigned)vusb,
                  (unsigned)thr_low, (unsigned)thr_crit,
                  (unsigned)latest_raw, (unsigned)latest_pattern);
}
// === END STAGE 3a TEMPORARY TRACE ===

// ---------------------------------------------------------------------------
// batteryMonitorStop
// ---------------------------------------------------------------------------
void batteryMonitorStop() {
    RTC_SLOW_MEM[RTC_STOP_FLAG] = 1;
    ulp_timer_stop();
    KeyLog::add("[BatMon] stopped");
}

// === END STAGE 3a ULP BATTERY MONITOR ===
