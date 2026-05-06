// ============================================================
//  Makita Battery Monitor / Unlock / Omega Lock Utility
//
//  Normal mode    : full battery scan + auto-unlock (default)
//  Omega lock mode: sets charger lock nibble (nybble 34) non-zero
//
//  Mode select :  bridge GPIO0 -> GPIO1  =>  OMEGA LOCK  (red pulse)
//                 both pins open         =>  SCAN/UNLOCK (white pulse)
//
//  Target : Waveshare RP2040 Zero (primary) + other Arduino boards
//  Build  : PlatformIO + Adafruit NeoPixel + OneWire2 (vendored fork)
//
//  License: Provided as-is, no warranty. Interacting with battery
//           pack firmware can permanently brick a pack. Use at your
//           own risk.
// ============================================================

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "OneWire2.h"  // vendored fork of OneWire with tweaks for Makita timing

#if defined(ARDUINO_ARCH_RP2040)
  #include <pico/stdlib.h>
  #include <hardware/watchdog.h>
#endif

// ─── Firmware identity ────────────────────────────────────────
#define FIRMWARE_VERSION "1.5.0"

// ─── Pin definitions ─────────────────────────────────────────
static constexpr uint8_t PIN_ONEWIRE      = 6;
static constexpr uint8_t PIN_ENABLE       = 8;
static constexpr uint8_t NEOPIXEL_OUT_PIN = 16;
static constexpr uint8_t NUM_PIXELS       = 1;
static constexpr uint8_t PIN_MODE_OUT     = 0;
static constexpr uint8_t PIN_MODE_IN1     = 1;   // Omega lock mode (bridge GPIO0-GPIO1)

// On RP2040 PIN_MODE_OUT drives HIGH and the inputs use INPUT_PULLDOWN, so a
// bridge pulls them HIGH. On other boards we drive LOW with INPUT_PULLUP.
#if defined(ARDUINO_ARCH_RP2040)
  static constexpr bool MODE_PIN_ACTIVE_HIGH = true;
#else
  static constexpr bool MODE_PIN_ACTIVE_HIGH = false;
#endif

// ─── Protocol constants ───────────────────────────────────────
static constexpr uint16_t BUS_BYTE_GAP_US   = 90;   // inter-byte gap on 1-Wire bus (OBI original value)
static constexpr uint16_t BUS_SETTLE_US     = 100;  // settle before reading response bytes
static constexpr uint16_t BUS_ENABLE_MS     = 150;  // bus power-up settle before reset
static constexpr uint16_t BUS_PRESENCE_MS   = 150;  // presence poll settle time
static constexpr uint16_t POWERCYCLE_OFF_MS = 100;  // bus off time
static constexpr uint16_t POWERCYCLE_ON_MS  = 150;  // bus on settle after power cycle

// Inter-step delays for the frame-write / store sequence.
static constexpr uint16_t WRITE_ARM_DELAY_MS    = 30;
static constexpr uint16_t WRITE_FRAME_DELAY_MS  = 30;
static constexpr uint16_t WRITE_STORE_DELAY_MS  = 30;
static constexpr uint16_t TESTMODE_SETTLE_MS    = 30;

// Polling — chip goes bus-silent for ~3 s during frame commit then wakes with data ready.
// 10 s failsafe gives 3x headroom. 25 ms poll interval catches recovery quickly.
static constexpr uint32_t NO_RESPONSE_TIMEOUT_MS = 10000;
static constexpr uint16_t NO_RESPONSE_POLL_MS    = 25;

// ─── Type 5 (F0513) constants ─────────────────────────────────
// F0513 needs a long power-up settle and a wider reset gap than other types.
static constexpr uint16_t TYPE5_POWERUP_MS    = 420;  // power-up settle
static constexpr uint16_t TYPE5_RESET_GAP_US  = 420;  // post-reset gap
static constexpr uint8_t  TYPE5_CMD_SESSION   = 0x99; // enters model-read session
static constexpr uint8_t  TYPE5_CMD_MODEL     = 0x31; // queries model bytes
static constexpr uint8_t  TYPE5_CMD_TEMP      = 0x52; // reads cell temperature

// ─── Type 6 constants ─────────────────────────────────────────
static constexpr uint8_t  TYPE6_VOLT_READ     = 0xD4; // voltage read prefix
static constexpr float    TEMP6_A             = 9323.0f;
static constexpr float    TEMP6_B             = -40.0f;

// ─── Protocol commands ────────────────────────────────────────
static constexpr uint8_t CMD_BASIC_INFO[]     = { 0xAA, 0x00 };
static constexpr uint8_t CMD_MODEL[]          = { 0xDC, 0x0C };
static constexpr uint8_t CMD_TESTMODE_ENTER[] = { 0xD9, 0x96, 0xA5 };
static constexpr uint8_t CMD_TESTMODE_EXIT[]  = { 0xD9, 0xFF, 0xFF };
static constexpr uint8_t CMD_RESET_ERRORS[]   = { 0xDA, 0x04 };
static constexpr uint8_t CMD_CC_F0[]          = { 0xF0, 0x00 }; // arms frame write path
static constexpr uint8_t CMD_STORE[]          = { 0x55, 0xA5 };
static constexpr uint8_t FRAME_WRITE_OPCODE   = 0x0F;
static constexpr uint8_t FRAME_WRITE_PAD      = 0x00;
static constexpr uint8_t BASIC_INFO_LEN       = 32;
static constexpr uint8_t TYPE_PROBE_MAGIC     = 0x06;
static constexpr float   TEMP_INVALID         = -999.0f;

static constexpr uint8_t  HEALTH_SCALE_EXTENDED_CODES[] = { 26, 28, 40, 50 };
static constexpr size_t   HEALTH_SCALE_EXTENDED_COUNT   =
    sizeof(HEALTH_SCALE_EXTENDED_CODES) / sizeof(HEALTH_SCALE_EXTENDED_CODES[0]);
static constexpr uint16_t HEALTH_SCALE_STANDARD = 600;
static constexpr uint16_t HEALTH_SCALE_EXTENDED = 1000;
static constexpr float    HEALTH_MAX            = 4.0f;
static constexpr float    SOC_DIVISOR           = 2880.0f;

// ─── Scan timing ─────────────────────────────────────────────
static constexpr uint16_t POLL_INTERVAL_MS      = 200;

// ─── Unlock / lock attempt limits ────────────────────────────
static constexpr uint8_t  UNLOCK_MAX_CYCLES     = 6;
static constexpr uint8_t  LOCK_MAX_ATTEMPTS     = 6;

// ─── LED parameters ──────────────────────────────────────────
static constexpr uint8_t  LED_BRIGHTNESS_MAX    = 80;
static constexpr uint8_t  LED_FLASH_COUNT       = 3;
static constexpr uint16_t LED_FLASH_ON_MS       = 200;
static constexpr uint16_t LED_FLASH_OFF_MS      = 100;
static constexpr uint16_t LED_PULSE_PERIOD_MS        = 2000;  // full breathe cycle (ms)
static constexpr uint8_t  LED_PULSE_INTERVAL_MS      = 20;    // how often core1 updates the pulse
static constexpr float    IMBALANCE_THRESHOLD_V      = 0.300f;// cell diff above this = physical warning
static constexpr uint16_t IMBALANCE_BLIP_INTERVAL_MS = 1000;  // orange blip every N ms when imbalanced
static constexpr uint16_t IMBALANCE_BLIP_ON_MS       = 80;    // blip duration

// ─── Watchdog ────────────────────────────────────────────────
// Must exceed NO_RESPONSE_TIMEOUT_MS (10 s). Set to 12 s.
static constexpr uint32_t WATCHDOG_TIMEOUT_MS = 12000;

// ─── Lock cause flags (bitfield) ─────────────────────────────
// Each bit represents one confirmed cause of a battery not charging.
static constexpr uint16_t LF_FC   = 0x0001;  // nybble 40 (failure code) != 0
static constexpr uint16_t LF_CS0  = 0x0002;  // CS0 mismatch (nybbles 0-15)
static constexpr uint16_t LF_CS1  = 0x0004;  // CS1 mismatch (nybbles 16-31)
static constexpr uint16_t LF_CS2  = 0x0008;  // CS2 mismatch (nybbles 32-40)
static constexpr uint16_t LF_B0   = 0x0010;  // byte 0 invalid (not 0xF1 or 0x50)
static constexpr uint16_t LF_B1   = 0x0020;  // byte 1 invalid (not 0x26, 0x36, or 0x31)
static constexpr uint16_t LF_N34  = 0x0040;  // nybble 34 != 0 (charger lock nibble)
static constexpr uint16_t LF_B18  = 0x0080;  // byte 18 == 0x00 (must not be zero)
static constexpr uint16_t LF_B19  = 0x0100;  // byte 19 == 0x00 (must not be zero)

// ─── Return codes / types ─────────────────────────────────────
enum BasicInfoResult { BASIC_INFO_NO_RESPONSE = 0, BASIC_INFO_OK = 1, BASIC_INFO_PRE_TYPE0 = -1 };
enum BusResult       { BUS_OK = 0, BUS_NO_PRESENCE = 1 };

// Failure codes stored in nybble 40 of the basic-info frame.
enum FailureCode : uint8_t {
    FC_OK         = 0,
    FC_OVERLOADED = 1,
    FC_WARNING    = 5,
    FC_DEAD       = 15,
};

enum class BatteryType : int8_t {
    UNKNOWN = -1,
    T0      = 0,
    T2      = 2,
    T3      = 3,
    T5      = 5,
    T6      = 6,
};

