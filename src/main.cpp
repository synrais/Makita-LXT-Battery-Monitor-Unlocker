// ============================================================
//  Makita Battery Monitor / Unlock / Lock Utility
//
//  Normal mode    : full battery scan + auto-unlock (default)
//  Dead lock mode : sets failure-code nybble 40 = 15 on types 0 / 2 / 3
//  CRC lock mode  : corrupts checksums on types 0 / 2 / 3
//
//  Mode select :  bridge GPIO0 -> GPIO1  =>  DEAD LOCK  (red pulse)
//                 bridge GPIO0 -> GPIO2  =>  CRC LOCK   (red pulse)
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
#define FIRMWARE_VERSION "1.2.0"

// ─── Pin definitions ─────────────────────────────────────────
static constexpr uint8_t PIN_ONEWIRE      = 6;
static constexpr uint8_t PIN_ENABLE       = 8;
static constexpr uint8_t NEOPIXEL_OUT_PIN = 16;
static constexpr uint8_t NUM_PIXELS       = 1;
static constexpr uint8_t PIN_MODE_OUT     = 0;
static constexpr uint8_t PIN_MODE_IN1     = 1;   // Dead-lock mode (bridge GPIO0-GPIO1)
static constexpr uint8_t PIN_MODE_IN2     = 2;   // CRC-lock mode  (bridge GPIO0-GPIO2)

// On RP2040 PIN_MODE_OUT drives HIGH and the inputs use INPUT_PULLDOWN, so a
// bridge pulls them HIGH. On other boards we drive LOW with INPUT_PULLUP.
#if defined(ARDUINO_ARCH_RP2040)
  static constexpr bool MODE_PIN_ACTIVE_HIGH = true;
#else
  static constexpr bool MODE_PIN_ACTIVE_HIGH = false;
#endif

// ─── Protocol constants ───────────────────────────────────────
static constexpr uint16_t BUS_BYTE_GAP_US   = 90;   // OBI: delayMicroseconds(90)
static constexpr uint16_t BUS_RESET_GAP_US  = 400;  // OBI: delayMicroseconds(400)
static constexpr uint16_t BUS_SETTLE_US     = 150;  // OBI: no equivalent
static constexpr uint16_t BUS_ENABLE_MS     = 100;  // OBI: delay(400)
static constexpr uint16_t BUS_PRESENCE_MS   = 100;   // OBI: delay(400)
static constexpr uint16_t POWERCYCLE_OFF_MS = 200;  // OBI: no equivalent
static constexpr uint16_t POWERCYCLE_ON_MS  = 200;  // OBI: no equivalent

// Type-5 (F0513) chip needs a longer power-up settle than the rest of the protocol.
static constexpr uint16_t TYPE5_POWERUP_MS  = 420;  // OBI: delay(400) - not a typo - F0513 power-up settle

// Inter-step delays for the frame-write / store sequence.
static constexpr uint16_t WRITE_ARM_DELAY_MS    = 50;   // OBI: no equivalent
static constexpr uint16_t WRITE_FRAME_DELAY_MS  = 50;   // OBI: no equivalent
static constexpr uint16_t WRITE_STORE_DELAY_MS  = 100;  // OBI: no equivalent
static constexpr uint16_t TESTMODE_SETTLE_MS    = 50;   // OBI: no equivalent
static constexpr uint16_t TYPE_PROBE_RECOVER_MS = 150;  // OBI: no equivalent
static constexpr uint16_t VERIFY_RETRY_DELAY_MS = 420;  // OBI: no equivalent
static constexpr uint8_t  VERIFY_RETRIES        = 10;   // OBI: no equivalent

static constexpr uint8_t CMD_BASIC_INFO[]      = { 0xAA, 0x00 };
static constexpr uint8_t BASIC_INFO_LEN        = 32;
static constexpr uint8_t TYPE_PROBE_MAGIC      = 0x06;

