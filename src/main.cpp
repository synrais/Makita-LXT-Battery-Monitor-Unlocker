// ============================================================
//  Makita Battery Monitor — Production Firmware
//  Target: Waveshare RP2040 Zero
//  Build:   PlatformIO + Adafruit NeoPixel + OneWire
// ============================================================

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "OneWire2.h"

// ─────────────────────────────────────────────
//  Pin definitions
// ─────────────────────────────────────────────
static const uint8_t PIN_ONEWIRE  = 6;
static const uint8_t PIN_ENABLE   = 8;
static const uint8_t NEOPIXEL_OUT_PIN = 16;
static const uint8_t NUM_PIXELS   = 1;

// ─────────────────────────────────────────────
//  Protocol constants
// ─────────────────────────────────────────────

// Inter-byte gap on the 1-Wire bus (µs).
// Measured empirically against BTC04 / DC18RC traffic.
static const uint16_t BUS_BYTE_GAP_US   = 100;
// Post-write settle before reading response (µs).
static const uint16_t BUS_SETTLE_US     = 150;
// Bus enable stabilisation (ms) — lets BMS decoupling caps charge.
static const uint16_t BUS_ENABLE_MS     = 100;
// Shorter stabilisation used for presence-check only.
static const uint16_t BUS_PRESENCE_MS   = 50;
// Power-cycle timings (ms).
static const uint16_t POWERCYCLE_OFF_MS = 150;
static const uint16_t POWERCYCLE_ON_MS  = 200;

// Basic-info command.
static const uint8_t CMD_BASIC_INFO[]  = { 0xAA, 0x00 };
static const uint8_t BASIC_INFO_LEN   = 32;

// Magic byte in the last position of a type-probe response that identifies a type.
static const uint8_t TYPE_PROBE_MAGIC  = 0x06;

// Model read.
static const uint8_t CMD_MODEL[]       = { 0xDC, 0x0C };

// Test-mode enter / exit.
static const uint8_t CMD_TESTMODE_ENTER[] = { 0xD9, 0x96, 0xA5 };
static const uint8_t CMD_TESTMODE_EXIT[]  = { 0xD9, 0xFF, 0xFF };

// Battery LED control (requires test mode).
static const uint8_t CMD_LEDS_ON[]  = { 0xDA, 0x31 };
static const uint8_t CMD_LEDS_OFF[] = { 0xDA, 0x34 };
static const uint8_t LED_CMD_RSP_LEN = 9;

// Error-reset command (DA 04) — asks BMS to self-correct checksums.
static const uint8_t CMD_RESET_ERRORS[] = { 0xDA, 0x04 };

// Charger-mode arm — sent once before a frame write.
// WARNING: issuing this twice without cycling ENABLE locks the bus.
// g_charger_arm_issued guards against re-issue within one session.
static const uint8_t CMD_CHARGER_ARM[] = { 0xF0, 0x00 };

// Frame write opcode and padding byte (sent via 0x33 ROM path).
// 0x0F is the BMS data-write command — confirmed from OBI-1 source (CLEAN_FRAME_CMD).
// The 0x00 padding byte is required between the opcode and the 32-byte payload;
// without it the BMS misinterprets the first payload byte as a command parameter.
static const uint8_t FRAME_WRITE_OPCODE = 0x0F;
static const uint8_t FRAME_WRITE_PAD    = 0x00;
static const uint8_t CMD_STORE[]         = { 0x55, 0xA5 };

// Temperature: raw value indicating sensor absent / command unsupported.
static const float TEMP_INVALID = -999.0f;

// Kelvin offset for 1/10 K fixed-point temperature format.
static const float KELVIN_OFFSET = 273.15f;

// Type-6 temperature formula coefficients: t = (A + B*raw) / 100
static const float TEMP6_A =  9323.0f;
static const float TEMP6_B =   -40.0f;

// Health scale denominators (charge cycles to full wear).
// Capacity codes 26, 28, 40, 50 use the extended 1000-cycle scale;
// all others use 600. Determined from BTC04 reference implementation.
static const uint8_t  HEALTH_SCALE_EXTENDED_CODES[] = { 26, 28, 40, 50 };
static const uint16_t HEALTH_SCALE_STANDARD  = 600;
static const uint16_t HEALTH_SCALE_EXTENDED  = 1000;
static const float    HEALTH_MAX             = 4.0f;

// State-of-charge denominator:
//   charge_level / capacity_raw / SOC_DIVISOR gives a 0–70 ratio,
//   clamped to 1–7 segments. Value derived from BTC04 coulomb-counter
//   calibration: 1 Ah = 3600 C; internal counter unit = 1.25 mC;
//   so 1 Ah ≈ 2880 counts.
static const float SOC_DIVISOR = 2880.0f;

// ─────────────────────────────────────────────
//  Scan timing
// ─────────────────────────────────────────────
static const uint16_t SCAN_RETRY_DELAY_MS  = 200;
static const uint8_t  SCAN_MAX_RETRIES     = 15;
static const uint16_t POLL_INTERVAL_MS     = 800;
static const uint8_t  LED_PULSE_INTERVAL_MS = 20;

// ─────────────────────────────────────────────
//  Return codes
// ─────────────────────────────────────────────
enum BasicInfoResult {
    BASIC_INFO_NO_RESPONSE  =  0,
    BASIC_INFO_OK           =  1,
    BASIC_INFO_PRE_TYPE0    = -1,   // Freescale HC08 — all-0xFF response
};

enum BusResult {
    BUS_OK          = 0,
    BUS_NO_PRESENCE = 1,
};

// ─────────────────────────────────────────────
//  Battery types
// ─────────────────────────────────────────────
enum BatteryType {
    BATT_TYPE_UNKNOWN = -1,
    BATT_TYPE_0 = 0,
    BATT_TYPE_2 = 2,
    BATT_TYPE_3 = 3,
    BATT_TYPE_5 = 5,
    BATT_TYPE_6 = 6,
};

// ─────────────────────────────────────────────
//  Battery data — type-generic fields only.
//  Raw protocol values kept separate from derived.
// ─────────────────────────────────────────────
struct RawBasicInfo {
    uint8_t  batt_type;       // nybbles 22-23
    uint8_t  capacity;        // nybbles 32-33
    uint8_t  flags;           // nybbles 34-35
    uint8_t  failure_code;    // nybble 40
    uint8_t  overdischarge;   // nybbles 48-49
    uint8_t  overload;        // nybbles 50-51
    uint16_t cycles;          // nybbles 52-55
    bool     cell_failure;    // nybble 44 bit 2
    uint8_t  checksum_n[3];   // stored nybbles 41, 42, 43
    uint8_t  aux_checksum_n[2]; // stored nybbles 62, 63
};

struct HealthData {
    float    rating;          // 0.0 – 4.0
    uint16_t overload_count;  // sum of all overload event counters
    uint8_t  od_count;        // overdischarge event count
    uint32_t charge_level;    // coulomb counter raw value
};

struct BatteryInfo {
    char          model[8];
    uint8_t       rom_id[8];
    BatteryType   type;
    RawBasicInfo  raw;
    HealthData    health;
    float         temperature_c;        // Cell / primary NTC sensor
    float         temperature_mosfet_c; // MOSFET sensor (types 0/2/3 only)
    uint8_t       cell_count;
    float         capacity_ah;
    bool          locked;
    bool          checksums_ok[3];
    bool          aux_checksums_ok[2];
};

// ─────────────────────────────────────────────
//  Globals
// ─────────────────────────────────────────────
static OneWire          g_ow(PIN_ONEWIRE);
static Adafruit_NeoPixel g_pixel(NUM_PIXELS, NEOPIXEL_OUT_PIN, NEO_GRB + NEO_KHZ800);

// Charger-arm guard: set true after CMD_CHARGER_ARM is issued.
// Cleared only by power_cycle_bus(). Prevents double-arm corruption.
static bool g_charger_arm_issued = false;