static inline int  batt_type_to_int(BatteryType t) { return static_cast<int>(t); }
static inline bool is_type_023(BatteryType t) {
    return t == BatteryType::T0 || t == BatteryType::T2 || t == BatteryType::T3;
}
static inline bool is_type_56(BatteryType t) {
    return t == BatteryType::T5 || t == BatteryType::T6;
}

// ─── Data structures ─────────────────────────────────────────
struct RawBasicInfo {
    uint8_t  batt_type, capacity, failure_code, overdischarge, overload;
    uint16_t cycles;
    bool     cell_failure;
    uint8_t  checksum_n[3];
    uint8_t  aux_checksum_n[2];
};

struct HealthData {
    float    rating;
    uint16_t overload_count;
    uint8_t  od_count;
    uint32_t charge_level;
};

static constexpr uint8_t MAX_CELLS = 10;

struct CellVoltages {
    float   v[MAX_CELLS];
    uint8_t n;
    bool    valid;
};

struct VoltageReadResult {
    float        vpack;
    CellVoltages cells;
    float        t_cell;
    float        t_mosfet;
    bool         ok;
};

static inline VoltageReadResult voltage_result_init() {
    VoltageReadResult vr{};
    vr.t_cell   = TEMP_INVALID;
    vr.t_mosfet = TEMP_INVALID;
    return vr;
}

struct BatteryInfo {
    char         model[8];
    uint8_t      rom_id[8];
    bool         rom_id_valid;
    BatteryType  type;
    RawBasicInfo raw;
    HealthData   health;
    float        temperature_c;
    float        temperature_mosfet_c;
    uint8_t      cell_count;
    float        capacity_ah;
    bool         locked;
    bool         checksums_ok[3];
    bool         aux_checksums_ok[2];
    uint16_t     lock_causes;     // bitfield of LF_* flags
};

// ─── Globals ──────────────────────────────────────────────────
static OneWire           g_ow(PIN_ONEWIRE);
static Adafruit_NeoPixel g_pixel(NUM_PIXELS, NEOPIXEL_OUT_PIN, NEO_GRB + NEO_KHZ800);
static bool              g_charger_arm_issued = false;
static bool              g_in_testmode        = false;

// ─── Watchdog helpers ────────────────────────────────────────
static inline void wdt_kick() {
#if defined(ARDUINO_ARCH_RP2040)
    watchdog_update();
#endif
}

static inline void wdt_begin() {
#if defined(ARDUINO_ARCH_RP2040)
    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);
#endif
}

static void safe_delay(uint32_t ms) {
    const uint32_t chunk = 500;
    while (ms > chunk) { delay(chunk); wdt_kick(); ms -= chunk; }
    delay(ms);
    wdt_kick();
}

// ─── Core 1 — LED animation flags ────────────────────────────
static volatile bool g_pulse_active    = false;
static volatile bool g_pulse_scan_mode = true;
static volatile bool g_imbalance       = false;

// ─── NeoPixel ────────────────────────────────────────────────
struct Colour { uint8_t r, g, b; };
static constexpr Colour COL_OFF    = {  0,                  0,                  0 };
static constexpr Colour COL_GREEN  = {  0, LED_BRIGHTNESS_MAX,                  0 };
static constexpr Colour COL_YELLOW = { LED_BRIGHTNESS_MAX, 60,                  0 };
static constexpr Colour COL_ORANGE = { LED_BRIGHTNESS_MAX, 40,                  0 };
static constexpr Colour COL_BLUE   = {  0,                  0, LED_BRIGHTNESS_MAX };
static constexpr Colour COL_RED    = { LED_BRIGHTNESS_MAX,  0,                  0 };
static constexpr Colour COL_PURPLE = { LED_BRIGHTNESS_MAX,  0, LED_BRIGHTNESS_MAX };
static constexpr Colour COL_WHITE  = { LED_BRIGHTNESS_MAX, LED_BRIGHTNESS_MAX, LED_BRIGHTNESS_MAX };

static volatile uint8_t g_result_r = 0;
static volatile uint8_t g_result_g = 0;
static volatile uint8_t g_result_b = 0;

static void led_set(Colour c) {
    g_pixel.setPixelColor(0, g_pixel.Color(c.r, c.g, c.b));
    g_pixel.show();
}
static void led_off()    { led_set(COL_OFF);    }
static void led_yellow() { led_set(COL_YELLOW); }
static void led_blue()   { led_set(COL_BLUE);   }
static void led_purple() { led_set(COL_PURPLE); }

static void led_flash(Colour c) {
    g_pulse_active = false;          // pause core1 during flash
    for (uint8_t i = 0; i < LED_FLASH_COUNT; i++) {
        led_set(c); safe_delay(LED_FLASH_ON_MS);
        led_off();  safe_delay(LED_FLASH_OFF_MS);
    }
    led_set(c);  // stays on after final flash
}

static void led_pulse(Colour c) {
    constexpr uint32_t HALF = LED_PULSE_PERIOD_MS / 2;
    uint32_t t = millis() % LED_PULSE_PERIOD_MS;
    uint8_t  v = (t < HALF)
        ? (uint8_t)(t * LED_BRIGHTNESS_MAX / HALF)
        : (uint8_t)((LED_PULSE_PERIOD_MS - t) * LED_BRIGHTNESS_MAX / HALF);
    Colour sc = {
        (uint8_t)(v * c.r / LED_BRIGHTNESS_MAX),
        (uint8_t)(v * c.g / LED_BRIGHTNESS_MAX),
        (uint8_t)(v * c.b / LED_BRIGHTNESS_MAX),
    };
    led_set(sc);
}

static inline void led_result(bool ok) { led_flash(ok ? COL_GREEN : COL_RED); }

// ─── Mode detection ───────────────────────────────────────────
// Mode 0 = SCAN/UNLOCK  (both pins open)
// Mode 1 = OMEGA LOCK   (GPIO0-GPIO1 bridged)
enum DeviceMode { MODE_SCAN = 0, MODE_LOCK = 1 };

static inline bool mode_pin_active(uint8_t pin) {
    return digitalRead(pin) == (MODE_PIN_ACTIVE_HIGH ? HIGH : LOW);
}

static DeviceMode mode_read() {
    if (mode_pin_active(PIN_MODE_IN1)) return MODE_LOCK;
    return MODE_SCAN;
}

// ─── Bus control ─────────────────────────────────────────────
static void bus_enable(uint16_t ms = BUS_ENABLE_MS) { digitalWrite(PIN_ENABLE, HIGH); safe_delay(ms); }
static void bus_disable()                            { digitalWrite(PIN_ENABLE, LOW);  }

static void power_cycle_bus(uint16_t off_ms = POWERCYCLE_OFF_MS, uint16_t on_ms = POWERCYCLE_ON_MS) {
    bus_disable();
    g_charger_arm_issued = false;
    safe_delay(off_ms);
    bus_enable(on_ms);
}

static bool battery_present() {
    digitalWrite(PIN_ENABLE, HIGH);
    safe_delay(BUS_PRESENCE_MS);
    bool p = (g_ow.reset() == 1);
    digitalWrite(PIN_ENABLE, LOW);
    return p;
}

// ─── Low-level 1-Wire transactions ───────────────────────────
static BusResult ow_transaction(bool managed, uint8_t rom_prefix,
                                const uint8_t *cmd, uint8_t cmd_len,
                                uint8_t *rsp,       uint8_t rsp_len) {
    if (managed) bus_enable();
    if (!g_ow.reset()) { if (managed) bus_disable(); return BUS_NO_PRESENCE; }
    g_ow.write(rom_prefix, 0);
    if (rom_prefix == 0x33) {
        for (uint8_t i = 0; i < 8; i++) { delayMicroseconds(BUS_BYTE_GAP_US); rsp[i] = g_ow.read(); }
        rsp += 8;
    }
    for (uint8_t i = 0; i < cmd_len; i++) { delayMicroseconds(BUS_BYTE_GAP_US); g_ow.write(cmd[i], 0); }
    delayMicroseconds(BUS_SETTLE_US);
    for (uint8_t i = 0; i < rsp_len; i++) { delayMicroseconds(BUS_BYTE_GAP_US); rsp[i] = g_ow.read(); }
    if (managed) bus_disable();
    return BUS_OK;
}

static BusResult cmd_cc    (const uint8_t *c, uint8_t cl, uint8_t *r, uint8_t rl) { return ow_transaction(true,  0xCC, c, cl, r, rl); }
static BusResult cmd_cc_raw(const uint8_t *c, uint8_t cl, uint8_t *r, uint8_t rl) { return ow_transaction(false, 0xCC, c, cl, r, rl); }

// Two safe wrappers for the 0x33 (read-ROM) command.
// _with_rom : caller wants the 8 ROM bytes + rsp_len response bytes.
// _no_rom   : ROM bytes are discarded internally; rsp_len may be 0.
static BusResult cmd_33_with_rom(const uint8_t *c, uint8_t cl,
                                  uint8_t rom_out[8],
                                  uint8_t *rsp, uint8_t rsp_len) {
    uint8_t buf[8 + 32] = {0};
    BusResult res = ow_transaction(true, 0x33, c, cl, buf, rsp_len);
    if (res == BUS_OK) {
        memcpy(rom_out, buf, 8);
        if (rsp && rsp_len) memcpy(rsp, buf + 8, rsp_len);
    }
    return res;
}

static BusResult cmd_33_no_rom(const uint8_t *c, uint8_t cl) {
    uint8_t throwaway[8] = {0};
    return ow_transaction(true, 0x33, c, cl, throwaway, 0);
}