static constexpr uint8_t CMD_MODEL[]           = { 0xDC, 0x0C };
static constexpr uint8_t CMD_TESTMODE_ENTER[]  = { 0xD9, 0x96, 0xA5 };
static constexpr uint8_t CMD_TESTMODE_EXIT[]   = { 0xD9, 0xFF, 0xFF };
static constexpr uint8_t CMD_RESET_ERRORS[]    = { 0xDA, 0x04 };
static constexpr uint8_t CMD_CC_F0[]           = { 0xF0, 0x00 }; // clears bus state / arms frame write path
static constexpr uint8_t FRAME_WRITE_OPCODE    = 0x0F;
static constexpr uint8_t FRAME_WRITE_PAD       = 0x00;
static constexpr uint8_t CMD_STORE[]           = { 0x55, 0xA5 };

static constexpr float TEMP_INVALID = -999.0f;
static constexpr float TEMP6_A      = 9323.0f;
static constexpr float TEMP6_B      = -40.0f;

// ─── Type 5 command bytes ─────────────────────────────────────
static constexpr uint8_t TYPE5_CMD_SESSION = 0x99;   // enters model-read session
static constexpr uint8_t TYPE5_CMD_MODEL   = 0x31;   // queries model bytes
static constexpr uint8_t TYPE5_CMD_TEMP    = 0x52;   // reads cell temperature

// ─── Type 6 command bytes ─────────────────────────────────────
static constexpr uint8_t TYPE6_VOLT_READ   = 0xD4;   // voltage read prefix

static constexpr uint8_t  HEALTH_SCALE_EXTENDED_CODES[] = { 26, 28, 40, 50 };
static constexpr size_t   HEALTH_SCALE_EXTENDED_COUNT   =
    sizeof(HEALTH_SCALE_EXTENDED_CODES) / sizeof(HEALTH_SCALE_EXTENDED_CODES[0]);
static constexpr uint16_t HEALTH_SCALE_STANDARD = 600;
static constexpr uint16_t HEALTH_SCALE_EXTENDED = 1000;
static constexpr float    HEALTH_MAX            = 4.0f;
static constexpr float    SOC_DIVISOR           = 2880.0f;

// ─── Scan timing ─────────────────────────────────────────────
static constexpr uint16_t SCAN_RETRY_DELAY_MS   = 200;
static constexpr uint8_t  SCAN_MAX_RETRIES      = 15;
static constexpr uint16_t POLL_INTERVAL_MS      = 800;
static constexpr uint8_t  LED_PULSE_INTERVAL_MS = 20;

// ─── Unlock / lock attempt limits ────────────────────────────
static constexpr uint8_t  UNLOCK_MAX_CYCLES     = 6;
static constexpr uint8_t  LOCK_MAX_ATTEMPTS     = 3;

// ─── LED parameters ──────────────────────────────────────────
static constexpr uint8_t  LED_BRIGHTNESS_MAX    = 80;
static constexpr uint8_t  LED_FLASH_COUNT       = 3;
static constexpr uint16_t LED_FLASH_ON_MS       = 200;
static constexpr uint16_t LED_FLASH_OFF_MS      = 100;
static constexpr uint16_t LED_PULSE_PERIOD_MS   = 2000;

// ─── Watchdog ────────────────────────────────────────────────
// Long enough that the slowest legitimate operation (verify retry loop,
// ~10 * 420 ms = 4.2 s) fits comfortably.
static constexpr uint32_t WATCHDOG_TIMEOUT_MS = 8000;

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

// ─── NeoPixel ────────────────────────────────────────────────
struct Colour { uint8_t r, g, b; };
static constexpr Colour COL_OFF    = {  0,                  0,                  0 };
static constexpr Colour COL_GREEN  = {  0, LED_BRIGHTNESS_MAX,                  0 };
static constexpr Colour COL_YELLOW = { LED_BRIGHTNESS_MAX, 60,                  0 };
static constexpr Colour COL_BLUE   = {  0,                  0, LED_BRIGHTNESS_MAX };
static constexpr Colour COL_RED    = { LED_BRIGHTNESS_MAX,  0,                  0 };
static constexpr Colour COL_PURPLE = { LED_BRIGHTNESS_MAX,  0, LED_BRIGHTNESS_MAX };
static constexpr Colour COL_WHITE  = { LED_BRIGHTNESS_MAX, LED_BRIGHTNESS_MAX, LED_BRIGHTNESS_MAX };