// ─────────────────────────────────────────────
//  NeoPixel helpers
// ─────────────────────────────────────────────
static void led_set(uint8_t r, uint8_t g, uint8_t b) {
    g_pixel.setPixelColor(0, g_pixel.Color(r, g, b));
    g_pixel.show();
}
static void led_off()    { led_set(0,   0,   0); }
static void led_green()  { led_set(0,  80,   0); }
static void led_yellow() { led_set(80, 60,   0); }
static void led_blue()   { led_set(0,   0,  80); }
static void led_red()    { led_set(80,  0,   0); }
static void led_purple() { led_set(80,  0,  80); }

// Triangle-wave white pulse: 0→80→0 over 2 s.
static void led_pulse_white() {
    uint32_t t = millis() % 2000UL;
    uint8_t  v = (t < 1000) ? (uint8_t)(t * 80 / 1000)
                             : (uint8_t)((2000UL - t) * 80 / 1000);
    led_set(v, v, v);
}

// ─────────────────────────────────────────────
//  Bus control
// ─────────────────────────────────────────────
static void bus_enable(uint16_t stabilise_ms = BUS_ENABLE_MS) {
    digitalWrite(PIN_ENABLE, HIGH);
    delay(stabilise_ms);
}

static void bus_disable() {
    digitalWrite(PIN_ENABLE, LOW);
}

// Power-cycle: drop bus so BMS caps discharge, then re-assert.
// Always clears the charger-arm guard.
static void power_cycle_bus(uint16_t off_ms = POWERCYCLE_OFF_MS,
                             uint16_t on_ms  = POWERCYCLE_ON_MS) {
    bus_disable();
    g_charger_arm_issued = false;
    delay(off_ms);
    bus_enable(on_ms);
}

// Non-destructive presence check. Uses raw GPIO rather than bus_enable()
// to avoid the longer BUS_ENABLE_MS stabilisation delay — we only need
// the BMS caps settled enough to see a presence pulse, not to run a full
// transaction.
static bool battery_present() {
    digitalWrite(PIN_ENABLE, HIGH);
    delay(BUS_PRESENCE_MS);
    bool present = (g_ow.reset() == 1);
    digitalWrite(PIN_ENABLE, LOW);
    return present;
}

// ─────────────────────────────────────────────
//  Low-level 1-Wire transactions
//
//  All functions return BusResult so callers can detect
//  missing-presence without silently reading zeroed buffers.
//
//  Convention:
//    cmd_cc  — Skip ROM (0xCC), write cmd, read rsp.
//    cmd_33  — Read ROM (0x33), read 8 ROM bytes into rsp[0..7],
//               write cmd, read rsp_len bytes into rsp[8..].
//              Caller MUST size rsp >= 8 + rsp_len.
//
//  _raw variants: bus already held HIGH by caller. Used for
//  multi-step sequences where ENABLE must not drop between commands.
// ─────────────────────────────────────────────
static BusResult cmd_cc(const uint8_t *cmd, uint8_t cmd_len,
                         uint8_t *rsp,       uint8_t rsp_len) {
    bus_enable();
    if (!g_ow.reset()) { bus_disable(); return BUS_NO_PRESENCE; }
    g_ow.write(0xCC, 0);
    for (uint8_t i = 0; i < cmd_len; i++) {
        delayMicroseconds(BUS_BYTE_GAP_US);
        g_ow.write(cmd[i], 0);
    }
    delayMicroseconds(BUS_SETTLE_US);
    for (uint8_t i = 0; i < rsp_len; i++) {
        delayMicroseconds(BUS_BYTE_GAP_US);
        rsp[i] = g_ow.read();
    }
    bus_disable();
    return BUS_OK;
}

static BusResult cmd_cc_raw(const uint8_t *cmd, uint8_t cmd_len,
                              uint8_t *rsp,       uint8_t rsp_len) {
    if (!g_ow.reset()) return BUS_NO_PRESENCE;
    g_ow.write(0xCC, 0);
    for (uint8_t i = 0; i < cmd_len; i++) {
        delayMicroseconds(BUS_BYTE_GAP_US);
        g_ow.write(cmd[i], 0);
    }
    delayMicroseconds(BUS_SETTLE_US);
    for (uint8_t i = 0; i < rsp_len; i++) {
        delayMicroseconds(BUS_BYTE_GAP_US);
        rsp[i] = g_ow.read();
    }
    return BUS_OK;
}

// rsp must be sized >= 8 + rsp_len.
static BusResult cmd_33(const uint8_t *cmd, uint8_t cmd_len,
                         uint8_t *rsp,       uint8_t rsp_len) {
    bus_enable();
    if (!g_ow.reset()) { bus_disable(); return BUS_NO_PRESENCE; }
    g_ow.write(0x33, 0);
    for (uint8_t i = 0; i < 8; i++) {
        delayMicroseconds(BUS_BYTE_GAP_US);
        rsp[i] = g_ow.read();
    }
    for (uint8_t i = 0; i < cmd_len; i++) {
        delayMicroseconds(BUS_BYTE_GAP_US);
        g_ow.write(cmd[i], 0);
    }
    delayMicroseconds(BUS_SETTLE_US);
    for (uint8_t i = 0; i < rsp_len; i++) {
        delayMicroseconds(BUS_BYTE_GAP_US);
        rsp[8 + i] = g_ow.read();
    }
    bus_disable();
    return BUS_OK;
}

// rsp must be sized >= 8 + rsp_len.
static BusResult cmd_33_raw(const uint8_t *cmd, uint8_t cmd_len,
                              uint8_t *rsp,       uint8_t rsp_len) {
    if (!g_ow.reset()) return BUS_NO_PRESENCE;
    g_ow.write(0x33, 0);
    for (uint8_t i = 0; i < 8; i++) {
        delayMicroseconds(BUS_BYTE_GAP_US);
        rsp[i] = g_ow.read();
    }
    for (uint8_t i = 0; i < cmd_len; i++) {
        delayMicroseconds(BUS_BYTE_GAP_US);
        g_ow.write(cmd[i], 0);
    }
    delayMicroseconds(BUS_SETTLE_US);
    for (uint8_t i = 0; i < rsp_len; i++) {
        delayMicroseconds(BUS_BYTE_GAP_US);
        rsp[8 + i] = g_ow.read();
    }
    return BUS_OK;
}

// ─────────────────────────────────────────────
//  Nybble helpers (32-byte frame, LSN first)
// ─────────────────────────────────────────────
static uint8_t nybble_get(const uint8_t *data, uint8_t n) {
    return (n % 2 == 0) ? (data[n / 2] & 0x0F) : ((data[n / 2] >> 4) & 0x0F);
}

static void nybble_set(uint8_t *data, uint8_t n, uint8_t val) {
    val &= 0x0F;
    if (n % 2 == 0) data[n / 2] = (data[n / 2] & 0xF0) | val;
    else            data[n / 2] = (data[n / 2] & 0x0F) | (val << 4);
}

static uint8_t nybble_pair(const uint8_t *data, uint8_t hi, uint8_t lo) {
    return (uint8_t)((nybble_get(data, hi) << 4) | nybble_get(data, lo));
}

// Sum of nybble values from index start_n to end_n (inclusive), clamped to 0x0F.
static uint8_t checksum_calc(const uint8_t *data, uint8_t start_n, uint8_t end_n) {
    uint16_t sum = 0;
    for (uint8_t i = start_n; i <= end_n; i++) sum += nybble_get(data, i);
    return (uint8_t)(min(sum, (uint16_t)0xFF) & 0x0F);
}