// ─── Nybble helpers ───────────────────────────────────────────
static uint8_t nybble_get(const uint8_t *d, uint8_t n) {
    return (n % 2 == 0) ? (d[n/2] & 0x0F) : ((d[n/2] >> 4) & 0x0F);
}
static void nybble_set(uint8_t *d, uint8_t n, uint8_t v) {
    v &= 0x0F;
    if (n % 2 == 0) d[n/2] = (d[n/2] & 0xF0) | v;
    else            d[n/2] = (d[n/2] & 0x0F) | (v << 4);
}
static uint8_t nybble_pair(const uint8_t *d, uint8_t hi, uint8_t lo) {
    return (uint8_t)((nybble_get(d, hi) << 4) | nybble_get(d, lo));
}
static uint8_t checksum_calc(const uint8_t *d, uint8_t s, uint8_t e) {
    uint8_t sum = 0;
    for (uint8_t i = s; i <= e; i++) sum += nybble_get(d, i);
    return sum & 0x0F;
}
static inline uint16_t le16(const uint8_t *b) { return (uint16_t)b[0] | ((uint16_t)b[1] << 8); }

// ─── Utility ─────────────────────────────────────────────────
static void print_sep() { Serial.println(F("-------------------------------------------------")); }

static void print_checksums(bool ok0, bool ok1, bool ok2) {
    Serial.print(ok0 ? F("0-15=ok") : F("0-15=bad"));
    Serial.print(' ');
    Serial.print(ok1 ? F("16-31=ok") : F("16-31=bad"));
    Serial.print(' ');
    Serial.println(ok2 ? F("32-40=ok") : F("32-40=bad"));
}

static void print_failure_code(uint8_t fc) {
    switch (fc) {
        case FC_OK:         Serial.println(F("0 - OK")); break;
        case FC_OVERLOADED: Serial.println(F("1 - Overloaded")); break;
        case FC_WARNING:    Serial.println(F("5 - Warning")); break;
        case FC_DEAD:       Serial.println(F("15 - Dead?")); break;
        default: Serial.print(fc); Serial.println(F(" - Invalid Error Code")); break;
    }
}

// Prints each active lock cause with aligned brackets.
// Only prints if there are causes — nothing printed if clean.
static void print_lock_causes(const BatteryInfo &info, const uint8_t d[BASIC_INFO_LEN]) {
    if (info.lock_causes == 0) return;
    Serial.println(F("Lock causes    :"));
    char buf[48];
    if (info.lock_causes & LF_FC) {
        snprintf(buf, sizeof(buf), "  Nybble 40 = %u      (must be 0)", info.raw.failure_code);
        Serial.println(buf);
    }
    if (info.lock_causes & LF_CS0) Serial.println(F("  CS0              (mismatch nybbles 0-15)"));
    if (info.lock_causes & LF_CS1) Serial.println(F("  CS1              (mismatch nybbles 16-31)"));
    if (info.lock_causes & LF_CS2) Serial.println(F("  CS2              (mismatch nybbles 32-40)"));
    if (info.lock_causes & LF_B0) {
        snprintf(buf, sizeof(buf), "  Byte 0 = 0x%02X    (invalid BMS family)", d[0]);
        Serial.println(buf);
    }
    if (info.lock_causes & LF_B1) {
        snprintf(buf, sizeof(buf), "  Byte 1 = 0x%02X    (invalid variant)", d[1]);
        Serial.println(buf);
    }
    if (info.lock_causes & LF_N34) {
        snprintf(buf, sizeof(buf), "  Nybble 34 = %u     (must be 0)", nybble_get(d, 34));
        Serial.println(buf);
    }
    if (info.lock_causes & LF_B18) Serial.println(F("  Byte 18 = 0x00   (must not be zero)"));
    if (info.lock_causes & LF_B19) Serial.println(F("  Byte 19 = 0x00   (must not be zero)"));
}

static void print_frame(const uint8_t d[BASIC_INFO_LEN],
                        const __FlashStringHelper *label = nullptr) {
    if (!label) label = F("Frame          : ");
    Serial.print(label);
    for (uint8_t i = 0; i < BASIC_INFO_LEN; i++) {
        if (i > 0 && i % 11 == 0) { Serial.println(); Serial.print(F("                 ")); }
        if (d[i] < 0x10) Serial.print('0');
        Serial.print(d[i], HEX);
        if (i < BASIC_INFO_LEN - 1) Serial.print(' ');
    }
    Serial.println();
}

// ─── Test mode ───────────────────────────────────────────────
static BusResult enter_testmode() {
    uint8_t r[1] = {0};
    BusResult res = cmd_cc(CMD_TESTMODE_ENTER, sizeof(CMD_TESTMODE_ENTER), r, 1);
    if (res == BUS_OK) { g_in_testmode = true; safe_delay(TESTMODE_SETTLE_MS); }
    return res;
}
static void exit_testmode() {
    if (!g_in_testmode) return;
    uint8_t r[1] = {0};
    cmd_cc(CMD_TESTMODE_EXIT, sizeof(CMD_TESTMODE_EXIT), r, 1);
    g_in_testmode = false;
    safe_delay(TESTMODE_SETTLE_MS);
}

// ─── Basic info read ─────────────────────────────────────────
static BasicInfoResult read_basic_info(uint8_t d[BASIC_INFO_LEN]) {
    if (cmd_cc(CMD_BASIC_INFO, sizeof(CMD_BASIC_INFO), d, BASIC_INFO_LEN) != BUS_OK)
        return BASIC_INFO_NO_RESPONSE;
    uint8_t orv = 0, andv = 0xFF;
    for (uint8_t i = 0; i < BASIC_INFO_LEN; i++) { orv |= d[i]; andv &= d[i]; }
    if (orv  == 0x00) return BASIC_INFO_NO_RESPONSE;
    if (andv == 0xFF) return BASIC_INFO_PRE_TYPE0;
    return BASIC_INFO_OK;
}

// ─── ROM ID read ─────────────────────────────────────────────
// Returns true on bus success. On failure rom_out is zero-filled; callers
// MUST treat it as invalid (a zero ROM looks like a valid type-5 ROM).
static bool read_rom_id(uint8_t rom_out[8]) {
    uint8_t rsp[BASIC_INFO_LEN] = {0};
    BusResult res = cmd_33_with_rom(CMD_BASIC_INFO, sizeof(CMD_BASIC_INFO),
                                     rom_out, rsp, BASIC_INFO_LEN);
    if (res != BUS_OK) { memset(rom_out, 0, 8); return false; }
    return true;
}

// ─── Raw bus session helper ──────────────────────────────────
static bool raw_cc_session(uint16_t powerup_ms, uint16_t reset_gap_us, uint16_t byte_gap_us,
                           bool send_skip_rom,
                           const uint8_t *cmd, uint8_t cmd_len,
                           uint8_t *rsp, uint8_t rsp_len) {
    digitalWrite(PIN_ENABLE, HIGH);
    if (powerup_ms) safe_delay(powerup_ms);
    if (!g_ow.reset()) { digitalWrite(PIN_ENABLE, LOW); return false; }
    delayMicroseconds(reset_gap_us);
    if (send_skip_rom) { g_ow.write(0xCC, 0); delayMicroseconds(byte_gap_us); }
    for (uint8_t i = 0; i < cmd_len; i++) { g_ow.write(cmd[i], 0); delayMicroseconds(byte_gap_us); }
    for (uint8_t i = 0; i < rsp_len; i++) { rsp[i] = g_ow.read();  delayMicroseconds(byte_gap_us); }
    digitalWrite(PIN_ENABLE, LOW);
    return true;
}

// ─── Model read ──────────────────────────────────────────────
static bool model_looks_valid(const char *m) {
    return strlen(m) >= 4 && ((m[0]=='B' && m[1]=='L') || (m[0]=='D' && m[1]=='C'));
}

static bool read_model(char out[8]) {
    uint8_t rsp[16] = {0};
    if (cmd_cc(CMD_MODEL, sizeof(CMD_MODEL), rsp, 16) != BUS_OK) { out[0] = '\0'; return false; }
    for (uint8_t i = 0; i < 7; i++) {
        uint8_t c = rsp[i];
        if (c < 0x20 || c >= 0x7F) { out[i] = '\0'; break; }
        out[i] = (char)c;
    }
    out[7] = '\0';
    return model_looks_valid(out);
}

static bool read_model_type5(char out[8]) {
    uint8_t dummy[1] = {0};
    bool ok = raw_cc_session(0, TYPE5_RESET_GAP_US, BUS_BYTE_GAP_US,
                             /*skip_rom=*/true, &TYPE5_CMD_SESSION, 1, dummy, 0);
    if (!ok) { snprintf(out, 8, "BL18xx"); return false; }
    safe_delay(10);

    uint8_t r[2] = {0};
    ok = raw_cc_session(0, TYPE5_RESET_GAP_US, BUS_BYTE_GAP_US,
                        /*skip_rom=*/false, &TYPE5_CMD_MODEL, 1, r, 2);
    if (!ok) { snprintf(out, 8, "BL18xx"); return false; }

    if ((r[0] != 0xFF || r[1] != 0xFF) && (r[0] != 0x00 || r[1] != 0x00)) {
        snprintf(out, 8, "BL%02X%02X", r[1], r[0]);
        return true;
    }
    snprintf(out, 8, "BL18xx");
    return false;
}

// ─── Universal no-response recovery ──────────────────────────
static bool wait_for_response() {
    Serial.print(F("  [No response] - waiting"));
    uint32_t started = millis();
    while (millis() - started < NO_RESPONSE_TIMEOUT_MS) {
        wdt_kick();
        power_cycle_bus();
        if (g_ow.reset() == 1) {
            uint32_t elapsed = millis() - started;
            Serial.print(F(" OK (")); Serial.print(elapsed); Serial.println(F(" ms)"));
            return true;
        }
        Serial.print(F("."));
    }
    Serial.print(F(" TIMEOUT (")); Serial.print(NO_RESPONSE_TIMEOUT_MS); Serial.println(F(" ms)"));
    return false;
}