static void led_set(Colour c) {
    g_pixel.setPixelColor(0, g_pixel.Color(c.r, c.g, c.b));
    g_pixel.show();
}
static void led_off()    { led_set(COL_OFF);    }
static void led_yellow() { led_set(COL_YELLOW); }
static void led_blue()   { led_set(COL_BLUE);   }
static void led_purple() { led_set(COL_PURPLE); }

static void led_flash(Colour c) {
    for (uint8_t i = 0; i < LED_FLASH_COUNT; i++) {
        led_set(c);  safe_delay(LED_FLASH_ON_MS);
        led_off();   safe_delay(LED_FLASH_OFF_MS);
    }
    led_set(c);
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
// Mode 1 = DEAD LOCK    (GPIO0-GPIO1 bridged)
// Mode 2 = CRC LOCK     (GPIO0-GPIO2 bridged)
enum DeviceMode { MODE_SCAN = 0, MODE_LOCK_DEAD = 1, MODE_LOCK_CRC = 2 };

static inline bool mode_pin_active(uint8_t pin) {
    return digitalRead(pin) == (MODE_PIN_ACTIVE_HIGH ? HIGH : LOW);
}

static DeviceMode mode_read() {
    if (mode_pin_active(PIN_MODE_IN1)) return MODE_LOCK_DEAD;  // GPIO1
    if (mode_pin_active(PIN_MODE_IN2)) return MODE_LOCK_CRC;   // GPIO2
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
    delayMicroseconds(BUS_RESET_GAP_US);
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
        default: Serial.print(fc); Serial.println(F(" - Invalid?")); break;
    }
}