static inline uint16_t le16(const uint8_t *b) {
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

// ─────────────────────────────────────────────
//  Utility printing
// ─────────────────────────────────────────────
static void print_sep() {
    Serial.println(F("--------------------------------------------"));
}

static void print_float(const __FlashStringHelper *label, float val,
                         const __FlashStringHelper *unit, uint8_t dp = 3) {
    Serial.print(label);
    Serial.print(val, dp);
    Serial.println(unit);
}

// ─────────────────────────────────────────────
//  Test mode
//
//  enter_testmode / exit_testmode are always paired.
//  enter_testmode sets a guard flag; exit_testmode clears it
//  and is safe to call even if enter was never called (no-op).
// ─────────────────────────────────────────────
static bool g_in_testmode = false;

static BusResult enter_testmode() {
    uint8_t rsp[1] = {0};
    BusResult r = cmd_cc(CMD_TESTMODE_ENTER, sizeof(CMD_TESTMODE_ENTER), rsp, 1);
    if (r == BUS_OK) { g_in_testmode = true; delay(20); }
    return r;
}

static void exit_testmode() {
    if (!g_in_testmode) return;
    uint8_t rsp[1] = {0};
    cmd_cc(CMD_TESTMODE_EXIT, sizeof(CMD_TESTMODE_EXIT), rsp, 1);
    g_in_testmode = false;
    delay(20);
}

// ─────────────────────────────────────────────
//  Battery LED flash (test mode required internally)
//
//  Manages test mode entry and exit itself.
//  Returns false if bus presence lost mid-sequence.
// ─────────────────────────────────────────────
static bool flash_battery_leds(uint8_t times, uint16_t on_ms = 150, uint16_t off_ms = 100) {
    // Sized for: 8 ROM bytes + LED_CMD_RSP_LEN response bytes.
    uint8_t rsp_tm[8 + LED_CMD_RSP_LEN] = {0};
    uint8_t rsp_on[8 + LED_CMD_RSP_LEN] = {0};
    uint8_t rsp_off[8 + LED_CMD_RSP_LEN] = {0};

    for (uint8_t i = 0; i < times; i++) {
        bus_enable(BUS_PRESENCE_MS);

        if (cmd_33_raw(CMD_TESTMODE_ENTER, sizeof(CMD_TESTMODE_ENTER), rsp_tm, LED_CMD_RSP_LEN) != BUS_OK) {
            bus_disable();
            return false;
        }
        if (cmd_33_raw(CMD_LEDS_ON, sizeof(CMD_LEDS_ON), rsp_on, LED_CMD_RSP_LEN) != BUS_OK) {
            bus_disable();
            return false;
        }
        delay(on_ms);
        if (cmd_33_raw(CMD_TESTMODE_ENTER, sizeof(CMD_TESTMODE_ENTER), rsp_tm, LED_CMD_RSP_LEN) != BUS_OK) {
            bus_disable();
            return false;
        }
        if (cmd_33_raw(CMD_LEDS_OFF, sizeof(CMD_LEDS_OFF), rsp_off, LED_CMD_RSP_LEN) != BUS_OK) {
            bus_disable();
            return false;
        }
        bus_disable();
        delay(off_ms);
    }

    // _raw commands do not set g_in_testmode, so exit_testmode() is not
    // needed here. power_cycle_bus() resets the BMS out of test mode.
    power_cycle_bus();
    return true;
}

// ─────────────────────────────────────────────
//  Basic info read
// ─────────────────────────────────────────────
static BasicInfoResult read_basic_info(uint8_t data32[BASIC_INFO_LEN]) {
    BusResult r = cmd_cc(CMD_BASIC_INFO, sizeof(CMD_BASIC_INFO), data32, BASIC_INFO_LEN);
    if (r != BUS_OK) return BASIC_INFO_NO_RESPONSE;

    uint8_t or_val = 0, and_val = 0xFF;
    for (uint8_t i = 0; i < BASIC_INFO_LEN; i++) { or_val |= data32[i]; and_val &= data32[i]; }
    if (or_val  == 0x00) return BASIC_INFO_NO_RESPONSE;
    if (and_val == 0xFF) return BASIC_INFO_PRE_TYPE0;
    return BASIC_INFO_OK;
}

// ─────────────────────────────────────────────
//  ROM ID read
// ─────────────────────────────────────────────
static void read_rom_id(uint8_t rom_out[8]) {
    // cmd_33 unconditionally reads the 8 ROM bytes into rsp[0..7] before
    // sending any command. We reuse CMD_BASIC_INFO as the payload so the
    // BMS also returns useful data rather than an undefined response,
    // but only the first 8 bytes (the ROM ID) are retained.
    uint8_t rsp[8 + BASIC_INFO_LEN] = {0};
    cmd_33(CMD_BASIC_INFO, sizeof(CMD_BASIC_INFO), rsp, BASIC_INFO_LEN);
    memcpy(rom_out, rsp, 8);
}

// ─────────────────────────────────────────────
//  Model read
// ─────────────────────────────────────────────

// Returns true if the model string looks like a Makita part number.
static bool model_looks_valid(const char *m) {
    // All known model strings begin with "BL" or "DC".
    return strlen(m) >= 4 &&
           ((m[0] == 'B' && m[1] == 'L') ||
            (m[0] == 'D' && m[1] == 'C'));
}

static bool read_model(char model_out[8]) {
    uint8_t rsp[16] = {0};
    if (cmd_cc(CMD_MODEL, sizeof(CMD_MODEL), rsp, 16) != BUS_OK) {
        model_out[0] = '\0';
        return false;
    }
    for (uint8_t i = 0; i < 7; i++) {
        uint8_t c = rsp[i];
        if (c < 0x20 || c >= 0x7F) { model_out[i] = '\0'; break; }
        model_out[i] = (char)c;
    }
    model_out[7] = '\0';
    return model_looks_valid(model_out);
}

// Type-5 (F0513) model: secondary command tree (cc 99), then (cc 31).
static bool read_model_type5(char model_out[8]) {
    static const uint8_t enter[] = { 0x99 };
    uint8_t ignored[1] = {0};
    if (cmd_cc(enter, sizeof(enter), ignored, 0) != BUS_OK) {
        snprintf(model_out, 8, "BLxxxx");
        return false;
    }
    static const uint8_t cmd[] = { 0x31 };
    uint8_t rsp[2] = {0};
    if (cmd_cc(cmd, sizeof(cmd), rsp, 2) != BUS_OK) {
        snprintf(model_out, 8, "BLxxxx");
        return false;
    }
    snprintf(model_out, 8, "BL%02X%02X", rsp[1], rsp[0]);
    return true;
}

// ─────────────────────────────────────────────
//  Battery type detection
//
//  Priority (per protocol spec):
//    1. Type 5  — ROM ID byte 3 < 100  (F0513 chip)
//    2. Type 6  — data32[17] == 30
//    3. Type 0  — cc dc 0b → last byte 0x06
//    4. Type 2  — cc dc 0a IN TEST MODE → last byte 0x06
//    5. Type 3  — cc d4 2c 00 02 → last byte 0x06
//    6. Default — type 0
// ─────────────────────────────────────────────
static BatteryType detect_battery_type(const uint8_t rom_id[8],
                                        const uint8_t data32[BASIC_INFO_LEN]) {
    if (rom_id[3] < 100) return BATT_TYPE_5;
    if (data32[17] == 30) return BATT_TYPE_6;

    {
        static const uint8_t cmd[] = { 0xDC, 0x0B };
        uint8_t rsp[17] = {0};
        if (cmd_cc(cmd, sizeof(cmd), rsp, 17) == BUS_OK && rsp[16] == TYPE_PROBE_MAGIC)
            return BATT_TYPE_0;
    }

    {
        if (enter_testmode() == BUS_OK) {
            static const uint8_t cmd[] = { 0xDC, 0x0A };
            uint8_t rsp[17] = {0};
            BusResult r = cmd_cc(cmd, sizeof(cmd), rsp, 17);
            exit_testmode();
            delay(150);
            if (r == BUS_OK && rsp[16] == TYPE_PROBE_MAGIC) return BATT_TYPE_2;
        }
    }

    {
        static const uint8_t cmd[] = { 0xD4, 0x2C, 0x00, 0x02 };
        uint8_t rsp[3] = {0};
        if (cmd_cc(cmd, sizeof(cmd), rsp, 3) == BUS_OK && rsp[2] == TYPE_PROBE_MAGIC)
            return BATT_TYPE_3;
    }

    return BATT_TYPE_0;
}

// ─────────────────────────────────────────────
//  Parse basic info frame into RawBasicInfo
// ─────────────────────────────────────────────
static void parse_basic_info(const uint8_t data32[BASIC_INFO_LEN],
                               RawBasicInfo &out) {
    out.batt_type    = nybble_pair(data32, 22, 23);
    out.capacity     = nybble_pair(data32, 32, 33);
    out.flags        = nybble_pair(data32, 34, 35);
    out.failure_code = nybble_get(data32, 40);
    out.overdischarge = nybble_pair(data32, 48, 49);
    out.overload      = nybble_pair(data32, 50, 51);
    out.cell_failure  = (nybble_get(data32, 44) & 0x04) != 0;

    out.cycles = ((uint16_t)(nybble_get(data32, 52) & 0x1) << 12)
               | ((uint16_t) nybble_get(data32, 53)         << 8)
               | ((uint16_t) nybble_get(data32, 54)         << 4)
               |  (uint16_t) nybble_get(data32, 55);

    out.checksum_n[0]   = nybble_get(data32, 41);
    out.checksum_n[1]   = nybble_get(data32, 42);
    out.checksum_n[2]   = nybble_get(data32, 43);
    out.aux_checksum_n[0] = nybble_get(data32, 62);
    out.aux_checksum_n[1] = nybble_get(data32, 63);
}

// Derive high-level BatteryInfo fields from a parsed RawBasicInfo.
static void derive_battery_info(const uint8_t data32[BASIC_INFO_LEN],
                                  BatteryInfo &info) {
    const RawBasicInfo &raw = info.raw;

    if      (raw.batt_type < 13) info.cell_count = 4;
    else if (raw.batt_type < 30) info.cell_count = 5;
    else                         info.cell_count = 10;

    info.capacity_ah = raw.capacity / 10.0f;

    info.checksums_ok[0] = (checksum_calc(data32, 0,  15) == raw.checksum_n[0]);
    info.checksums_ok[1] = (checksum_calc(data32, 16, 31) == raw.checksum_n[1]);
    info.checksums_ok[2] = (checksum_calc(data32, 32, 40) == raw.checksum_n[2]);

    info.aux_checksums_ok[0] = (checksum_calc(data32, 44, 47) == raw.aux_checksum_n[0]);
    info.aux_checksums_ok[1] = (checksum_calc(data32, 48, 61) == raw.aux_checksum_n[1]);

    // Locked if any primary checksum fails, or the hard-lock sentinel pattern is set
    // (failure code nybble 40 + all three checksum nybbles 41-43 are 0xF).
    bool all_ff = (raw.checksum_n[0] == 0xF && raw.checksum_n[1] == 0xF &&
                   raw.checksum_n[2] == 0xF && raw.failure_code == 0xF);
    info.locked = all_ff ||
                  !info.checksums_ok[0] ||
                  !info.checksums_ok[1] ||
                  !info.checksums_ok[2];
}

// ─────────────────────────────────────────────
//  Health calculations
// ─────────────────────────────────────────────

// Types 5 and 6: from basic info fields only.
static float calc_health_type56(const RawBasicInfo &raw) {
    int16_t f_ol = max((int16_t)((int16_t)raw.overload    - 29), (int16_t)0);
    int16_t f_od = max((int16_t)(35 - (int16_t)raw.overdischarge), (int16_t)0);
    float   dmg  = raw.cycles + raw.cycles * (f_ol + f_od) / 32.0f;

    uint16_t scale = HEALTH_SCALE_STANDARD;
    for (uint8_t i = 0; i < sizeof(HEALTH_SCALE_EXTENDED_CODES); i++) {
        if (raw.capacity == HEALTH_SCALE_EXTENDED_CODES[i]) {
            scale = HEALTH_SCALE_EXTENDED;
            break;
        }
    }
    return max(0.0f, HEALTH_MAX - dmg / scale);
}

// Types 0, 2, 3: from dedicated health command.
static float calc_health_type023(uint16_t health_raw, uint8_t capacity_raw) {
    if (capacity_raw == 0) return 0.0f;
    float ratio = (float)health_raw / capacity_raw;
    if (ratio > 80.0f) return HEALTH_MAX;
    return max(0.0f, ratio / 10.0f - 5.0f);
}

// Old/unknown types: from damage-rating nybble 46 bits 1..3.
static float calc_health_damage_rating(const uint8_t data32[BASIC_INFO_LEN]) {
    uint8_t dmg = (nybble_get(data32, 46) >> 1) & 0x07;
    if (dmg < 3)  return HEALTH_MAX;
    if (dmg >= 7) return 0.0f;
    return max(0.0f, HEALTH_MAX - (float)(dmg - 2));
}

// ─────────────────────────────────────────────
//  Secondary reads (types 0, 2, 3)
//  All return 0 / sentinel on bus error so callers can
//  detect failure vs. a genuine zero reading.
// ─────────────────────────────────────────────
static uint16_t read_health_raw(BatteryType t) {
    uint8_t cmd[4];
    switch (t) {
        case BATT_TYPE_0: { const uint8_t c[] = {0xD4,0x50,0x01,0x02}; memcpy(cmd,c,4); break; }
        case BATT_TYPE_2: { const uint8_t c[] = {0xD6,0x04,0x05,0x02}; memcpy(cmd,c,4); break; }
        case BATT_TYPE_3: { const uint8_t c[] = {0xD6,0x38,0x02,0x02}; memcpy(cmd,c,4); break; }
        default: return 0;
    }
    uint8_t rsp[3] = {0};
    if (cmd_cc(cmd, 4, rsp, 3) != BUS_OK) return 0;
    if (rsp[2] != TYPE_PROBE_MAGIC) return 0;
    return le16(rsp);
}

static uint8_t read_od_count(BatteryType t) {
    uint8_t cmd[4];
    switch (t) {
        case BATT_TYPE_0: { const uint8_t c[] = {0xD4,0xBA,0x00,0x01}; memcpy(cmd,c,4); break; }
        case BATT_TYPE_2: { const uint8_t c[] = {0xD6,0x8D,0x05,0x01}; memcpy(cmd,c,4); break; }
        case BATT_TYPE_3: { const uint8_t c[] = {0xD6,0x09,0x03,0x01}; memcpy(cmd,c,4); break; }
        default: return 0;
    }
    uint8_t rsp[2] = {0};
    if (cmd_cc(cmd, 4, rsp, 2) != BUS_OK) return 0;
    if (rsp[1] != TYPE_PROBE_MAGIC) return 0;
    return rsp[0];
}

// Overload counters — bit-packed for type 0, byte counters for types 2/3.
static uint16_t read_overload_count(BatteryType t) {
    switch (t) {

        case BATT_TYPE_0: {
            static const uint8_t cmd[] = {0xD4, 0x8D, 0x00, 0x07};
            uint8_t rsp[8] = {0};
            if (cmd_cc(cmd, sizeof(cmd), rsp, 8) != BUS_OK) return 0;
            if (rsp[7] != TYPE_PROBE_MAGIC) return 0;
            // 10-bit counters packed across adjacent bytes.
            uint16_t a = ((uint16_t)(rsp[0] >> 6) & 0x03) | ((uint16_t)rsp[1] << 2);
            uint16_t b =  (uint16_t) rsp[3]               | (((uint16_t)rsp[4] & 0x03) << 8);
            uint16_t c = ((uint16_t)(rsp[5] >> 4))        | (((uint16_t)rsp[6] & 0x3F) << 4);
            return a + b + c;
        }

        case BATT_TYPE_2: {
            static const uint8_t cmd[] = {0xD6, 0x5F, 0x05, 0x07};
            uint8_t rsp[8] = {0};
            if (cmd_cc(cmd, sizeof(cmd), rsp, 8) != BUS_OK) return 0;
            if (rsp[7] != TYPE_PROBE_MAGIC) return 0;
            return (uint16_t)rsp[0] + rsp[2] + rsp[3] + rsp[5] + rsp[6];
        }

        case BATT_TYPE_3: {
            static const uint8_t cmd[] = {0xD6, 0x5B, 0x03, 0x04};
            uint8_t rsp[6] = {0};
            if (cmd_cc(cmd, sizeof(cmd), rsp, 6) != BUS_OK) return 0;
            if (rsp[5] != TYPE_PROBE_MAGIC) return 0;
            return (uint16_t)rsp[0] + rsp[2] + rsp[3];
        }

        default: return 0;
    }
}

// Coulomb counter — same command for types 0, 2, 3.
static uint32_t read_charge_level() {
    static const uint8_t cmd[] = { 0xD7, 0x19, 0x00, 0x04 };
    uint8_t rsp[5] = {0};
    if (cmd_cc(cmd, sizeof(cmd), rsp, 5) != BUS_OK) return 0;
    if (rsp[4] != TYPE_PROBE_MAGIC) return 0;
    return (uint32_t)rsp[0] | ((uint32_t)rsp[1] << 8)
         | ((uint32_t)rsp[2] << 16) | ((uint32_t)rsp[3] << 24);
}

// ─────────────────────────────────────────────
//  Temperature (types 5 and 6 only)
//
//  Types 0/2/3 dual-temperature is read as part of the extended
//  voltage command (read_voltages_type023) in a single transaction.
// ─────────────────────────────────────────────
static float read_temperature_type56(BatteryType t) {
    switch (t) {

        case BATT_TYPE_5: {
            // cc 52 — 2-byte LE uint16 in 1/10 K.
            static const uint8_t cmd[] = { 0x52 };
            uint8_t rsp[2] = {0};
            if (cmd_cc(cmd, sizeof(cmd), rsp, 2) != BUS_OK) return TEMP_INVALID;
            return le16(rsp) / 10.0f - KELVIN_OFFSET;
        }

        case BATT_TYPE_6: {
            // d2 — single-byte, formula: t = (-40*raw + 9323) / 100
            static const uint8_t cmd[] = { 0xD2 };
            uint8_t rsp[1] = {0};
            if (cmd_cc(cmd, sizeof(cmd), rsp, 1) != BUS_OK) return TEMP_INVALID;
            return (TEMP6_A + TEMP6_B * rsp[0]) / 100.0f;
        }

        default: return TEMP_INVALID;
    }
}

// ─────────────────────────────────────────────
//  Voltages
// ─────────────────────────────────────────────

// Types 0, 2, 3 — extended read: cc d7 00 00 ff
//
// Sends the same D7 register base as the short form but requests 0xFF
// bytes, which the BMS interprets as "give me everything from this
// register bank". The response layout (confirmed from original OBI-1
// script traces):
//
//   Bytes  0- 1  Pack voltage    (uint16 LE, mV)
//   Bytes  2- 3  Cell 1 voltage  (uint16 LE, mV)
//   Bytes  4- 5  Cell 2 voltage  (uint16 LE, mV)
//   Bytes  6- 7  Cell 3 voltage  (uint16 LE, mV)
//   Bytes  8- 9  Cell 4 voltage  (uint16 LE, mV)
//   Bytes 10-11  Cell 5 voltage  (uint16 LE, mV)
//   Bytes 12-13  Unknown / padding
//   Bytes 14-15  Cell NTC temp   (uint16 LE, 1/10 K → subtract 273.15 for °C)
//   Bytes 16-17  MOSFET NTC temp (uint16 LE, 1/10 K → subtract 273.15 for °C)
//   Bytes 18-26  Unused / unknown
//   Byte  26     Always 0x06 (TYPE_PROBE_MAGIC)
//
// Note: the temperature encoding is 1/10 Kelvin (same as the dedicated
// cc d7 0e 00 02 command which reads offset 0x0E of this same D7 register
// bank): convert with raw / 10.0 - 273.15. The original OBI-1 script used
// / 100 here, which is incorrect — that would yield raw Kelvin tenths as
// a Celsius value (e.g. 2965 instead of 23.4 °C).
//
// If the extended read fails or the magic byte is absent we fall back
// to the short 13-byte read (voltages only, no temperature).
static void read_voltages_type023(float *v_pack, float cells[10],
                                   uint8_t cell_count,
                                   float *t_cell_out,
                                   float *t_mosfet_out) {
    // Extended read: 27 payload bytes.
    // The 0xFF length byte tells the BMS to return the full D7 register block.
    // Unlike fixed-length commands (e.g. 0x0C), this response does NOT append
    // a trailing 0x06 magic byte — validate on BUS_OK and a plausible pack
    // voltage instead.
    static const uint8_t cmd_ext[] = { 0xD7, 0x00, 0x00, 0xFF };
    uint8_t rsp[27] = {0};

    bool extended_ok = (cmd_cc(cmd_ext, sizeof(cmd_ext), rsp, 27) == BUS_OK)
                       && (le16(&rsp[0]) > 0);   // pack voltage non-zero = live response

    if (extended_ok) {
        *v_pack = le16(&rsp[0]) / 1000.0f;
        uint8_t n = min(cell_count, (uint8_t)5);
        for (uint8_t i = 0; i < n; i++)
            cells[i] = le16(&rsp[2 + i * 2]) / 1000.0f;

        // Bytes 14-15: cell NTC; bytes 16-17: MOSFET NTC.
        // Both live in the D7 register bank — same encoding as the dedicated
        // cc d7 0e 00 02 temperature command: raw uint16 in 1/10 Kelvin,
        // convert to °C with: (raw / 10.0) - 273.15
        // A plausible raw value for 25 °C is ~2981 (= (25 + 273.15) * 10).
        // Values below 1000 (< -173 °C) are treated as absent/invalid.
        uint16_t raw_cell   = le16(&rsp[14]);
        uint16_t raw_mosfet = le16(&rsp[16]);

        *t_cell_out   = (raw_cell   > 1000) ? raw_cell   / 10.0f - KELVIN_OFFSET : TEMP_INVALID;
        *t_mosfet_out = (raw_mosfet > 1000) ? raw_mosfet / 10.0f - KELVIN_OFFSET : TEMP_INVALID;

    } else {
        // Fallback: short read — voltages only.
        static const uint8_t cmd_short[] = { 0xD7, 0x00, 0x00, 0x0C };
        uint8_t rsp_s[13] = {0};
        if (cmd_cc(cmd_short, sizeof(cmd_short), rsp_s, 13) != BUS_OK) return;
        if (rsp_s[12] != TYPE_PROBE_MAGIC) return;
        *v_pack = le16(&rsp_s[0]) / 1000.0f;
        uint8_t n = min(cell_count, (uint8_t)5);
        for (uint8_t i = 0; i < n; i++)
            cells[i] = le16(&rsp_s[2 + i * 2]) / 1000.0f;
        // Temperatures remain TEMP_INVALID (caller pre-initialises).
    }
}

// Type 5 (F0513) — bare commands 0x31..0x35, NO 0xCC prefix.
//
// The F0513 cell-voltage commands are bare single-byte writes on the
// 1-Wire bus — the BMS does not expect a ROM-function prefix (0xCC or
// 0x33) before them. Using cmd_cc() here would prepend 0xCC and cause
// the BMS to ignore the command entirely.
static void read_voltages_type5(float *v_pack, float cells[10],
                                  uint8_t cell_count) {
    float pack = 0;
    uint8_t n = min(cell_count, (uint8_t)5);
    bus_enable();
    for (uint8_t i = 0; i < n; i++) {
        // Bare 1-Wire write: reset, then send the command byte with no prefix.
        if (!g_ow.reset()) continue;
        delayMicroseconds(BUS_BYTE_GAP_US);
        g_ow.write((uint8_t)(0x31 + i), 0);
        delayMicroseconds(BUS_SETTLE_US);
        uint8_t rsp[2] = {0};
        rsp[0] = g_ow.read();
        delayMicroseconds(BUS_BYTE_GAP_US);
        rsp[1] = g_ow.read();
        cells[i] = le16(rsp) / 1000.0f;
        pack += cells[i];
    }
    bus_disable();
    *v_pack = pack;
}

static void read_voltages_type6(float *v_pack, float cells[10],
                                  uint8_t cell_count) {
    uint8_t ignored[1] = {0};
    uint8_t rsp[20]  = {0};

    // ENABLE must remain HIGH across both steps — the BMS drops out of
    // voltage-read mode if it sees the bus go low between commands.
    bus_enable();
    {
        static const uint8_t cmd[] = { 0x10, 0x21 };
        cmd_cc_raw(cmd, sizeof(cmd), ignored, 0);
        delay(10);
    }
    {
        static const uint8_t cmd[] = { 0xD4 };
        cmd_cc_raw(cmd, sizeof(cmd), rsp, 20);
    }
    bus_disable();

    // Conversion: v_mV = 6000 - raw/10  (raw is a 16-bit LE value).
    float pack = 0;
    uint8_t n = min(cell_count, (uint8_t)10);
    for (uint8_t i = 0; i < n; i++) {
        uint16_t raw = le16(&rsp[i * 2]);
        cells[i] = (6000.0f - raw / 10.0f) / 1000.0f;
        pack += cells[i];
    }
    *v_pack = pack;
}

// ─────────────────────────────────────────────
//  Diagnostic report
// ─────────────────────────────────────────────
static void print_report(const BatteryInfo &info, float v_pack,
                           const float cells[10],
                           const uint8_t data32[BASIC_INFO_LEN]) {
    Serial.print(F("Model          : ")); Serial.println(info.model);

    Serial.print(F("ROM ID         : "));
    for (uint8_t i = 0; i < 8; i++) {
        if (info.rom_id[i] < 0x10) Serial.print('0');
        Serial.print(info.rom_id[i], HEX);
        if (i < 7) Serial.print(' ');
    }
    Serial.println();

    // ROM ID bytes 0-2: manufacture date (YY MM DD, raw decimal).
    char date_buf[13];
    snprintf(date_buf, sizeof(date_buf), "%02d/%02d/20%02d",
             info.rom_id[2], info.rom_id[1], info.rom_id[0]);
    Serial.print(F("Mfg date       : ")); Serial.println(date_buf);

    Serial.print(F("Detected type  : ")); Serial.println((int)info.type);

    Serial.print(F("Battery type   : "));
    Serial.print(info.raw.batt_type);
    Serial.print(F("  ("));
    if      (info.raw.batt_type < 13) Serial.print(F("4 cell BL14xx"));
    else if (info.raw.batt_type < 30) Serial.print(F("5 cell BL18xx"));
    else                              Serial.print(F("10 cell BL36xx"));
    Serial.println(')');

    Serial.print(F("Capacity       : ")); Serial.print(info.capacity_ah, 1); Serial.println(F(" Ah"));

    print_sep();
    Serial.print(F("Lock status    : ")); Serial.println(info.locked ? F("LOCKED") : F("UNLOCKED"));
    Serial.print(F("Cell failure   : ")); Serial.println(info.raw.cell_failure ? F("YES") : F("No"));

    Serial.print(F("Failure code   : "));
    switch (info.raw.failure_code) {
        case 0:  Serial.println(F("0 - OK"));                    break;
        case 1:  Serial.println(F("1 - Overloaded"));            break;
        case 5:  Serial.println(F("5 - Warning"));               break;
        case 15: Serial.println(F("15 - Critical fault (hard-locked by charger)")); break;
        default: Serial.println(info.raw.failure_code);          break;
    }

    Serial.print(F("Checksum 0-15  : ")); Serial.println(info.checksums_ok[0] ? F("OK") : F("FAIL"));
    Serial.print(F("Checksum 16-31 : ")); Serial.println(info.checksums_ok[1] ? F("OK") : F("FAIL"));
    Serial.print(F("Checksum 32-40 : ")); Serial.println(info.checksums_ok[2] ? F("OK") : F("FAIL"));
    Serial.print(F("Aux CSum 44-47 : ")); Serial.println(info.aux_checksums_ok[0] ? F("OK") : F("FAIL"));
    Serial.print(F("Aux CSum 48-61 : ")); Serial.println(info.aux_checksums_ok[1] ? F("OK") : F("FAIL"));

    print_sep();
    Serial.print(F("Cycle count    : ")); Serial.println(info.raw.cycles);

    Serial.print(F("Health         : "));
    Serial.print(info.health.rating, 2);
    Serial.print(F(" / 4  ("));
    int h = (int)round(info.health.rating);
    for (int i = 0; i < 4; i++) Serial.print(i < h ? '#' : '-');
    Serial.println(')');

    if (info.type == BATT_TYPE_0 || info.type == BATT_TYPE_2 || info.type == BATT_TYPE_3) {
        Serial.print(F("OD events      : ")); Serial.println(info.health.od_count);
        Serial.print(F("Overload cnt   : ")); Serial.println(info.health.overload_count);
        if (info.health.od_count > 0 && info.raw.cycles > 0) {
            float p = 4.0f + 100.0f * info.health.od_count / info.raw.cycles;
            Serial.print(F("OD %           : ")); Serial.print(p, 1); Serial.println(F(" %"));
        }
        if (info.health.overload_count > 0 && info.raw.cycles > 0) {
            float p = 4.0f + 100.0f * info.health.overload_count / info.raw.cycles;
            Serial.print(F("Overload %     : ")); Serial.print(p, 1); Serial.println(F(" %"));
        }
    }

    if (info.type == BATT_TYPE_5 || info.type == BATT_TYPE_6) {
        float od_pct = -5.0f * info.raw.overdischarge + 160.0f;
        float ol_pct =  5.0f * info.raw.overload      - 160.0f;
        Serial.print(F("OD %           : ")); Serial.print(od_pct, 1); Serial.println(F(" %"));
        Serial.print(F("Overload %     : ")); Serial.print(ol_pct, 1); Serial.println(F(" %"));
    }

    if (info.temperature_c != TEMP_INVALID || info.temperature_mosfet_c != TEMP_INVALID) {
        if (info.temperature_c != TEMP_INVALID) {
            Serial.print(F("Temp (Cells)   : ")); Serial.print(info.temperature_c, 1); Serial.println(F(" C"));
        }
        if (info.temperature_mosfet_c != TEMP_INVALID) {
            Serial.print(F("Temp (Mosfet)  : ")); Serial.print(info.temperature_mosfet_c, 1); Serial.println(F(" C"));
        }
    }

    print_sep();
    print_float(F("Pack voltage   : "), v_pack, F(" V"));

    float v_max = cells[0], v_min = cells[0];
    for (uint8_t i = 0; i < info.cell_count; i++) {
        char label[22];
        snprintf(label, sizeof(label), "Cell %2u        : ", (unsigned)i + 1);
        Serial.print(label);
        Serial.print(cells[i], 3);
        Serial.println(F(" V"));
        if (cells[i] > v_max) v_max = cells[i];
        if (cells[i] < v_min) v_min = cells[i];
    }
    print_float(F("Cell diff      : "), v_max - v_min, F(" V"));

    if ((info.type == BATT_TYPE_0 || info.type == BATT_TYPE_2 || info.type == BATT_TYPE_3)
        && info.health.charge_level > 0 && info.raw.capacity > 0) {
        // charge_level / capacity_raw / SOC_DIVISOR gives a 0-70 ratio.
        // Clamp to 1-7 segments (display convention matches BTC04).
        float ratio = (float)info.health.charge_level / info.raw.capacity / SOC_DIVISOR;
        uint8_t soc = (ratio < 10.0f) ? 1 : (uint8_t)min((int)(ratio / 10.0f), 7);
        print_sep();
        Serial.print(F("State of charge: ")); Serial.print(soc); Serial.println(F(" / 7"));
    }

    print_sep();
    Serial.print(F("Frame          : "));
    for (uint8_t i = 0; i < BASIC_INFO_LEN; i++) {
        if (data32[i] < 0x10) Serial.print('0');
        Serial.print(data32[i], HEX);
        if (i < BASIC_INFO_LEN - 1) Serial.print(' ');
    }
    Serial.println();
}

// ─────────────────────────────────────────────
//  Frame repair
//
//  Clears the failure-code nybble (40) and recalculates all five
//  checksum nybbles in a copy of the frame, then writes it back
//  using the charger write sequence:
//
//    1. enter_testmode
//    2. CMD_CHARGER_ARM  (once — guard enforced)
//    3. 0x33 write:  [ROM ID] 0x0F 0x00 [32 frame bytes]
//    4. 0x33 store:  [ROM ID] 0x55 0xA5
//    5. exit_testmode
//    6. power_cycle_bus
//    7. verify by re-reading
//
//  Returns true if post-write checksums all pass.
// ─────────────────────────────────────────────
static bool write_corrected_frame(uint8_t data32[BASIC_INFO_LEN]) {
    uint8_t frame[BASIC_INFO_LEN];
    memcpy(frame, data32, BASIC_INFO_LEN);

    // Clear failure code before recalculating checksums.
    // Nybble 40 falls inside the checksum-32-40 range, so it must be
    // zeroed first — otherwise the written frame passes checksums but
    // still reports failure code 15 on every subsequent read.
    nybble_set(frame, 40, 0);
    nybble_set(frame, 41, checksum_calc(frame, 0,  15));
    nybble_set(frame, 42, checksum_calc(frame, 16, 31));
    nybble_set(frame, 43, checksum_calc(frame, 32, 40));
    nybble_set(frame, 62, checksum_calc(frame, 44, 47));
    nybble_set(frame, 63, checksum_calc(frame, 48, 61));

    Serial.print(F("  Frame to write: "));
    for (uint8_t i = 0; i < BASIC_INFO_LEN; i++) {
        if (frame[i] < 0x10) Serial.print('0');
        Serial.print(frame[i], HEX);
        if (i < BASIC_INFO_LEN - 1) Serial.print(' ');
    }
    Serial.println();

    if (enter_testmode() != BUS_OK) {
        Serial.println(F("  Frame write: no presence on test-mode enter."));
        return false;
    }

    // Guard: if arm was already issued this session without a power cycle,
    // refuse to proceed — issuing it again would corrupt the bus.
    // NOTE: the arm is sent in test mode here (entered above), which is
    // correct for the unlock path — the BMS accepts it from within test mode
    // when recovering checksums. Sending arm WITHOUT test mode (as the lock
    // utility does) is the normal charger sequence; with test mode is the
    // recovery sequence. Both are valid but context-dependent.
    if (g_charger_arm_issued) {
        Serial.println(F("  Frame write: charger arm already issued. Power-cycle required."));
        exit_testmode();
        return false;
    }

    {
        uint8_t rsp[BASIC_INFO_LEN] = {0};
        cmd_cc(CMD_CHARGER_ARM, sizeof(CMD_CHARGER_ARM), rsp, BASIC_INFO_LEN);
        g_charger_arm_issued = true;
        delay(10);
    }

    {
        // Build: [write opcode 0x0F] [pad 0x00] [32 frame bytes]
        uint8_t payload[2 + BASIC_INFO_LEN];
        payload[0] = FRAME_WRITE_OPCODE;
        payload[1] = FRAME_WRITE_PAD;
        memcpy(&payload[2], frame, BASIC_INFO_LEN);
        uint8_t rsp[8] = {0};
        if (cmd_33(payload, sizeof(payload), rsp, 0) != BUS_OK) {
            Serial.println(F("  Frame write: no presence on write."));
            exit_testmode();
            return false;
        }
        delay(20);
    }

    {
        uint8_t rsp[8] = {0};
        if (cmd_33(CMD_STORE, sizeof(CMD_STORE), rsp, 0) != BUS_OK) {
            Serial.println(F("  Frame write: no presence on store."));
            exit_testmode();
            return false;
        }
        delay(50);
    }

    exit_testmode();
    power_cycle_bus();   // also clears g_charger_arm_issued

    uint8_t verify[BASIC_INFO_LEN] = {0};
    if (read_basic_info(verify) != BASIC_INFO_OK) {
        Serial.println(F("  Frame write: no response during verify."));
        return false;
    }

    BatteryInfo check;
    memset(&check, 0, sizeof(check));
    parse_basic_info(verify, check.raw);
    derive_battery_info(verify, check);

    Serial.print(F("  Verify checksum 0-15  : ")); Serial.println(check.checksums_ok[0] ? F("OK") : F("FAIL"));
    Serial.print(F("  Verify checksum 16-31 : ")); Serial.println(check.checksums_ok[1] ? F("OK") : F("FAIL"));
    Serial.print(F("  Verify checksum 32-40 : ")); Serial.println(check.checksums_ok[2] ? F("OK") : F("FAIL"));



    if (!check.locked) {
        memcpy(data32, verify, BASIC_INFO_LEN);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────
//  Unlock sequence (types 0, 2, 3 only)
//
//  1. Power-cycle + DA 04 reset — BMS self-corrects if it can.
//  2. Re-read + check lock state.
//  3a. Unlocked → success.
//  3b. Checksums corrupt → write_corrected_frame.
//  3c. Otherwise → cannot recover.
// ─────────────────────────────────────────────
static bool attempt_unlock(BatteryInfo &info, uint8_t data32[BASIC_INFO_LEN]) {
    power_cycle_bus();

    if (enter_testmode() != BUS_OK) {
        Serial.println(F("  Unlock: no presence entering test mode."));
        return false;
    }
    {
        uint8_t rsp[8 + 9] = {0};
        cmd_33(CMD_RESET_ERRORS, sizeof(CMD_RESET_ERRORS), rsp, 9);
        delay(50);
    }
    exit_testmode();
    delay(50);

    if (read_basic_info(data32) != BASIC_INFO_OK) {
        Serial.println(F("  Unlock: no response after DA 04."));
        return false;
    }

    parse_basic_info(data32, info.raw);
    derive_battery_info(data32, info);

    Serial.print(F("  Checksum 0-15  : ")); Serial.println(info.checksums_ok[0] ? F("OK") : F("FAIL"));
    Serial.print(F("  Checksum 16-31 : ")); Serial.println(info.checksums_ok[1] ? F("OK") : F("FAIL"));
    Serial.print(F("  Checksum 32-40 : ")); Serial.println(info.checksums_ok[2] ? F("OK") : F("FAIL"));

    if (!info.locked) {
        Serial.println(F("  -> UNLOCKED by DA 04."));
        return true;
    }

    bool any_checksum_bad = !info.checksums_ok[0] ||
                            !info.checksums_ok[1] ||
                            !info.checksums_ok[2];
    if (any_checksum_bad) {
        Serial.println(F("  -> Checksums corrupt; DA 04 cannot fix. Trying frame write..."));
        led_purple();
        flash_battery_leds(2);
        if (write_corrected_frame(data32)) {
            parse_basic_info(data32, info.raw);
            derive_battery_info(data32, info);
            Serial.println(F("  -> Frame write succeeded. UNLOCKED."));
            return true;
        }
        Serial.println(F("  -> Frame write failed."));
        return false;
    }

    Serial.println(F("  -> Still locked (non-checksum reason). Cannot recover."));
    return false;
}

static bool type_supports_unlock(BatteryType t) {
    return t == BATT_TYPE_0 || t == BATT_TYPE_2 || t == BATT_TYPE_3;
}

// ─────────────────────────────────────────────
//  Scan steps — each focused, independently testable
// ─────────────────────────────────────────────

// Step 1: read basic info with retries.
// Returns BASIC_INFO_OK, BASIC_INFO_PRE_TYPE0, or BASIC_INFO_NO_RESPONSE.
static BasicInfoResult step_read_basic_info(uint8_t data32[BASIC_INFO_LEN]) {
    for (uint8_t attempt = 0; attempt < SCAN_MAX_RETRIES; attempt++) {
        BasicInfoResult r = read_basic_info(data32);
        if (r != BASIC_INFO_NO_RESPONSE) return r;
        delay(SCAN_RETRY_DELAY_MS);
    }
    return BASIC_INFO_NO_RESPONSE;
}

// Step 2: identify battery — type, model, ROM ID.
static void step_identify(BatteryInfo &info, const uint8_t data32[BASIC_INFO_LEN]) {
    read_rom_id(info.rom_id);
    info.type = detect_battery_type(info.rom_id, data32);

    if (info.type == BATT_TYPE_5)
        read_model_type5(info.model);
    else
        read_model(info.model);
}

// Step 3: read all voltages (and dual temperature for types 0/2/3).
static void step_read_voltages(const BatteryInfo &info, float *v_pack, float cells[10],
                                float *t_cell_out, float *t_mosfet_out) {
    switch (info.type) {
        case BATT_TYPE_5: read_voltages_type5(v_pack, cells, info.cell_count); break;
        case BATT_TYPE_6: read_voltages_type6(v_pack, cells, info.cell_count); break;
        default:          read_voltages_type023(v_pack, cells, info.cell_count,
                                                t_cell_out, t_mosfet_out);      break;
    }
}

// Step 4: read health and counters.
static void step_read_health(BatteryInfo &info, const uint8_t data32[BASIC_INFO_LEN]) {
    if (info.type == BATT_TYPE_5 || info.type == BATT_TYPE_6) {
        info.health.rating        = calc_health_type56(info.raw);
        info.health.od_count      = 0;
        info.health.overload_count = 0;
        info.health.charge_level  = 0;

    } else if (info.type == BATT_TYPE_0 || info.type == BATT_TYPE_2 || info.type == BATT_TYPE_3) {
        uint16_t health_raw       = read_health_raw(info.type);
        info.health.rating        = calc_health_type023(health_raw, info.raw.capacity);
        info.health.od_count      = read_od_count(info.type);
        info.health.overload_count = read_overload_count(info.type);
        info.health.charge_level  = read_charge_level();

    } else {
        info.health.rating        = calc_health_damage_rating(data32);
        info.health.od_count      = 0;
        info.health.overload_count = 0;
        info.health.charge_level  = 0;
    }
}

// Step 5: handle lock state.
static void step_handle_lock(BatteryInfo &info, uint8_t data32[BASIC_INFO_LEN]) {
    if (!info.locked) {
        Serial.println(F("Battery UNLOCKED."));
        led_green();
        if (type_supports_unlock(info.type)) flash_battery_leds(3);
        led_green();
        return;
    }

    Serial.print(F("Battery LOCKED. Failure code: "));
    Serial.println(info.raw.failure_code);

    if (!type_supports_unlock(info.type)) {
        Serial.print(F("Unlock not supported for type "));
        Serial.print((int)info.type);
        Serial.println(F(". No unlock attempted."));
        led_red();
        return;
    }

    Serial.println(F("Attempting DA 04 unlock..."));
    led_yellow();
    flash_battery_leds(2);

    bool unlocked = attempt_unlock(info, data32);
    if (unlocked) {
        Serial.println(F("Battery successfully unlocked."));
        led_green();
        flash_battery_leds(3);
        led_green();
    } else {
        Serial.println(F("Could not unlock battery."));
        led_red();
    }
}

// ─────────────────────────────────────────────
//  Main scan — orchestrates the steps
// ─────────────────────────────────────────────
static bool run_scan() {
    // Ensure no lingering test mode from a previous interrupted scan.
    // power_cycle_bus() clears g_charger_arm_issued as a side-effect.
    exit_testmode();
    power_cycle_bus();

    led_blue();
    Serial.println(F("============================================"));
    Serial.println(F("  Makita Monitor - Scanning..."));
    Serial.println(F("============================================"));

    BatteryInfo info;
    memset(&info, 0, sizeof(info));
    info.temperature_c        = TEMP_INVALID;
    info.temperature_mosfet_c = TEMP_INVALID;
    info.type = BATT_TYPE_UNKNOWN;

    uint8_t data32[BASIC_INFO_LEN] = {0};

    // ── Step 1: basic info ─────────────────────────────────
    BasicInfoResult basic_result = step_read_basic_info(data32);

    if (basic_result == BASIC_INFO_NO_RESPONSE) {
        Serial.println(F("ERROR: Battery not responding after retries. Aborting."));
        led_off();
        bus_disable();
        return false;
    }

    if (basic_result == BASIC_INFO_PRE_TYPE0) {
        print_sep();
        Serial.println(F("WARNING: Pre-type-0 HC08 battery detected!"));
        Serial.println(F("         Freescale MC908JK3E BMS -- no cell protection."));
        Serial.println(F("         DO NOT charge on any charger."));
        print_sep();
        led_red();
        bus_disable();
        return true;
    }

    // ── Step 2: parse + identify ───────────────────────────
    parse_basic_info(data32, info.raw);
    derive_battery_info(data32, info);
    step_identify(info, data32);

    print_sep();
    Serial.print(F("Battery found  : ")); Serial.println(info.model);
    if (type_supports_unlock(info.type)) flash_battery_leds(1);

    // ── Step 3: voltages (+ dual-temp for types 0/2/3) ────
    float v_pack = 0.0f;
    float cells[10] = {0};
    step_read_voltages(info, &v_pack, cells,
                       &info.temperature_c, &info.temperature_mosfet_c);

    // ── Step 4: temperature (types 5 and 6 only) ──────────
    // Types 0/2/3 already have both temps from the extended voltage read.
    if (info.type == BATT_TYPE_5 || info.type == BATT_TYPE_6)
        info.temperature_c = read_temperature_type56(info.type);

    // ── Step 5: health + counters ──────────────────────────
    step_read_health(info, data32);

    // ── Abort if battery was removed mid-scan ─────────────
    if (!battery_present()) {
        Serial.println(F("Battery removed during scan. Aborting."));
        led_off();
        bus_disable();
        return false;
    }

    // ── Step 6: report ─────────────────────────────────────
    print_sep();
    print_report(info, v_pack, cells, data32);
    print_sep();

    // ── Step 7: lock handling ──────────────────────────────
    step_handle_lock(info, data32);

    print_sep();
    Serial.println(F("Complete."));
    bus_disable();
    return true;
}

// ─────────────────────────────────────────────
//  Hot-swap state machine
// ─────────────────────────────────────────────
enum ScanState { WAIT_BATTERY, SCAN_PENDING, IDLE };

static ScanState     g_state       = WAIT_BATTERY;
static uint32_t      g_last_poll   = 0;
static uint32_t      g_last_pulse  = 0;

// ─────────────────────────────────────────────
//  Arduino entry points
// ─────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    pinMode(PIN_ENABLE, OUTPUT);
    digitalWrite(PIN_ENABLE, LOW);

    g_pixel.begin();
    g_pixel.setBrightness(80);
    led_off();

    delay(500);

    Serial.println(F("============================================"));
    Serial.println(F("  Makita Monitor — Production"));
    Serial.println(F("  's' = rescan | auto-detects connect/disconnect"));
    Serial.println(F("============================================"));
    Serial.println(F("Waiting for battery..."));

    g_state = WAIT_BATTERY;
    // g_last_poll, g_last_pulse, and g_state are zero-/default-initialised
    // as statics; the assignment above is explicit for readability only.
}

void loop() {
    uint32_t now = millis();

    // ── Serial command ──────────────────────────────────────
    if (Serial.available()) {
        char c = (char)Serial.read();
        if ((c == 's' || c == 'S') && g_state != SCAN_PENDING) {
            Serial.println(F("Manual rescan requested."));
            g_state = SCAN_PENDING;
        }
    }

    // ── Presence poll ───────────────────────────────────────
    if (now - g_last_poll >= POLL_INTERVAL_MS) {
        g_last_poll = now;
        bool present = battery_present();

        if (g_state == WAIT_BATTERY && present) {
            Serial.println(F("Battery detected — starting scan."));
            g_state = SCAN_PENDING;
        } else if (g_state == IDLE && !present) {
            Serial.println();
            Serial.println(F("============================================"));
            Serial.println(F("  Battery removed. Waiting..."));
            Serial.println(F("============================================"));
            led_off();
            g_state = WAIT_BATTERY;
        }
    }

    // ── White pulse while waiting ───────────────────────────
    if (g_state == WAIT_BATTERY && now - g_last_pulse >= LED_PULSE_INTERVAL_MS) {
        g_last_pulse = now;
        led_pulse_white();
    }

    // ── Execute scan ────────────────────────────────────────
    if (g_state == SCAN_PENDING) {
        bool ok = run_scan();
        // State is set after scan completes — not before — so
        // a failed scan correctly returns to WAIT_BATTERY.
        if (ok) {
            g_state = IDLE;
        } else {
            led_off();
            g_state = WAIT_BATTERY;
            Serial.println(F("Waiting for battery..."));
        }
    }
}