static bool poll_until_response(uint8_t frame[BASIC_INFO_LEN]) {
    Serial.print(F("  Waiting"));
    uint32_t started = millis();
    while (millis() - started < NO_RESPONSE_TIMEOUT_MS) {
        wdt_kick();
        if (read_basic_info(frame) == BASIC_INFO_OK) {
            uint32_t elapsed = millis() - started;
            Serial.print(F(" (")); Serial.print(elapsed); Serial.println(F(" ms)"));
            return true;
        }
        Serial.print(F("."));
        power_cycle_bus();
    }
    Serial.print(F(" TIMEOUT (")); Serial.print(NO_RESPONSE_TIMEOUT_MS); Serial.println(F(" ms)"));
    return false;
}

// ─── Battery type detection ───────────────────────────────────
static int8_t detect_battery_type_raw(const uint8_t rom[8], const uint8_t d[BASIC_INFO_LEN],
                                       bool lock_only) {
    if (rom[3] < 100) return lock_only ? -1 : (int8_t)BatteryType::T5;
    if (d[17]  == 30) return lock_only ? -1 : (int8_t)BatteryType::T6;

    { static const uint8_t c[] = {0xDC,0x0B}; uint8_t r[17]={0};
      if (cmd_cc(c,sizeof(c),r,17)==BUS_OK && r[16]==TYPE_PROBE_MAGIC) return 0; }

    { if (enter_testmode()==BUS_OK) {
          static const uint8_t c[] = {0xDC,0x0A}; uint8_t r[17]={0};
          BusResult br = cmd_cc(c,sizeof(c),r,17);
          exit_testmode();
          power_cycle_bus();  // quiet settle after testmode exit — do NOT poll here
          if (br==BUS_OK && r[16]==TYPE_PROBE_MAGIC) return 2; } }

    { static const uint8_t c[] = {0xD4,0x2C,0x00,0x02}; uint8_t r[3]={0};
      if (cmd_cc(c,sizeof(c),r,3)==BUS_OK && r[2]==TYPE_PROBE_MAGIC) return 3; }

    return lock_only ? -1 : 0;
}

static BatteryType detect_battery_type(const uint8_t rom[8], const uint8_t d[BASIC_INFO_LEN]) {
    return (BatteryType)detect_battery_type_raw(rom, d, false);
}
static int8_t lock_detect_type(const uint8_t rom[8], const uint8_t d[BASIC_INFO_LEN]) {
    return detect_battery_type_raw(rom, d, true);
}

// ─── Parse / derive ───────────────────────────────────────────
static void parse_basic_info(const uint8_t d[BASIC_INFO_LEN], RawBasicInfo &o) {
    o.batt_type    = nybble_pair(d, 22, 23);
    o.capacity     = nybble_pair(d, 32, 33);
    o.failure_code = nybble_get(d, 40);
    o.overdischarge = nybble_pair(d, 48, 49);
    o.overload      = nybble_pair(d, 50, 51);
    o.cell_failure  = (nybble_get(d, 44) & 0x04) != 0;
    o.cycles = ((uint16_t)(nybble_get(d,52)&0x1)<<12)
             | ((uint16_t) nybble_get(d,53)       <<8)
             | ((uint16_t) nybble_get(d,54)       <<4)
             |  (uint16_t) nybble_get(d,55);
    o.checksum_n[0]     = nybble_get(d, 41);
    o.checksum_n[1]     = nybble_get(d, 42);
    o.checksum_n[2]     = nybble_get(d, 43);
    o.aux_checksum_n[0] = nybble_get(d, 62);
    o.aux_checksum_n[1] = nybble_get(d, 63);
}

static void derive_battery_info(const uint8_t d[BASIC_INFO_LEN], BatteryInfo &info) {
    const RawBasicInfo &r = info.raw;
    info.cell_count  = (r.batt_type < 13) ? 4 : (r.batt_type < 30) ? 5 : 10;
    info.capacity_ah = r.capacity / 10.0f;
    info.checksums_ok[0] = (checksum_calc(d, 0,  15) == r.checksum_n[0]);
    info.checksums_ok[1] = (checksum_calc(d, 16, 31) == r.checksum_n[1]);
    info.checksums_ok[2] = (checksum_calc(d, 32, 40) == r.checksum_n[2]);
    info.aux_checksums_ok[0] = (checksum_calc(d, 44, 47) == r.aux_checksum_n[0]);
    info.aux_checksums_ok[1] = (checksum_calc(d, 48, 61) == r.aux_checksum_n[1]);

    // Build lock cause bitfield — every known cause of a battery not charging
    info.lock_causes = 0;
    if (r.failure_code != 0)                                          info.lock_causes |= LF_FC;
    if (!info.checksums_ok[0])                                        info.lock_causes |= LF_CS0;
    if (!info.checksums_ok[1])                                        info.lock_causes |= LF_CS1;
    if (!info.checksums_ok[2])                                        info.lock_causes |= LF_CS2;
    if (d[0] != 0xF1 && d[0] != 0x50)                                info.lock_causes |= LF_B0;
    if (d[1] != 0x26 && d[1] != 0x36 && d[1] != 0x31)               info.lock_causes |= LF_B1;
    if (nybble_get(d, 34) != 0)                                       info.lock_causes |= LF_N34;
    if (d[18] == 0x00)                                                info.lock_causes |= LF_B18;
    if (d[19] == 0x00)                                                info.lock_causes |= LF_B19;

    info.locked = (info.lock_causes != 0);
}

static inline void refresh_info_from_frame(const uint8_t d[BASIC_INFO_LEN], BatteryInfo &info) {
    parse_basic_info(d, info.raw);
    derive_battery_info(d, info);
}

// ─── Health calculations ──────────────────────────────────────
static float calc_health_type56(const RawBasicInfo &r) {
    int16_t f_ol = max((int16_t)((int16_t)r.overload    - 29), (int16_t)0);
    int16_t f_od = max((int16_t)(35 - (int16_t)r.overdischarge), (int16_t)0);
    float   dmg  = r.cycles + r.cycles * (f_ol + f_od) / 32.0f;
    uint16_t scale = HEALTH_SCALE_STANDARD;
    for (size_t i = 0; i < HEALTH_SCALE_EXTENDED_COUNT; i++)
        if (r.capacity == HEALTH_SCALE_EXTENDED_CODES[i]) { scale = HEALTH_SCALE_EXTENDED; break; }
    return max(0.0f, HEALTH_MAX - dmg / scale);
}

static float calc_health_type023(uint16_t raw, uint8_t cap) {
    if (cap == 0) return 0.0f;
    float ratio = (float)raw / cap;
    return (ratio > 80.0f) ? HEALTH_MAX : max(0.0f, ratio / 10.0f - 5.0f);
}

static float calc_health_damage_rating(const uint8_t d[BASIC_INFO_LEN]) {
    uint8_t dmg = (nybble_get(d, 46) >> 1) & 0x07;
    if (dmg < 3)  return HEALTH_MAX;
    if (dmg >= 7) return 0.0f;
    return max(0.0f, HEALTH_MAX - (float)(dmg - 2));
}

// ─── Secondary reads (types 0, 2, 3) ─────────────────────────
static bool cc_probe(const uint8_t *cmd, uint8_t *rsp, uint8_t rsp_len) {
    if (cmd_cc(cmd, 4, rsp, rsp_len) != BUS_OK) return false;
    return rsp[rsp_len - 1] == TYPE_PROBE_MAGIC;
}

static uint8_t type023_idx(BatteryType t) {
    return (t == BatteryType::T2) ? 1 : (t == BatteryType::T3) ? 2 : 0;
}

static uint16_t read_health_raw(BatteryType t) {
    static const uint8_t cmds[3][4] = {
        {0xD4,0x50,0x01,0x02}, {0xD6,0x04,0x05,0x02}, {0xD6,0x38,0x02,0x02}
    };
    if (!is_type_023(t)) return 0;
    uint8_t r[3]={0};
    return cc_probe(cmds[type023_idx(t)], r, 3) ? le16(r) : 0;
}

static uint8_t read_od_count(BatteryType t) {
    static const uint8_t cmds[3][4] = {
        {0xD4,0xBA,0x00,0x01}, {0xD6,0x8D,0x05,0x01}, {0xD6,0x09,0x03,0x01}
    };
    if (!is_type_023(t)) return 0;
    uint8_t r[2]={0};
    return cc_probe(cmds[type023_idx(t)], r, 2) ? r[0] : 0;
}

static uint16_t read_overload_count(BatteryType t) {
    switch (t) {
        case BatteryType::T0: {
            static const uint8_t c[] = {0xD4,0x8D,0x00,0x07}; uint8_t r[8]={0};
            if (!cc_probe(c,r,8)) return 0;
            uint16_t a  = ((uint16_t)(r[0]>>6)&0x03)|((uint16_t)r[1]<<2);
            uint16_t b  =  (uint16_t) r[3]           |(((uint16_t)r[4]&0x03)<<8);
            uint16_t c2 = ((uint16_t)(r[5]>>4))      |(((uint16_t)r[6]&0x3F)<<4);
            return a+b+c2;
        }
        case BatteryType::T2: {
            static const uint8_t c[] = {0xD6,0x5F,0x05,0x07}; uint8_t r[8]={0};
            return cc_probe(c,r,8) ? (uint16_t)(r[0]+r[2]+r[3]+r[5]+r[6]) : 0;
        }
        case BatteryType::T3: {
            static const uint8_t c[] = {0xD6,0x5B,0x03,0x04}; uint8_t r[6]={0};
            return cc_probe(c,r,6) ? (uint16_t)(r[0]+r[2]+r[3]) : 0;
        }
        default: return 0;
    }
}