static void print_frame(const uint8_t d[BASIC_INFO_LEN],
                        const __FlashStringHelper *label = F("Frame          : ")) {
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
// Used by type-5 transactions that don't go through the normal cmd_cc path.
// Handles: bus enable + power-up settle, reset (bail on no presence),
// optional 0xCC skip-ROM, send cmd[], read rsp[], bus disable.
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
    // Step 1: open a model-read session (skip-ROM + session command).
    uint8_t dummy[1] = {0};
    bool ok = raw_cc_session(0, BUS_RESET_GAP_US, BUS_BYTE_GAP_US,
                             /*skip_rom=*/true, &TYPE5_CMD_SESSION, 1, dummy, 0);
    if (!ok) { snprintf(out, 8, "BL18xx"); return false; }
    safe_delay(10);

    // Step 2: query model bytes (no skip-ROM, bare command).
    uint8_t r[2] = {0};
    ok = raw_cc_session(0, BUS_RESET_GAP_US, BUS_BYTE_GAP_US,
                        /*skip_rom=*/false, &TYPE5_CMD_MODEL, 1, r, 2);
    if (!ok) { snprintf(out, 8, "BL18xx"); return false; }

    if ((r[0] != 0xFF || r[1] != 0xFF) && (r[0] != 0x00 || r[1] != 0x00)) {
        snprintf(out, 8, "BL%02X%02X", r[1], r[0]);
        return true;
    }
    snprintf(out, 8, "BL18xx");
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
          exit_testmode(); safe_delay(TYPE_PROBE_RECOVER_MS);
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
    info.locked = !info.checksums_ok[0] || !info.checksums_ok[1] || !info.checksums_ok[2]
               || ((d[20] & 0x0F) != 0);
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

// Type-5 raw transaction. Returns true if presence detected.
static bool type5_raw_cc(const uint8_t *data, uint8_t dlen, uint8_t *rsp, uint8_t rlen) {
    return raw_cc_session(TYPE5_POWERUP_MS, BUS_RESET_GAP_US, BUS_BYTE_GAP_US,
                          /*skip_rom=*/true, data, dlen, rsp, rlen);
}

static bool read_voltages_type5(VoltageReadResult &vr, uint8_t ncells) {
    static const uint8_t CELL_CMDS[] = { 0x31, 0x32, 0x33, 0x34, 0x35 };
    uint8_t ign[2] = {0};
    if (!type5_raw_cc(CMD_CC_F0, sizeof(CMD_CC_F0), ign, 0)) return false;
    type5_raw_cc(CMD_CC_F0, sizeof(CMD_CC_F0), ign, 0);
    type5_raw_cc(&CELL_CMDS[0], 1, ign, 2);   // prime read (throwaway)

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
    // Header
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
    Serial.print(F("Battery type   : ")); Serial.print(info.raw.batt_type); Serial.print(F("  ("));
    if      (info.raw.batt_type<13) Serial.print(F("4 cell BL14xx"));
    else if (info.raw.batt_type<30) Serial.print(F("5 cell BL18xx"));
    else                            Serial.print(F("10 cell BL36xx"));
    Serial.println(')');
    Serial.print(F("Capacity       : ")); Serial.print(info.capacity_ah,1); Serial.println(F(" Ah"));

    // Status
    print_sep();
    Serial.print(F("Lock status    : ")); Serial.println(info.locked ? F("LOCKED") : F("UNLOCKED"));
    Serial.print(F("Cell failure   : ")); Serial.println(info.raw.cell_failure ? F("YES") : F("No"));
    Serial.print(F("Failure code   : ")); print_failure_code(info.raw.failure_code);
    Serial.print(F("Checksum 0-15  : ")); Serial.println(info.checksums_ok[0] ? F("OK") : F("FAIL"));
    Serial.print(F("Checksum 16-31 : ")); Serial.println(info.checksums_ok[1] ? F("OK") : F("FAIL"));
    Serial.print(F("Checksum 32-40 : ")); Serial.println(info.checksums_ok[2] ? F("OK") : F("FAIL"));
    Serial.print(F("Aux CSum 44-47 : ")); Serial.println(info.aux_checksums_ok[0] ? F("OK") : F("FAIL"));
    Serial.print(F("Aux CSum 48-61 : ")); Serial.println(info.aux_checksums_ok[1] ? F("OK") : F("FAIL"));

    // Health
    print_sep();
    Serial.print(F("Cycle count    : ")); Serial.println(info.raw.cycles);
    Serial.print(F("Health         : ")); Serial.print(info.health.rating,2); Serial.print(F(" / 4  ("));
    int h=(int)round(info.health.rating);
    for (int i=0;i<4;i++) Serial.print(i<h ? '#' : '-');
    Serial.println(')');
    if (is_type_023(info.type)) {
        Serial.print(F("OD events      : ")); Serial.println(info.health.od_count);
        Serial.print(F("Overload cnt   : ")); Serial.println(info.health.overload_count);
        if (info.health.od_count>0 && info.raw.cycles>0) {
            Serial.print(F("OD %           : ")); Serial.print(4.0f+100.0f*info.health.od_count/info.raw.cycles,1); Serial.println(F(" %"));
        }
        if (info.health.overload_count>0 && info.raw.cycles>0) {
            Serial.print(F("Overload %     : ")); Serial.print(4.0f+100.0f*info.health.overload_count/info.raw.cycles,1); Serial.println(F(" %"));
        }
    }
    if (is_type_56(info.type)) {
        Serial.print(F("OD %           : ")); Serial.print(-5.0f*info.raw.overdischarge+160.0f,1); Serial.println(F(" %"));
        Serial.print(F("Overload %     : ")); Serial.print( 5.0f*info.raw.overload     -160.0f,1); Serial.println(F(" %"));
    }
    if (info.temperature_c != TEMP_INVALID) {
        Serial.print(F("Temp (Cells)   : ")); Serial.print(info.temperature_c,1); Serial.println(F(" C"));
    }
    if (info.temperature_mosfet_c != TEMP_INVALID) {
        Serial.print(F("Temp (Mosfet)  : ")); Serial.print(info.temperature_mosfet_c,1); Serial.println(F(" C"));
    }

    // Voltages
    print_sep();
    Serial.print(F("Pack voltage   : ")); Serial.print(vr.vpack,3); Serial.println(F(" V"));
    if (!vr.cells.valid || vr.cells.n == 0) {
        Serial.println(F("(no cells reported)"));
    } else {
        float vmax=vr.cells.v[0], vmin=vr.cells.v[0];
        for (uint8_t i=0;i<vr.cells.n;i++) {
            char lb[22]; snprintf(lb,sizeof(lb),"Cell %2u        : ",(unsigned)i+1);
            Serial.print(lb); Serial.print(vr.cells.v[i],3); Serial.println(F(" V"));
            if (vr.cells.v[i]>vmax) vmax=vr.cells.v[i];
            if (vr.cells.v[i]<vmin) vmin=vr.cells.v[i];
        }
        Serial.print(F("Cell diff      : ")); Serial.print(vmax-vmin,3); Serial.println(F(" V"));
    }

    // State of charge (type 0/2/3 only)
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

// Enter testmode, write frame, exit testmode.
// On write failure: exits testmode and power-cycles to clear arm state.
static bool do_protected_write(const uint8_t frame[BASIC_INFO_LEN],
                               const __FlashStringHelper *log_prefix) {
    if (enter_testmode() != BUS_OK) {
        Serial.print(log_prefix); Serial.println(F("No presence entering testmode."));
        return false;
    }
    if (!charger_write_frame(frame)) {
        exit_testmode(); power_cycle_bus(); return false;
    }
    exit_testmode();
    return true;
}

static bool write_corrected_frame(uint8_t data32[BASIC_INFO_LEN]) {
    uint8_t frame[BASIC_INFO_LEN]; memcpy(frame,data32,BASIC_INFO_LEN);
    nybble_set(frame,40,0);
    nybble_set(frame,41,checksum_calc(frame, 0, 15));
    nybble_set(frame,42,checksum_calc(frame,16, 31));
    nybble_set(frame,43,checksum_calc(frame,32, 40));
    nybble_set(frame,62,checksum_calc(frame,44, 47));
    nybble_set(frame,63,checksum_calc(frame,48, 61));
    print_frame(frame, F("  Repair frame : "));

    if (!do_protected_write(frame, F("  "))) return false;
    power_cycle_bus();

    uint8_t verify[BASIC_INFO_LEN]={0};
    BasicInfoResult vr = BASIC_INFO_NO_RESPONSE;
    for (uint8_t i = 0; i < VERIFY_RETRIES && vr == BASIC_INFO_NO_RESPONSE; i++) {
        safe_delay(VERIFY_RETRY_DELAY_MS); vr = read_basic_info(verify);
    }
    if (vr != BASIC_INFO_OK) { Serial.println(F("  No response during verify.")); return false; }

    bool ok0 = (checksum_calc(verify, 0,  15) == nybble_get(verify, 41));
    bool ok1 = (checksum_calc(verify, 16, 31) == nybble_get(verify, 42));
    bool ok2 = (checksum_calc(verify, 32, 40) == nybble_get(verify, 43));
    bool lk  = (verify[20] & 0x0F) != 0;
    Serial.print(F("  Checksums : ")); print_checksums(ok0, ok1, ok2);

    memcpy(data32, verify, BASIC_INFO_LEN);
    return ok0 && ok1 && ok2 && !lk;
}

static bool type_supports_unlock(BatteryType t) { return is_type_023(t); }

// ─── Scan steps ───────────────────────────────────────────────
static BasicInfoResult step_read_basic_info(uint8_t d[BASIC_INFO_LEN]) {
    for (uint8_t i = 0; i < SCAN_MAX_RETRIES; i++) {
        BasicInfoResult r = read_basic_info(d);
        if (r != BASIC_INFO_NO_RESPONSE) return r;
        safe_delay(SCAN_RETRY_DELAY_MS);
    }
    return BASIC_INFO_NO_RESPONSE;
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

// ─── Unlock result handling ───────────────────────────────────
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

    Serial.print(F("Battery LOCKED. Failure code: ")); print_failure_code(info.raw.failure_code);
    led_yellow();

    bool unlocked = false;
    for (uint8_t attempt = 1; attempt <= UNLOCK_MAX_CYCLES && !unlocked; attempt++) {
        Serial.print(F("--- Unlock attempt ")); Serial.println(attempt);
        wdt_kick();

        if (attempt % 2 == 1) {
            // Odd: DA04 reset
            Serial.println(F("  DA04 reset...")); led_yellow();
            power_cycle_bus();
            if (enter_testmode() != BUS_OK) { Serial.println(F("  No presence entering testmode.")); continue; }
            { uint8_t rom[8]={0}, r[9]={0};
              cmd_33_with_rom(CMD_RESET_ERRORS, sizeof(CMD_RESET_ERRORS), rom, r, 9); }
            exit_testmode(); power_cycle_bus();
            if (read_basic_info(d) != BASIC_INFO_OK) { Serial.println(F("  No response after DA04.")); continue; }
            refresh_info_from_frame(d, info);
            Serial.print(F("  Checksums : ")); print_checksums(info.checksums_ok[0], info.checksums_ok[1], info.checksums_ok[2]);
            if (!info.locked) { Serial.println(F("  -> UNLOCKED by DA04.")); unlocked = true; }
            else               Serial.println(F("  -> Still locked."));
        } else {
            // Even: frame write
            Serial.println(F("  Frame repair...")); led_purple();
            if (write_corrected_frame(d)) {
                refresh_info_from_frame(d, info);
                Serial.println(F("  -> Frame repair succeeded. UNLOCKED."));
                unlocked = true;
            } else {
                Serial.println(F("  -> Frame repair failed."));
            }
        }
    }

    if (unlocked) { Serial.println(F("Battery successfully unlocked.")); led_flash(COL_GREEN); power_cycle_bus(); }
    else          { Serial.println(F("Battery still locked."));           led_flash(COL_RED); }
}

// ═══════════════════════════════════════════════════════════════
//  LOCK MODES (CRC + DEAD)
// ═══════════════════════════════════════════════════════════════

static bool checksums_bad(const uint8_t *buf) {
    return (checksum_calc(buf, 0,  15) != nybble_get(buf, 41))
        || (checksum_calc(buf, 16, 31) != nybble_get(buf, 42))
        || (checksum_calc(buf, 32, 40) != nybble_get(buf, 43));
}

static BasicInfoResult lock_read_frame(uint8_t frame[BASIC_INFO_LEN],
                                       uint8_t retries = SCAN_MAX_RETRIES) {
    BasicInfoResult last = BASIC_INFO_NO_RESPONSE;
    for (uint8_t i = 0; i < retries; i++) {
        last = read_basic_info(frame);
        if (last != BASIC_INFO_NO_RESPONSE) return last;
        safe_delay(SCAN_RETRY_DELAY_MS);
    }
    return last;
}

struct LockStrategy {
    void (*mutate)(uint8_t v[BASIC_INFO_LEN], uint8_t attempt);
    bool (*verify)(const uint8_t v[BASIC_INFO_LEN]);
    void (*report)(const uint8_t v[BASIC_INFO_LEN]);
    bool (*already_locked)(const uint8_t frame[BASIC_INFO_LEN]);
    const __FlashStringHelper *already_locked_msg;
    const __FlashStringHelper *success_msg;
};

static void crc_mutate(uint8_t v[BASIC_INFO_LEN], uint8_t attempt) {
    uint8_t cs0 = (checksum_calc(v,  0, 15) + attempt) & 0xF;
    uint8_t cs1 = (checksum_calc(v, 16, 31) + attempt) & 0xF;
    uint8_t cs2 = (checksum_calc(v, 32, 40) + attempt) & 0xF;
    nybble_set(v, 41, cs0);
    nybble_set(v, 42, cs1);
    nybble_set(v, 40, cs2);  // corrupt nybble 40 (failure code, inside range 32..40)
    nybble_set(v, 43, cs2);
}
static bool crc_verify(const uint8_t v[BASIC_INFO_LEN]) { return checksums_bad(v); }
static void crc_report(const uint8_t v[BASIC_INFO_LEN]) {
    Serial.print(F("  [Lock] Checksums : "));
    print_checksums(checksum_calc(v, 0,  15) == nybble_get(v, 41),
                    checksum_calc(v, 16, 31) == nybble_get(v, 42),
                    checksum_calc(v, 32, 40) == nybble_get(v, 43));
}
static bool crc_already_locked(const uint8_t frame[BASIC_INFO_LEN])  { return checksums_bad(frame); }

static void dead_mutate(uint8_t v[BASIC_INFO_LEN], uint8_t) { nybble_set(v, 40, FC_DEAD); }
static bool dead_verify(const uint8_t v[BASIC_INFO_LEN])    { return nybble_get(v, 40) == FC_DEAD; }
static void dead_report(const uint8_t v[BASIC_INFO_LEN]) {
    Serial.print(F("  [Lock] Failure code nybble 40 = ")); Serial.println(nybble_get(v, 40));
}
static bool dead_already_locked(const uint8_t frame[BASIC_INFO_LEN]) { return nybble_get(frame, 40) == FC_DEAD; }

static const LockStrategy STRAT_CRC  = { crc_mutate,  crc_verify,  crc_report,  crc_already_locked,
    F("  [Lock] Battery is already CRC LOCKED."),   F("  [Lock] CRC LOCKED.")  };
static const LockStrategy STRAT_DEAD = { dead_mutate, dead_verify, dead_report, dead_already_locked,
    F("  [Lock] Battery already has failure code 15."), F("  [Lock] DEAD LOCKED.") };

static bool lock_apply(uint8_t frame[BASIC_INFO_LEN], const LockStrategy &s) {
    uint8_t v[BASIC_INFO_LEN];
    memcpy(v, frame, BASIC_INFO_LEN);
    for (uint8_t attempt = 1; attempt <= LOCK_MAX_ATTEMPTS; attempt++) {
        Serial.print(F("  [Lock] Attempt ")); Serial.println(attempt);
        wdt_kick();
        s.mutate(v, attempt);
        led_purple();
        if (!do_protected_write(v, F("  [Lock] "))) return false;
        power_cycle_bus();
        if (lock_read_frame(v, VERIFY_RETRIES) != BASIC_INFO_OK) {
            Serial.println(F("  [Lock] No response.")); return false;
        }
        s.report(v);
        if (s.verify(v)) return true;
        memcpy(v, frame, BASIC_INFO_LEN);
    }
    Serial.println(F("  [Lock] Failed after all attempts."));
    return false;
}

static bool run_lock_common(const LockStrategy &s) {
    if (g_in_testmode) exit_testmode();
    power_cycle_bus(); led_blue();

    uint8_t rom_id[8] = {0};
    bool rom_ok = read_rom_id(rom_id);

    uint8_t frame[BASIC_INFO_LEN] = {0};
    BasicInfoResult fr = lock_read_frame(frame);
    if (fr != BASIC_INFO_OK) {
        if (fr == BASIC_INFO_PRE_TYPE0) { Serial.println(F("  [Lock] Pre-type-0 HC08 - unsupported.")); led_yellow(); }
        else                            { Serial.println(F("  [Lock] ERROR: No valid frame."));          led_flash(COL_RED); }
        bus_disable(); return false;
    }
    if (!rom_ok) Serial.println(F("  [Lock] WARNING: ROM ID read failed; type detection may be unreliable."));

    int8_t type = lock_detect_type(rom_id, frame);
    Serial.print(F("  [Lock] Type: "));
    if (type < 0) { Serial.println(F("unsupported (5, 6, or unknown).")); led_yellow(); bus_disable(); return false; }
    Serial.println(type);

    if (s.already_locked(frame)) { Serial.println(s.already_locked_msg); led_flash(COL_RED); bus_disable(); return true; }

    bool ok = lock_apply(frame, s);
    print_sep();
    Serial.println(ok ? s.success_msg : F("  [Lock] FAILED."));
    print_sep();
    led_result(ok); bus_disable();
    return ok;
}

static bool run_lock()      { return run_lock_common(STRAT_CRC);  }
static bool run_dead_lock() { return run_lock_common(STRAT_DEAD); }

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
    step_handle_lock(info, data32);
    print_sep(); Serial.println(F("Complete."));
    bus_disable(); g_charger_arm_issued = false;
    return true;
}

// ─── Hot-swap state machine ───────────────────────────────────
enum ScanState { WAIT_BATTERY, SCAN_PENDING, IDLE, UNSUPPORTED };
static ScanState  g_state         = WAIT_BATTERY;
static uint32_t   g_last_poll     = 0;
static uint32_t   g_last_pulse    = 0;
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
    pinMode(PIN_MODE_IN1,  INPUT_PULLDOWN);
    pinMode(PIN_MODE_IN2, INPUT_PULLDOWN);
#else
    pinMode(PIN_MODE_OUT, OUTPUT); digitalWrite(PIN_MODE_OUT, LOW);
    pinMode(PIN_MODE_IN1,  INPUT_PULLUP);
    pinMode(PIN_MODE_IN2, INPUT_PULLUP);
#endif
    g_pixel.begin(); g_pixel.setBrightness(LED_BRIGHTNESS_MAX); led_off();
    delay(500);

    print_sep();
    Serial.print(F("  Firmware v")); Serial.println(F(FIRMWARE_VERSION));
    DeviceMode m = mode_read();
    switch (m) {
        case MODE_LOCK_DEAD:
            Serial.println(F("  Makita DEAD LOCK Utility - Types 0 / 2 / 3 only"));
            Serial.println(F("  GPIO0-GPIO1 bridged -> DEAD LOCK MODE (nybble 40 = 15)"));
            Serial.println(F("  Remove bridge to switch to scan/unlock mode."));
            break;
        case MODE_LOCK_CRC:
            Serial.println(F("  Makita CRC LOCK Utility - Types 0 / 2 / 3 only"));
            Serial.println(F("  GPIO0-GPIO2 bridged -> CRC LOCK MODE"));
            Serial.println(F("  Remove bridge to switch to scan/unlock mode."));
            break;
        default:
            Serial.println(F("  Makita Monitor - Scan / Unlock Mode"));
            Serial.println(F("  's' = rescan | auto-detects connect/disconnect"));
            Serial.println(F("  Bridge GPIO0-GPIO1 for dead lock, GPIO0-GPIO2 for CRC lock."));
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
                    case MODE_LOCK_DEAD:
                        Serial.println(F("  Mode changed -> DEAD LOCK MODE"));
                        Serial.println(F("  GPIO0-GPIO1 bridged. Insert battery to lock."));
                        break;
                    case MODE_LOCK_CRC:
                        Serial.println(F("  Mode changed -> CRC LOCK MODE"));
                        Serial.println(F("  GPIO0-GPIO2 bridged. Insert battery to lock."));
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
            switch (cur_mode) {
                case MODE_LOCK_CRC:  Serial.println(F("  [Lock] Battery detected - CRC locking..."));  break;
                case MODE_LOCK_DEAD: Serial.println(F("  [Lock] Battery detected - dead locking...")); break;
                default:             Serial.println(F("  Battery detected - starting scan."));          break;
            }
            print_sep();
            g_state = SCAN_PENDING;
        } else if ((g_state==IDLE || g_state==UNSUPPORTED) && !present) {
            print_sep();
            if (cur_mode == MODE_LOCK_CRC || cur_mode == MODE_LOCK_DEAD)
                Serial.println(F("  [Lock] Battery removed. Waiting for next..."));
            else
                Serial.println(F("  Battery removed. Waiting..."));
            print_sep();
            led_off(); g_state = WAIT_BATTERY;
        }
    }

    // Idle pulse - white=scan, red=lock
    if (g_state==WAIT_BATTERY && now-g_last_pulse>=LED_PULSE_INTERVAL_MS) {
        g_last_pulse = now;
        led_pulse(cur_mode == MODE_SCAN ? COL_WHITE : COL_RED);
    }

    // Execute pending action
    if (g_state==SCAN_PENDING) {
        bool ok;
        if      (cur_mode == MODE_LOCK_CRC)  ok = run_lock();
        else if (cur_mode == MODE_LOCK_DEAD) ok = run_dead_lock();
        else                                 ok = run_scan();
        g_state = ok ? IDLE : UNSUPPORTED;
        if (!ok) Serial.println(F("  Remove battery and try again."));
    }
}