static uint32_t read_charge_level() {
    static const uint8_t cmd[] = {0xD7,0x19,0x00,0x04};
    uint8_t r[5]={0};
    if (!cc_probe(cmd,r,5)) return 0;
    return (uint32_t)r[0]|((uint32_t)r[1]<<8)|((uint32_t)r[2]<<16)|((uint32_t)r[3]<<24);
}

// ─── Temperature (type 6 only) ───────────────────────────────
static float read_temperature_type6() {
    static const uint8_t CMD_TEMP6[] = {0xD2};
    bus_enable();
    uint8_t r[1] = {0};
    bool ok = g_ow.reset() != 0;
    if (ok) {
        delayMicroseconds(BUS_BYTE_GAP_US);
        g_ow.write(CMD_TEMP6[0], 0);
        delayMicroseconds(BUS_SETTLE_US);
        r[0] = g_ow.read();
    }
    bus_disable();
    return ok ? (TEMP6_A + TEMP6_B * r[0]) / 100.0f : TEMP_INVALID;
}

// ─── Voltage reads ────────────────────────────────────────────
static bool read_voltages_type023(VoltageReadResult &vr, uint8_t ncells) {
    static const uint8_t ext[]    = {0xD7,0x00,0x00,0xFF};
    static const uint8_t short_[] = {0xD7,0x00,0x00,0x0C};
    uint8_t r[27]={0};
    uint8_t n = min(ncells, (uint8_t)5);
    bool ok = (cmd_cc(ext, sizeof(ext), r, 27)==BUS_OK) && (le16(&r[0])>0);
    if (ok) {
        vr.vpack = le16(&r[0]) / 1000.0f;
        for (uint8_t i = 0; i < n; i++) vr.cells.v[i] = le16(&r[2+i*2]) / 1000.0f;
        vr.cells.n = n; vr.cells.valid = true;
        uint16_t rc = le16(&r[14]), rm = le16(&r[16]);
        vr.t_cell   = (rc != 0 && rc != 0xFFFF) ? rc / 100.0f : TEMP_INVALID;
        vr.t_mosfet = (rm != 0 && rm != 0xFFFF) ? rm / 100.0f : TEMP_INVALID;
        vr.ok = true;
        return true;
    }
    uint8_t s[13]={0};
    if (cmd_cc(short_,sizeof(short_),s,13)!=BUS_OK || s[12]!=TYPE_PROBE_MAGIC) return false;
    vr.vpack = le16(&s[0]) / 1000.0f;
    for (uint8_t i = 0; i < n; i++) vr.cells.v[i] = le16(&s[2+i*2]) / 1000.0f;
    vr.cells.n = n; vr.cells.valid = true;
    vr.ok = true;
    return true;
}

static bool type5_raw_cc(const uint8_t *data, uint8_t dlen, uint8_t *rsp, uint8_t rlen) {
    return raw_cc_session(TYPE5_POWERUP_MS, TYPE5_RESET_GAP_US, BUS_BYTE_GAP_US,
                          /*skip_rom=*/true, data, dlen, rsp, rlen);
}

static bool read_voltages_type5(VoltageReadResult &vr, uint8_t ncells) {
    static const uint8_t CELL_CMDS[] = { 0x31, 0x32, 0x33, 0x34, 0x35 };
    uint8_t ign[2] = {0};
    if (!type5_raw_cc(CMD_CC_F0, sizeof(CMD_CC_F0), ign, 0)) return false;
    type5_raw_cc(CMD_CC_F0, sizeof(CMD_CC_F0), ign, 0);
    type5_raw_cc(&CELL_CMDS[0], 1, ign, 2);

    uint8_t n = min(ncells, (uint8_t)5);
    float   pack = 0.0f;
    uint8_t any  = 0;
    uint8_t r[2];
    for (uint8_t i = 0; i < n; i++) {
        r[0] = r[1] = 0;
        if (!type5_raw_cc(&CELL_CMDS[i], 1, r, 2)) continue;
        uint16_t mv = le16(r);
        if (mv > 0 && mv != 0xFFFF) { vr.cells.v[i] = mv / 1000.0f; pack += vr.cells.v[i]; any = 1; }
    }
    vr.cells.n = n; vr.cells.valid = any != 0; vr.vpack = pack;

    r[0] = r[1] = 0;
    if (type5_raw_cc(&TYPE5_CMD_TEMP, 1, r, 2)) {
        uint16_t traw = le16(r);
        if (traw > 0 && traw != 0xFFFF) vr.t_cell = traw / 100.0f;
    }
    vr.ok = vr.cells.valid;
    return vr.ok;
}

static bool read_voltages_type6(VoltageReadResult &vr, uint8_t ncells) {
    uint8_t ign[1]={0}, r[20]={0};
    bus_enable();
    { static const uint8_t c[]={0x10,0x21}; cmd_cc_raw(c,sizeof(c),ign,0); safe_delay(10); }
    bool ok = g_ow.reset() != 0;
    if (ok) {
        delayMicroseconds(BUS_BYTE_GAP_US);
        g_ow.write(TYPE6_VOLT_READ, 0);
        delayMicroseconds(BUS_SETTLE_US);
        for (uint8_t i = 0; i < 20; i++) { r[i] = g_ow.read(); delayMicroseconds(BUS_BYTE_GAP_US); }
    }
    bus_disable();
    if (!ok) return false;
    uint8_t n = min(ncells, (uint8_t)MAX_CELLS);
    float pack = 0;
    for (uint8_t i = 0; i < n; i++) {
        vr.cells.v[i] = (6000.0f - le16(&r[i*2])/10.0f)/1000.0f;
        pack += vr.cells.v[i];
    }
    vr.cells.n = n; vr.cells.valid = true; vr.vpack = pack; vr.ok = true;
    return true;
}

// ─── Diagnostic report ───────────────────────────────────────
static void print_report(const BatteryInfo &info, const VoltageReadResult &vr,
                         const uint8_t d[BASIC_INFO_LEN]) {
    Serial.print(F("Model          : ")); Serial.println(info.model);
    Serial.print(F("ROM ID         : "));
    if (info.rom_id_valid) {
        for (uint8_t i=0;i<8;i++) { if(info.rom_id[i]<0x10) Serial.print('0'); Serial.print(info.rom_id[i],HEX); if(i<7) Serial.print(' '); }
        Serial.println();
        char db[13]; snprintf(db,sizeof(db),"%02d/%02d/20%02d",info.rom_id[2],info.rom_id[1],info.rom_id[0]);
        Serial.print(F("Mfg date       : ")); Serial.println(db);
    } else {
        Serial.println(F("(unavailable)"));
    }
    Serial.print(F("Detected type  : ")); Serial.println(batt_type_to_int(info.type));
    Serial.print(F("Battery type   : ")); Serial.print(info.raw.batt_type); Serial.print(F("v  ("));
    if      (info.raw.batt_type<13) Serial.print(F("4 cell BL14xx"));
    else if (info.raw.batt_type<30) Serial.print(F("5 cell BL18xx"));
    else                            Serial.print(F("10 cell BL36xx"));
    Serial.println(')');
    Serial.print(F("Capacity       : ")); Serial.print(info.capacity_ah,1); Serial.println(F(" Ah"));

    print_sep();
    Serial.print(F("Lock status    : ")); Serial.println(info.locked ? F("LOCKED") : F("UNLOCKED"));
    Serial.print(F("Cell failure   : ")); Serial.println(info.raw.cell_failure ? F("YES") : F("No"));
    Serial.print(F("Failure code   : ")); print_failure_code(info.raw.failure_code);
    Serial.print(F("Checksum 0-15  : ")); Serial.println(info.checksums_ok[0] ? F("OK") : F("BAD"));
    Serial.print(F("Checksum 16-31 : ")); Serial.println(info.checksums_ok[1] ? F("OK") : F("BAD"));
    Serial.print(F("Checksum 32-40 : ")); Serial.println(info.checksums_ok[2] ? F("OK") : F("BAD"));
    Serial.print(F("Aux CSum 44-47 : ")); Serial.println(info.aux_checksums_ok[0] ? F("OK") : F("BAD"));
    Serial.print(F("Aux CSum 48-61 : ")); Serial.println(info.aux_checksums_ok[1] ? F("OK") : F("BAD"));
    { char buf[48];
      if (d[0]==0xF1||d[0]==0x50) Serial.println(F("Byte 0         : OK"));
      else { snprintf(buf,sizeof(buf),"Byte 0         : BAD (0x%02X, must be 0xF1 or 0x50)",d[0]); Serial.println(buf); }
      if (d[1]==0x26||d[1]==0x36||d[1]==0x31) Serial.println(F("Byte 1         : OK"));
      else { snprintf(buf,sizeof(buf),"Byte 1         : BAD (0x%02X, must be 0x26/0x36/0x31)",d[1]); Serial.println(buf); }
      uint8_t n34=nybble_get(d,34);
      if (n34==0) Serial.println(F("Nybble 34      : OK"));
      else { snprintf(buf,sizeof(buf),"Nybble 34      : BAD (0x%X, must be 0)",n34); Serial.println(buf); }
      if (d[18]!=0x00) Serial.println(F("Byte 18        : OK"));
      else Serial.println(F("Byte 18        : BAD (0x00, must not be 0)"));
      if (d[19]!=0x00) Serial.println(F("Byte 19        : OK"));
      else Serial.println(F("Byte 19        : BAD (0x00, must not be 0)")); }
    if (info.locked) print_lock_causes(info, d);

    print_sep();
    if (info.raw.cycles > 0) {
        Serial.print(F("Cycle count    : ")); Serial.println(info.raw.cycles);
    }
    if (info.health.rating > 0.0f) {
        Serial.print(F("Health         : ")); Serial.print(info.health.rating,2); Serial.print(F(" / 4  ("));
        int h=(int)round(info.health.rating);
        for (int i=0;i<4;i++) Serial.print(i<h ? '#' : '-');
        Serial.println(')');
    }
    if (is_type_023(info.type)) {
        if (info.health.od_count > 0) {
            Serial.print(F("OD events      : ")); Serial.println(info.health.od_count);
            if (info.raw.cycles > 0) {
                Serial.print(F("OD %           : ")); Serial.print(4.0f+100.0f*info.health.od_count/info.raw.cycles,1); Serial.println(F(" %"));
            }
        }
        if (info.health.overload_count > 0) {
            Serial.print(F("Overload cnt   : ")); Serial.println(info.health.overload_count);
            if (info.raw.cycles > 0) {
                Serial.print(F("Overload %     : ")); Serial.print(4.0f+100.0f*info.health.overload_count/info.raw.cycles,1); Serial.println(F(" %"));
            }
        }
    }
    if (is_type_56(info.type)) {
        if (info.raw.overdischarge > 0) {
            Serial.print(F("OD %           : ")); Serial.print(-5.0f*info.raw.overdischarge+160.0f,1); Serial.println(F(" %"));
        }
        if (info.raw.overload > 0) {
            Serial.print(F("Overload %     : ")); Serial.print( 5.0f*info.raw.overload-160.0f,1); Serial.println(F(" %"));
        }
    }
    if (info.temperature_c != TEMP_INVALID) {
        Serial.print(F("Temp (Cells)   : ")); Serial.print(info.temperature_c,1); Serial.println(F(" C"));
    }
    if (info.temperature_mosfet_c != TEMP_INVALID) {
        Serial.print(F("Temp (Mosfet)  : ")); Serial.print(info.temperature_mosfet_c,1); Serial.println(F(" C"));
    }

    print_sep();
    Serial.print(F("Pack voltage   : ")); Serial.println(vr.vpack,3);
    if (!vr.cells.valid || vr.cells.n == 0) {
        Serial.println(F("(no cells reported)"));
    } else {
        float vmax=vr.cells.v[0], vmin=vr.cells.v[0];
        for (uint8_t i=0;i<vr.cells.n;i++) {
            char lb[22]; snprintf(lb,sizeof(lb),"Cell %2u        : ",(unsigned)i+1);
            Serial.print(lb); Serial.println(vr.cells.v[i],3);
            if (vr.cells.v[i]>vmax) vmax=vr.cells.v[i];
            if (vr.cells.v[i]<vmin) vmin=vr.cells.v[i];
        }
        Serial.print(F("Cell diff      : ")); Serial.println(vmax-vmin,3);
    }

    if (is_type_023(info.type) && info.health.charge_level > 0 && info.raw.capacity > 0) {
        float ratio = (float)info.health.charge_level / info.raw.capacity / SOC_DIVISOR;
        uint8_t soc = (ratio<10.0f) ? 1 : (uint8_t)min((int)(ratio/10.0f),7);
        print_sep();
        Serial.print(F("State of charge: ")); Serial.print(soc); Serial.println(F(" / 7"));
    }

    print_sep();
    print_frame(d);
}

// ─── Frame repair / write ────────────────────────────────────
static bool charger_write_frame(const uint8_t src[BASIC_INFO_LEN]) {
    if (g_charger_arm_issued) {
        Serial.println(F("  Arm already issued - power-cycle required."));
        return false;
    }
    {
        uint8_t r[BASIC_INFO_LEN]={0};
        if (cmd_cc(CMD_CC_F0,sizeof(CMD_CC_F0),r,BASIC_INFO_LEN)!=BUS_OK) {
            Serial.println(F("  Arm: no presence.")); return false;
        }
        g_charger_arm_issued = true;
        safe_delay(WRITE_ARM_DELAY_MS);
    }
    {
        uint8_t payload[2+BASIC_INFO_LEN];
        payload[0]=FRAME_WRITE_OPCODE; payload[1]=FRAME_WRITE_PAD;
        memcpy(&payload[2],src,BASIC_INFO_LEN);
        if (cmd_33_no_rom(payload,sizeof(payload))!=BUS_OK) {
            Serial.println(F("  Write: no presence.")); return false;
        }
        safe_delay(WRITE_FRAME_DELAY_MS);
    }
    {
        if (cmd_33_no_rom(CMD_STORE,sizeof(CMD_STORE))!=BUS_OK) {
            Serial.println(F("  Store: no presence.")); return false;
        }
        safe_delay(WRITE_STORE_DELAY_MS);
    }
    return true;
}

static bool do_protected_write(const uint8_t frame[BASIC_INFO_LEN],
                               const __FlashStringHelper *log_prefix) {
    if (enter_testmode() != BUS_OK) {
        Serial.print(log_prefix); Serial.println(F("No presence entering testmode."));
        if (!wait_for_response()) return false;
        if (enter_testmode() != BUS_OK) {
            Serial.print(log_prefix); Serial.println(F("Still no presence. Aborting write."));
            return false;
        }
    }
    if (!charger_write_frame(frame)) {
        exit_testmode(); power_cycle_bus(); return false;
    }
    exit_testmode();
    return true;
}

// Derive correct status code (byte 19) from variant and capacity.
// Used when byte 19 is corrupt (0x00 or 0xA5 with no real cell fault).
static uint8_t derive_status_code(uint8_t byte1, uint8_t capacity) {
    if (byte1 == 0x36) return 0x1B;           // Vietnam → BL1850B Samsung
    // China variant — derive from capacity
    if (capacity >= 58) return 0x67;           // BL1860B
    if (capacity >= 48) return 0x45;           // BL1850B
    return 0x67;                               // BL1830B and others
}

// Infer variant byte 1 from the other variant bytes when byte 1 is corrupt.
static uint8_t infer_byte1(const uint8_t d[BASIC_INFO_LEN]) {
    uint8_t china_score = 0, viet_score = 0;
    if (d[2]  == 0xBD) china_score++; if (d[2]  == 0xB6) viet_score++;
    if (d[3]  == 0x13) china_score++; if (d[3]  == 0xC3) viet_score++;
    if (d[4]  == 0x14) china_score++; if (d[4]  == 0x18) viet_score++;
    if (d[12] == 0xD0) china_score++; if (d[12] == 0x01) viet_score++;
    return (viet_score > china_score) ? 0x36 : 0x26;
}

// Send DA04 error register clear. Call after a successful frame write.
static void send_da04() {
    power_cycle_bus();
    if (enter_testmode() == BUS_OK) {
        uint8_t rom[8]={0}, r[9]={0};
        cmd_33_with_rom(CMD_RESET_ERRORS, sizeof(CMD_RESET_ERRORS), rom, r, 9);
        exit_testmode();
    }
    power_cycle_bus();
}

// Comprehensive frame repair. Fixes all known lock causes while preserving
// all salvageable battery-specific data (history, variant identity, capacity).
// New family (0xF1): full repair of all known fields.
// Old family (0x50): minimal repair — nybble 34 and failure code only.
static bool repair_frame(uint8_t data32[BASIC_INFO_LEN]) {
    uint8_t frame[BASIC_INFO_LEN];
    memcpy(frame, data32, BASIC_INFO_LEN);

    if (data32[0] == 0x50) {
        // ── Old family: minimal safe repair ───────────────────
        // Only touch the charger lock nibble and failure code.
        // Preserve all other bytes — old family frame layout is different.
        uint8_t b17 = frame[17];
        b17 = (b17 & 0xF0);            // zero nybble 34 (low), preserve nybble 35 (high)
        frame[17] = b17;
        nybble_set(frame, 40, 0);
        nybble_set(frame, 41, checksum_calc(frame,  0, 15));
        nybble_set(frame, 42, checksum_calc(frame, 16, 31));
        nybble_set(frame, 43, checksum_calc(frame, 32, 40));
        nybble_set(frame, 62, checksum_calc(frame, 44, 47));
        nybble_set(frame, 63, checksum_calc(frame, 48, 61));
    } else {
    uint8_t frame[BASIC_INFO_LEN];
    memcpy(frame, data32, BASIC_INFO_LEN);

    } // end old family minimal repair

    // ── New family: full repair ────────────────────────────────
    // (only entered when byte 0 == 0xF1)
    if (data32[0] == 0xF1) {
        frame[0]  = 0xF1;
        frame[5]  = 0x58;
        frame[6]  = 0x00;
        frame[7]  = 0x00;
        frame[10] = 0x40;
        frame[11] = 0x21;
        frame[13] = 0x80;
        frame[14] = 0x02;
        frame[22] = 0x00;
        frame[17] = 0xD0;
        if (frame[18] == 0x00) frame[18] = 0x8E;
        if (frame[1] != 0x26 && frame[1] != 0x36)
            frame[1] = infer_byte1(data32);
        bool b19_corrupt = (frame[19] == 0x00)
                        || (frame[19] == 0xA5 && (frame[22] & 0x04) == 0);
        if (b19_corrupt)
            frame[19] = derive_status_code(frame[1], data32[16]);
        nybble_set(frame, 40, 0);
        nybble_set(frame, 41, checksum_calc(frame,  0, 15));
        nybble_set(frame, 42, checksum_calc(frame, 16, 31));
        nybble_set(frame, 43, checksum_calc(frame, 32, 40));
        nybble_set(frame, 62, checksum_calc(frame, 44, 47));
        nybble_set(frame, 63, checksum_calc(frame, 48, 61));
    }
    print_frame(frame, F("  Repair frame : "));

    if (!do_protected_write(frame, F("  "))) return false;
    power_cycle_bus();

    uint8_t verify[BASIC_INFO_LEN] = {0};
    if (!poll_until_response(verify)) {
        Serial.println(F("  No response during verify."));
        return false;
    }

    bool ok0 = (checksum_calc(verify,  0, 15) == nybble_get(verify, 41));
    bool ok1 = (checksum_calc(verify, 16, 31) == nybble_get(verify, 42));
    bool ok2 = (checksum_calc(verify, 32, 40) == nybble_get(verify, 43));
    bool lk  = (verify[20] & 0x0F) != 0;
    Serial.print(F("  Checksums : ")); print_checksums(ok0, ok1, ok2);

    if (ok0 && ok1 && ok2 && !lk) {
        // Send DA04 to clear internal error register so battery charges on first insert
        Serial.println(F("  DA04 clear..."));
        send_da04();
        uint8_t after_da04[BASIC_INFO_LEN] = {0};
        poll_until_response(after_da04);
        memcpy(data32, after_da04, BASIC_INFO_LEN);
        return true;
    }

    memcpy(data32, verify, BASIC_INFO_LEN);
    return false;
}

static bool type_supports_unlock(BatteryType t) { return is_type_023(t); }

// ─── Scan steps ───────────────────────────────────────────────
static BasicInfoResult step_read_basic_info(uint8_t d[BASIC_INFO_LEN]) {
    uint32_t started = millis();
    bool printed = false;
    while (millis() - started < NO_RESPONSE_TIMEOUT_MS) {
        BasicInfoResult r = read_basic_info(d);
        if (r != BASIC_INFO_NO_RESPONSE) {
            if (printed) {
                uint32_t elapsed = millis() - started;
                Serial.print(F(" OK (")); Serial.print(elapsed); Serial.println(F(" ms)"));
            }
            return r;
        }
        if (!printed) { Serial.print(F("  Waiting")); printed = true; }
        Serial.print(F("."));
        wdt_kick();
        power_cycle_bus();
    }
    if (printed) Serial.println();
    if (!wait_for_response()) return BASIC_INFO_NO_RESPONSE;
    return read_basic_info(d);
}

static void step_identify(BatteryInfo &info, const uint8_t d[BASIC_INFO_LEN]) {
    info.rom_id_valid = read_rom_id(info.rom_id);
    if (!info.rom_id_valid)
        Serial.println(F("WARNING: ROM ID read failed; using fallback type detection."));
    info.type = detect_battery_type(info.rom_id, d);
    (void)((info.type == BatteryType::T5) ? read_model_type5(info.model) : read_model(info.model));
}

static VoltageReadResult step_read_voltages(const BatteryInfo &info) {
    VoltageReadResult vr = voltage_result_init();
    switch (info.type) {
        case BatteryType::T5: safe_delay(10); read_voltages_type5(vr, info.cell_count); break;
        case BatteryType::T6: read_voltages_type6(vr, info.cell_count); break;
        default:              read_voltages_type023(vr, info.cell_count); break;
    }
    return vr;
}

static void step_read_health(BatteryInfo &info, const uint8_t d[BASIC_INFO_LEN]) {
    info.health = {};
    if (is_type_56(info.type)) {
        info.health.rating = calc_health_type56(info.raw);
    } else if (is_type_023(info.type)) {
        info.health.rating         = calc_health_type023(read_health_raw(info.type), info.raw.capacity);
        info.health.od_count       = read_od_count(info.type);
        info.health.overload_count = read_overload_count(info.type);
        info.health.charge_level   = (info.type==BatteryType::T0) ? read_charge_level() : 0;
    } else {
        info.health.rating = calc_health_damage_rating(d);
    }
}

// ─── Unlock sequence ─────────────────────────────────────────
static void step_handle_lock(BatteryInfo &info, uint8_t d[BASIC_INFO_LEN]) {
    if (!info.locked) {
        Serial.println(F("Battery UNLOCKED."));
        led_flash(COL_GREEN);
        if (type_supports_unlock(info.type)) power_cycle_bus();
        return;
    }
    if (!type_supports_unlock(info.type)) {
        Serial.print(F("Unlock not supported for type "));
        Serial.print(batt_type_to_int(info.type));
        Serial.println(F(". Skipping."));
        led_flash(COL_RED); return;
    }

    Serial.println(F("Battery LOCKED."));
    led_yellow();

    bool unlocked = false;

    // Step 1 — DA04: clears internal error register.
    // Handles naturally locked batteries (overdischarge, overload) on all types.
    // Type 2 batteries self-repair entirely via DA04.
    Serial.println(F("--- Step 1: DA04 reset"));
    led_yellow();
    power_cycle_bus();
    if (enter_testmode() == BUS_OK) {
        uint8_t rom[8]={0}, r[9]={0};
        cmd_33_with_rom(CMD_RESET_ERRORS, sizeof(CMD_RESET_ERRORS), rom, r, 9);
        exit_testmode();
        power_cycle_bus();
        uint8_t verify[BASIC_INFO_LEN] = {0};
        if (poll_until_response(verify)) {
            memcpy(d, verify, BASIC_INFO_LEN);
            refresh_info_from_frame(d, info);
            if (!info.locked) {
                Serial.println(F("  -> UNLOCKED by DA04."));
                unlocked = true;
            } else {
                Serial.println(F("  -> Still locked. Proceeding to frame repair."));
            }
        }
    }

    // Step 2 — Comprehensive frame repair.
    // Fixes all known corrupt fields: lock nibble, checksums, failure code,
    // variant bytes, status code, constants. Followed by DA04 internally.
    if (!unlocked) {
        for (uint8_t attempt = 1; attempt <= UNLOCK_MAX_CYCLES && !unlocked; attempt++) {
            Serial.print(F("--- Step 2: Frame repair (attempt ")); Serial.print(attempt); Serial.println(F(")"));
            led_purple();
            wdt_kick();
            if (repair_frame(d)) {
                refresh_info_from_frame(d, info);
                if (!info.locked) {
                    Serial.println(F("  -> Frame repair succeeded. UNLOCKED."));
                    unlocked = true;
                } else {
                    Serial.println(F("  -> Repaired but still locked. Retrying."));
                }
            } else {
                Serial.println(F("  -> Frame repair failed."));
            }
        }
    }

    if (unlocked) { Serial.println(F("Battery successfully unlocked.")); led_flash(COL_GREEN); power_cycle_bus(); }
    else          { Serial.println(F("Battery still locked."));           led_flash(COL_RED); }
}

// ═══════════════════════════════════════════════════════════════
//  OMEGA LOCK
//  Sets nybble 34 (original Makita charger lock nibble) non-zero.
//  Present in ALL Makita LXT batteries. Charger rejects if non-zero.
//  DA04 cannot undo this lock. Only repair_frame() can restore it.
// ═══════════════════════════════════════════════════════════════

static void omega_mutate(uint8_t v[BASIC_INFO_LEN]) {
    // Set nybble 34 = 4, preserve nybble 35 = D → byte 17 = 0xD4
    v[17] = 0xD4;
    // Recalculate CS2 (covers nybbles 32-40, includes byte 17)
    nybble_set(v, 43, checksum_calc(v, 32, 40));
}

static bool omega_verify(const uint8_t v[BASIC_INFO_LEN]) {
    return nybble_get(v, 34) != 0;
}

static bool omega_already_locked(const uint8_t frame[BASIC_INFO_LEN]) {
    return nybble_get(frame, 34) != 0;
}

static bool run_lock() {
    if (g_in_testmode) exit_testmode();
    power_cycle_bus(); led_blue();

    uint8_t rom_id[8] = {0};
    bool rom_ok = read_rom_id(rom_id);

    uint8_t frame[BASIC_INFO_LEN] = {0};
    BasicInfoResult fr = step_read_basic_info(frame);
    if (fr != BASIC_INFO_OK) {
        if (fr == BASIC_INFO_PRE_TYPE0) { Serial.println(F("  [Lock] Pre-type-0 HC08 - unsupported.")); led_yellow(); }
        else                            { Serial.println(F("  [Lock] ERROR: No valid frame."));          led_flash(COL_RED); }
        bus_disable(); return false;
    }
    if (!rom_ok) Serial.println(F("  [Lock] WARNING: ROM ID read failed."));

    int8_t type = lock_detect_type(rom_id, frame);
    Serial.print(F("  [Lock] Type: "));
    if (type < 0) { Serial.println(F("unsupported (5, 6, or unknown).")); led_yellow(); bus_disable(); return false; }
    Serial.println(type);

    if (omega_already_locked(frame)) {
        Serial.println(F("  [Lock] Battery already OMEGA LOCKED (nybble 34 != 0)."));
        led_flash(COL_RED); bus_disable(); return true;
    }

    bool ok = false;
    for (uint8_t attempt = 1; attempt <= LOCK_MAX_ATTEMPTS && !ok; attempt++) {
        Serial.print(F("  [Lock] Attempt ")); Serial.println(attempt);
        wdt_kick();
        uint8_t v[BASIC_INFO_LEN];
        memcpy(v, frame, BASIC_INFO_LEN);
        omega_mutate(v);
        led_purple();
        if (!do_protected_write(v, F("  [Lock] "))) { bus_disable(); return false; }
        power_cycle_bus();
        uint8_t verify[BASIC_INFO_LEN] = {0};
        if (!poll_until_response(verify)) { Serial.println(F("  [Lock] No response.")); break; }
        Serial.print(F("  [Lock] Nybble 34 = ")); Serial.println(nybble_get(verify, 34));
        ok = omega_verify(verify);
    }

    print_sep();
    if (ok) Serial.println(F("  [Lock] OMEGA LOCKED. Nybble 34 set non-zero."));
    else    Serial.println(F("  [Lock] FAILED."));
    print_sep();
    led_result(ok); bus_disable();
    return ok;
}

// ─── Main scan ───────────────────────────────────────────────
static bool run_scan() {
    if (g_in_testmode) exit_testmode();
    power_cycle_bus(); led_blue();

    BatteryInfo info{};
    info.temperature_c = info.temperature_mosfet_c = TEMP_INVALID;
    info.type = BatteryType::UNKNOWN;
    uint8_t data32[BASIC_INFO_LEN] = {0};

    BasicInfoResult br = step_read_basic_info(data32);
    if (br == BASIC_INFO_NO_RESPONSE) {
        Serial.println(F("ERROR: Battery not responding after retries. Aborting."));
        bus_disable(); g_charger_arm_issued = false; return false;
    }
    if (br == BASIC_INFO_PRE_TYPE0) {
        print_sep();
        Serial.println(F("WARNING: Pre-type-0 HC08 battery detected!"));
        Serial.println(F("         Freescale MC908JK3E BMS -- no cell protection."));
        Serial.println(F("         DO NOT charge on any charger."));
        print_sep();
        led_flash(COL_RED); bus_disable(); g_charger_arm_issued = false; return true;
    }

    refresh_info_from_frame(data32, info);
    step_identify(info, data32);
    print_sep();
    Serial.print(F("Battery found  : ")); Serial.println(info.model);

    VoltageReadResult vr = step_read_voltages(info);
    info.temperature_c        = vr.t_cell;
    info.temperature_mosfet_c = vr.t_mosfet;
    if (info.type == BatteryType::T6) info.temperature_c = read_temperature_type6();
    step_read_health(info, data32);

    if (!battery_present()) {
        Serial.println(F("Battery removed during scan. Aborting."));
        bus_disable(); g_charger_arm_issued = false; return false;
    }

    if (!vr.ok) Serial.println(F("WARNING: voltage read failed; cell data may be missing."));
    print_sep(); print_report(info, vr, data32); print_sep();

    // Detect cell imbalance — warn via orange blip after result
    bool imbalanced = false;
    if (vr.cells.valid && vr.cells.n > 1) {
        float vmax=vr.cells.v[0], vmin=vr.cells.v[0];
        for (uint8_t i=1;i<vr.cells.n;i++) {
            if (vr.cells.v[i]>vmax) vmax=vr.cells.v[i];
            if (vr.cells.v[i]<vmin) vmin=vr.cells.v[i];
        }
        if ((vmax-vmin) >= IMBALANCE_THRESHOLD_V) {
            imbalanced = true;
            Serial.print(F("WARNING: Cell imbalance "));
            Serial.print(vmax-vmin, 3);
            Serial.println(F("V — check balancing tabs."));
        }
    }

    step_handle_lock(info, data32);
    print_sep(); Serial.println(F("Complete."));

    // Set imbalance blip — orange over result colour
    g_imbalance     = imbalanced;
    Colour rc = info.locked ? COL_RED : COL_GREEN;
    g_result_r = rc.r; g_result_g = rc.g; g_result_b = rc.b;

    bus_disable(); g_charger_arm_issued = false;
    return true;
}

// ─── Core 1 — LED animation ───────────────────────────────────
// Core 1 handles the breathing pulse independently of Core 0's
// battery operations. Volatile flags are the only shared state.
void setup1() { /* nothing needed */ }

void loop1() {
    if (g_pulse_active) {
        led_pulse(g_pulse_scan_mode ? COL_WHITE : COL_RED);
        delay(LED_PULSE_INTERVAL_MS);
        return;
    }
    if (g_imbalance) {
        // Orange blip every IMBALANCE_BLIP_INTERVAL_MS over the result colour
        delay(IMBALANCE_BLIP_INTERVAL_MS - IMBALANCE_BLIP_ON_MS);
        led_set(COL_ORANGE);
        delay(IMBALANCE_BLIP_ON_MS);
        led_set({(uint8_t)g_result_r, (uint8_t)g_result_g, (uint8_t)g_result_b});
        return;
    }
    delay(LED_PULSE_INTERVAL_MS);
}

// ─── Hot-swap state machine ───────────────────────────────────
enum ScanState { WAIT_BATTERY, SCAN_PENDING, IDLE, UNSUPPORTED };
static ScanState  g_state         = WAIT_BATTERY;
static uint32_t   g_last_poll     = 0;
static DeviceMode g_last_mode     = MODE_SCAN;

static constexpr uint16_t MODE_DEBOUNCE_MS    = 50;
static constexpr uint8_t  MODE_DEBOUNCE_COUNT = 4;
static uint8_t   g_mode_debounce  = 0;
static uint32_t  g_last_mode_poll = 0;

// ─── Arduino entry points ─────────────────────────────────────
void setup() {
    Serial.begin(115200);
    pinMode(PIN_ENABLE, OUTPUT); digitalWrite(PIN_ENABLE, LOW);
#if defined(ARDUINO_ARCH_RP2040)
    pinMode(PIN_MODE_OUT, OUTPUT); digitalWrite(PIN_MODE_OUT, HIGH);
    pinMode(PIN_MODE_IN1, INPUT_PULLDOWN);
#else
    pinMode(PIN_MODE_OUT, OUTPUT); digitalWrite(PIN_MODE_OUT, LOW);
    pinMode(PIN_MODE_IN1, INPUT_PULLUP);
    delay(750);  // pullups need time to settle before mode_read()
#endif
    g_pixel.begin(); g_pixel.setBrightness(LED_BRIGHTNESS_MAX); led_off();

    print_sep();
    Serial.print(F("  Firmware v")); Serial.println(F(FIRMWARE_VERSION));
    DeviceMode m = mode_read();
    switch (m) {
        case MODE_LOCK:
            Serial.println(F("  Makita OMEGA LOCK Utility - Types 0 / 2 / 3 only"));
            Serial.println(F("  GPIO0-GPIO1 bridged -> OMEGA LOCK MODE (nybble 34)"));
            Serial.println(F("  Remove bridge to switch to scan/unlock mode."));
            break;
        default:
            Serial.println(F("  Makita Monitor - Scan / Unlock Mode"));
            Serial.println(F("  's' = rescan | auto-detects connect/disconnect"));
            Serial.println(F("  Bridge GPIO0-GPIO1 for Omega lock."));
            break;
    }
    print_sep();
    Serial.println(F("Waiting for battery..."));
    g_state = WAIT_BATTERY;
    g_last_mode = m;
    wdt_begin();
}

void loop() {
    wdt_kick();
    uint32_t now = millis();
    DeviceMode cur_mode = g_last_mode;

    // Mode-change detection (debounced, 4x50 ms ~= 200 ms)
    if (now - g_last_mode_poll >= MODE_DEBOUNCE_MS) {
        g_last_mode_poll = now;
        DeviceMode raw = mode_read();
        if (raw == g_last_mode) {
            g_mode_debounce = 0;
        } else if (++g_mode_debounce >= MODE_DEBOUNCE_COUNT) {
            g_mode_debounce = 0; g_last_mode = raw; cur_mode = raw;
            if (g_state==IDLE || g_state==WAIT_BATTERY || g_state==UNSUPPORTED) {
                led_off(); g_state = WAIT_BATTERY;
                print_sep();
                switch (cur_mode) {
                    case MODE_LOCK:
                        Serial.println(F("  Mode changed -> OMEGA LOCK MODE"));
                        Serial.println(F("  GPIO0-GPIO1 bridged. Insert battery to lock."));
                        break;
                    default:
                        Serial.println(F("  Mode changed -> SCAN / UNLOCK MODE"));
                        Serial.println(F("  Bridge removed. Insert battery to scan."));
                        break;
                }
                print_sep();
            }
        }
    }

    // Serial command (scan mode only)
    if (cur_mode == MODE_SCAN && Serial.available()) {
        char c = (char)Serial.read();
        if ((c=='s'||c=='S') && g_state!=SCAN_PENDING) {
            Serial.println(F("Manual rescan requested."));
            g_state = SCAN_PENDING;
        }
    }

    // Presence poll
    if (now - g_last_poll >= POLL_INTERVAL_MS) {
        g_last_poll = now;
        bool present = battery_present();
        if (g_state==WAIT_BATTERY && present) {
            print_sep();
            if (cur_mode == MODE_LOCK) Serial.println(F("  [Lock] Battery detected - omega locking..."));
            else                       Serial.println(F("  Battery detected - starting scan."));
            print_sep();
            g_state = SCAN_PENDING;
        } else if ((g_state==IDLE || g_state==UNSUPPORTED) && !present) {
            print_sep();
            if (cur_mode == MODE_LOCK) Serial.println(F("  [Lock] Battery removed. Waiting for next..."));
            else                       Serial.println(F("  Battery removed. Waiting..."));
            print_sep();
            led_off(); g_state = WAIT_BATTERY; g_imbalance = false;
        }
    }

    // LED pulse — driven by Core 1 via g_pulse_active flag.
    // Just update which colour/mode to use.
    if (g_state == WAIT_BATTERY) {
        g_pulse_scan_mode = (cur_mode == MODE_SCAN);
        g_pulse_active    = true;
    } else {
        g_pulse_active = false;
    }

    // Execute pending action
    if (g_state==SCAN_PENDING) {
        bool ok;
        if (cur_mode == MODE_LOCK) ok = run_lock();
        else                       ok = run_scan();
        g_state = ok ? IDLE : UNSUPPORTED;
        if (!ok) Serial.println(F("  Remove battery and try again."));
    }
}
