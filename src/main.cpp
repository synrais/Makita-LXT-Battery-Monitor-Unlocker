#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "OneWire2.h"

// ─────────────────────────────────────────────
//  Pin definitions
// ─────────────────────────────────────────────
#define ONEWIRE_PIN   6
#define ENABLE_PIN    8
#define NEOPIXEL_PIN  16
#define NEOPIXEL_NUM  1

OneWire makita(ONEWIRE_PIN);
Adafruit_NeoPixel pixel(NEOPIXEL_NUM, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// ─────────────────────────────────────────────
//  NeoPixel helpers
// ─────────────────────────────────────────────
void led_set(uint8_t r, uint8_t g, uint8_t b) {
    pixel.setPixelColor(0, pixel.Color(r, g, b));
    pixel.show();
}
void led_off()    { led_set(0,   0,  0); }
void led_green()  { led_set(0,  80,  0); }
void led_yellow() { led_set(80, 60,  0); }
void led_blue()   { led_set(0,   0, 80); }
void led_red()    { led_set(80,  0,  0); }
void led_purple() { led_set(80,  0, 80); }

// Triangle-wave white pulse: 0→80→0 over 2 s.
void led_pulse_white() {
    unsigned long t = millis() % 2000UL;
    uint8_t v = (t < 1000) ? (uint8_t)(t * 80 / 1000) : (uint8_t)((2000UL - t) * 80 / 1000);
    led_set(v, v, v);
}

// ─────────────────────────────────────────────
//  Bus enable / disable
// ─────────────────────────────────────────────
void enable_bus(uint16_t ms = 100) { digitalWrite(ENABLE_PIN, HIGH); delay(ms); }
void disable_bus()                 { digitalWrite(ENABLE_PIN, LOW);             }

// Quick non-destructive presence check.
// Pulses ENABLE high briefly and issues a 1-Wire reset.
// Returns true if a battery responds with a presence pulse.
// Safe to call at any time — does not disturb battery firmware state.
bool battery_present() {
    digitalWrite(ENABLE_PIN, HIGH);
    delay(50);                        // enough for BMS caps to stabilise
    bool present = (makita.reset() == 1);
    digitalWrite(ENABLE_PIN, LOW);
    return present;
}

// Power-cycle: drop bus, let BMS caps discharge, re-assert.
// Replicates what happens when a battery is inserted into a real charger.
void power_cycle_bus(uint16_t off_ms = 150, uint16_t on_ms = 200) {
    disable_bus();
    delay(off_ms);
    enable_bus(on_ms);
}

// ─────────────────────────────────────────────
//  Low-level 1-Wire transactions
// ─────────────────────────────────────────────
void cmd_cc(const uint8_t *cmd, uint8_t cmd_len,
            uint8_t *rsp,       uint8_t rsp_len) {
    enable_bus();
    if (!makita.reset()) { disable_bus(); return; }
    makita.write(0xCC, 0);
    delayMicroseconds(100);
    for (int i = 0; i < cmd_len; i++) { delayMicroseconds(100); makita.write(cmd[i], 0);    }
    delayMicroseconds(150);
    for (int i = 0; i < rsp_len; i++) { delayMicroseconds(100); rsp[i]     = makita.read(); }
    disable_bus();
}

void cmd_33(const uint8_t *cmd, uint8_t cmd_len,
            uint8_t *rsp,       uint8_t rsp_len) {
    enable_bus();
    if (!makita.reset()) { disable_bus(); return; }
    makita.write(0x33, 0);
    delayMicroseconds(100);
    for (int i = 0; i < 8;       i++) { delayMicroseconds(100); rsp[i]     = makita.read();  }
    for (int i = 0; i < cmd_len; i++) { delayMicroseconds(100); makita.write(cmd[i], 0);     }
    delayMicroseconds(150);
    for (int i = 0; i < rsp_len; i++) { delayMicroseconds(100); rsp[8 + i] = makita.read();  }
    disable_bus();
}

// cmd_33 with bus already held HIGH — used inside flash_leds only.
void cmd_33_raw(const uint8_t *cmd, uint8_t cmd_len,
                uint8_t *rsp,       uint8_t rsp_len) {
    if (!makita.reset()) return;
    makita.write(0x33, 0);
    delayMicroseconds(100);
    for (int i = 0; i < 8;       i++) { delayMicroseconds(100); rsp[i]     = makita.read();  }
    for (int i = 0; i < cmd_len; i++) { delayMicroseconds(100); makita.write(cmd[i], 0);     }
    delayMicroseconds(150);
    for (int i = 0; i < rsp_len; i++) { delayMicroseconds(100); rsp[8 + i] = makita.read();  }
}

// cmd_cc with bus already held HIGH — used for multi-step protocols (e.g. type 6 voltages)
// where ENABLE must not drop between sequential commands.
void cmd_cc_raw(const uint8_t *cmd, uint8_t cmd_len,
                uint8_t *rsp,       uint8_t rsp_len) {
    if (!makita.reset()) return;
    makita.write(0xCC, 0);
    delayMicroseconds(100);
    for (int i = 0; i < cmd_len; i++) { delayMicroseconds(100); makita.write(cmd[i], 0);    }
    delayMicroseconds(150);
    for (int i = 0; i < rsp_len; i++) { delayMicroseconds(100); rsp[i]     = makita.read(); }
}

// ─────────────────────────────────────────────
//  Utility
// ─────────────────────────────────────────────
void print_sep() { Serial.println(F("--------------------------------------------")); }

void print_float(const char *label, float val, const char *unit, int decimals = 3) {
    Serial.print(label);
    Serial.print(val, decimals);
    Serial.println(unit);
}

static inline uint16_t le16(const uint8_t *b) {
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

uint8_t get_nybble(const uint8_t *data, int n) {
    uint8_t byte_val = data[n / 2];
    return (n % 2 == 0) ? (byte_val & 0x0F) : ((byte_val >> 4) & 0x0F);
}

uint8_t get_byte_from_nybbles(const uint8_t *data, int high_n, int low_n) {
    return (get_nybble(data, high_n) << 4) | get_nybble(data, low_n);
}

uint8_t calc_checksum(const uint8_t *data, int start_n, int end_n) {
    uint16_t sum = 0;
    for (int i = start_n; i <= end_n; i++) sum += get_nybble(data, i);
    return (uint8_t)(min(sum, (uint16_t)0xff) & 0x0f);
}

// Write a nybble value into the correct half-byte position in a data buffer.
void set_nybble(uint8_t *data, int n, uint8_t val) {
    val &= 0x0F;
    if (n % 2 == 0) {
        data[n / 2] = (data[n / 2] & 0xF0) | val;
    } else {
        data[n / 2] = (data[n / 2] & 0x0F) | (val << 4);
    }
}

// ─────────────────────────────────────────────
//  Battery state
// ─────────────────────────────────────────────
static const float INVALID_TEMP = -999.0f;

struct BatteryInfo {
    char     model[8];
    uint8_t  rom_id[8];
    uint8_t  batt_type_raw;
    uint8_t  cell_count;
    uint8_t  capacity_raw;
    float    capacity_ah;
    uint8_t  flags;
    uint8_t  failure_code;
    uint8_t  overdischarge;       // raw value from basic info (type 5/6 pct calc)
    uint8_t  overload;            // raw value from basic info (type 5/6 pct calc)
    uint16_t cycle_count;
    bool     locked;
    bool     cell_failure;        // nybble 44 bit 2
    bool     checksums_ok[3];     // primary lock-state checksums
    bool     aux_checksums_ok[2]; // informational only — nybbles 62, 63
    int      type;
    float    health_rating;
    float    temperature_c;       // INVALID_TEMP if unavailable
    uint16_t overload_count;      // sum of overload event counters (types 0/2/3)
    uint8_t  od_count;            // overdischarge event count (types 0/2/3)
    uint32_t charge_level;        // coulomb counter (types 0/2/3)
};

// ─────────────────────────────────────────────
//  LED / test-mode commands
// ─────────────────────────────────────────────
static const uint8_t TESTMODE_DATA[]   = { 0xD9, 0x96, 0xA5 };
static const uint8_t TESTMODE_RSP_LEN  = 0x09;
static const uint8_t LEDS_ON_DATA[]    = { 0xDA, 0x31 };
static const uint8_t LEDS_ON_RSP_LEN   = 0x09;
static const uint8_t LEDS_OFF_DATA[]   = { 0xDA, 0x34 };
static const uint8_t LEDS_OFF_RSP_LEN  = 0x09;
static const uint8_t RESET_ERR_DATA[]  = { 0xDA, 0x04 };
static const uint8_t RESET_ERR_RSP_LEN = 0x09;

// Charger-mode command: cc f0 00
// This is the form used by the BTC04 and DC18RC chargers when reading
// basic info and as the prerequisite step before writing a corrected frame.
// Per the spec: repeating this command changes BMS state and stops responses
// until ENABLE is cycled — so never call it twice without a power cycle.
static const uint8_t CHARGER_CMD[]  = { 0xF0, 0x00 };

// Store command: sent via 0x33 ROM path to commit a written frame to BMS memory.
static const uint8_t STORE_CMD[]    = { 0x55, 0xA5 };

// Enter / exit test mode — shared by types 0, 2, 3.
void enter_testmode() {
    uint8_t rsp[1] = {0};
    cmd_cc(TESTMODE_DATA, sizeof(TESTMODE_DATA), rsp, 1);
    delay(20);
}

void exit_testmode() {
    uint8_t cmd[] = { 0xD9, 0xFF, 0xFF };
    uint8_t rsp[1] = {0};
    cmd_cc(cmd, sizeof(cmd), rsp, 1);
    delay(20);
}

void reset_errors() {
    uint8_t rsp[8 + RESET_ERR_RSP_LEN] = {0};
    cmd_33(RESET_ERR_DATA, sizeof(RESET_ERR_DATA), rsp, RESET_ERR_RSP_LEN);
    delay(50);
}

// Flash the battery's own LED indicators via 1-Wire.
// CRITICAL: always call exit_testmode() after the loop.
// Leaving the battery in test mode locks it: checksums fail,
// no tool power, button unresponsive.
void flash_leds(int times, int on_ms = 150, int off_ms = 100) {
    uint8_t rsp[8  + TESTMODE_RSP_LEN]  = {0};
    uint8_t r_on[8 + LEDS_ON_RSP_LEN]   = {0};
    uint8_t r_off[8 + LEDS_OFF_RSP_LEN] = {0};

    for (int i = 0; i < times; i++) {
        digitalWrite(ENABLE_PIN, HIGH);
        delay(50);
        cmd_33_raw(TESTMODE_DATA, sizeof(TESTMODE_DATA), rsp,   TESTMODE_RSP_LEN);
        cmd_33_raw(LEDS_ON_DATA,  sizeof(LEDS_ON_DATA),  r_on,  LEDS_ON_RSP_LEN);
        delay(on_ms);
        cmd_33_raw(TESTMODE_DATA, sizeof(TESTMODE_DATA), rsp,   TESTMODE_RSP_LEN);
        cmd_33_raw(LEDS_OFF_DATA, sizeof(LEDS_OFF_DATA), r_off, LEDS_OFF_RSP_LEN);
        disable_bus();
        delay(off_ms);
    }
    exit_testmode();
    power_cycle_bus(150, 200);
}

// ─────────────────────────────────────────────
//  Basic info
// ─────────────────────────────────────────────
bool read_model(char *model_out) {
    uint8_t cmd[] = { 0xDC, 0x0C };
    uint8_t rsp[16] = {0};
    cmd_cc(cmd, sizeof(cmd), rsp, 16);

    for (int i = 0; i < 7; i++) {
        uint8_t c = rsp[i];
        if (c < 0x20 || c >= 0x7F) { model_out[i] = '\0'; break; }
        model_out[i] = (char)c;
    }
    model_out[7] = '\0';
    return strlen(model_out) >= 4;
}

void read_rom_id(uint8_t *rom_out) {
    uint8_t dummy_cmd[] = { 0xAA, 0x00 };
    uint8_t rsp[8 + 0x20] = {0};
    cmd_33(dummy_cmd, sizeof(dummy_cmd), rsp, 0x20);
    memcpy(rom_out, rsp, 8);
}

// F0513 (type 5) model: enter secondary command tree (cc 99), then read 2-byte
// model code (cc 31). Format: BL{hi:02X}{lo:02X}.
void read_model_type5(char *model_out) {
    uint8_t enter[] = { 0x99 };
    uint8_t dummy[1] = {0};
    cmd_cc(enter, sizeof(enter), dummy, 0);

    uint8_t cmd[] = { 0x31 };
    uint8_t rsp[2] = {0};
    cmd_cc(cmd, sizeof(cmd), rsp, 2);
    snprintf(model_out, 8, "BL%02X%02X", rsp[1], rsp[0]);
}

// Returns: 1 = valid data, 0 = no response (all zeros), -1 = pre-type-0 HC08 (all 0xFF).
// Pre-type-0 batteries (Freescale MC908JK3E BMS) return all-0xFF and have
// NO cell monitoring or protection — they must not be charged on any charger.
int read_basic_info(uint8_t *data32) {
    uint8_t cmd[] = { 0xAA, 0x00 };
    uint8_t rsp[32] = {0};
    cmd_cc(cmd, sizeof(cmd), rsp, 32);
    uint8_t or_val = 0, and_val = 0xFF;
    for (int i = 0; i < 32; i++) { or_val |= rsp[i]; and_val &= rsp[i]; }
    if (or_val  == 0x00) return 0;
    if (and_val == 0xFF) return -1;
    memcpy(data32, rsp, 32);
    return 1;
}

// ─────────────────────────────────────────────
//  Battery type detection
//
//  Priority order (per protocol spec):
//  1. Type 5  — ROM ID byte 3 < 100 (F0513 chip)
//  2. Type 6  — data32[17] == 30  (byte 17 of cc aa 00 response = 30 decimal)
//               NOTE: The spec says to check byte 17 of the raw response,
//               NOT the decoded flags field from nybbles 34/35.
//  3. Type 0  — responds to cc dc 0b with last byte 0x06
//  4. Type 2  — responds to cc dc 0a IN TEST MODE with last byte 0x06
//               (always exit test mode before returning, regardless of result)
//  5. Type 3  — responds to cc d4 2c 00 02 with last byte 0x06
//  6. Default — type 0
// ─────────────────────────────────────────────
int detect_battery_type(const uint8_t *rom_id, const uint8_t *data32) {
    if (rom_id[3] < 100) return 5;
    if (data32[17] == 30) return 6;

    {
        uint8_t cmd[] = { 0xDC, 0x0B };
        uint8_t rsp[17] = {0};
        cmd_cc(cmd, sizeof(cmd), rsp, 17);
        if (rsp[16] == 0x06) return 0;
    }

    {
        enter_testmode();
        uint8_t cmd[] = { 0xDC, 0x0A };
        uint8_t rsp[17] = {0};
        cmd_cc(cmd, sizeof(cmd), rsp, 17);
        exit_testmode();  // always exit, even if probe succeeded
        delay(150);
        if (rsp[16] == 0x06) return 2;
    }

    {
        uint8_t cmd[] = { 0xD4, 0x2C, 0x00, 0x02 };
        uint8_t rsp[3] = {0};
        cmd_cc(cmd, sizeof(cmd), rsp, 3);
        if (rsp[2] == 0x06) return 3;
    }

    return 0;
}

// ─────────────────────────────────────────────
//  Parse basic info frame
//  32 bytes, nybble-oriented, LSN first.
// ─────────────────────────────────────────────
void parse_basic_info(const uint8_t *data32, BatteryInfo &info) {
    info.batt_type_raw = get_byte_from_nybbles(data32, 22, 23);

    if      (info.batt_type_raw < 13) info.cell_count = 4;
    else if (info.batt_type_raw < 30) info.cell_count = 5;
    else                              info.cell_count = 10;

    info.capacity_raw  = get_byte_from_nybbles(data32, 32, 33);
    info.capacity_ah   = info.capacity_raw / 10.0f;
    info.flags         = get_byte_from_nybbles(data32, 34, 35);
    info.failure_code  = get_nybble(data32, 40);
    info.overdischarge = get_byte_from_nybbles(data32, 48, 49);
    info.overload      = get_byte_from_nybbles(data32, 50, 51);

    info.cycle_count = ((uint16_t)(get_nybble(data32, 52) & 0x1) << 12) |
                       ((uint16_t) get_nybble(data32, 53)         << 8)  |
                       ((uint16_t) get_nybble(data32, 54)         << 4)  |
                        (uint16_t) get_nybble(data32, 55);

    // Nybble 44 bit 2 signals a cell failure.
    info.cell_failure = (get_nybble(data32, 44) & 0x04) != 0;

    // Primary checksums — these determine lock state.
    info.checksums_ok[0] = (calc_checksum(data32, 0,  15) == get_nybble(data32, 41));
    info.checksums_ok[1] = (calc_checksum(data32, 16, 31) == get_nybble(data32, 42));
    info.checksums_ok[2] = (calc_checksum(data32, 32, 40) == get_nybble(data32, 43));

    // Auxiliary checksums — informational only, do not affect lock state.
    info.aux_checksums_ok[0] = (calc_checksum(data32, 44, 47) == get_nybble(data32, 62));
    info.aux_checksums_ok[1] = (calc_checksum(data32, 48, 61) == get_nybble(data32, 63));

    bool all_ff = (get_nybble(data32, 40) == 0xf && get_nybble(data32, 41) == 0xf &&
                   get_nybble(data32, 42) == 0xf && get_nybble(data32, 43) == 0xf);

    info.locked = all_ff || !info.checksums_ok[0] ||
                             !info.checksums_ok[1] ||
                             !info.checksums_ok[2];
}

// ─────────────────────────────────────────────
//  Health calculations
// ─────────────────────────────────────────────

// Types 5 and 6: entirely from basic info fields.
float calc_health_type56(const BatteryInfo &info) {
    int f_ol = max((int)info.overload - 29, 0);
    int f_od = max(35 - (int)info.overdischarge, 0);
    float dmg = info.cycle_count + info.cycle_count * (f_ol + f_od) / 32.0f;
    static const uint8_t special[] = {26, 28, 40, 50};
    int scale = 600;
    for (int i = 0; i < 4; i++) {
        if (info.capacity_raw == special[i]) { scale = 1000; break; }
    }
    return max(0.0f, 4.0f - dmg / scale);
}

// Types 0, 2, 3: from health command response.
float calc_health_type023(uint16_t health_raw, uint8_t capacity_raw) {
    if (capacity_raw == 0) return 0;
    float ratio = (float)health_raw / capacity_raw;
    if (ratio > 80) return 4.0f;
    return max(0.0f, ratio / 10.0f - 5.0f);
}

// Old battery types (not 0/2/3/5/6): damage rating from nybble 46 bits 1..3.
// Rating < 3 => 4/4, rating 7 => 0/4, linear in between.
float calc_health_damage_rating(const uint8_t *data32) {
    uint8_t dmg = (get_nybble(data32, 46) >> 1) & 0x07;
    if (dmg < 3) return 4.0f;
    if (dmg >= 7) return 0.0f;
    return max(0.0f, 4.0f - (float)(dmg - 2));
}

// ─────────────────────────────────────────────
//  Health command (types 0, 2, 3)
// ─────────────────────────────────────────────
uint16_t read_health(int batt_type) {
    uint8_t cmd[4];
    uint8_t rsp[3] = {0};
    switch (batt_type) {
        case 0: { uint8_t c[] = {0xD4, 0x50, 0x01, 0x02}; memcpy(cmd, c, 4); break; }
        case 2: { uint8_t c[] = {0xD6, 0x04, 0x05, 0x02}; memcpy(cmd, c, 4); break; }
        case 3: { uint8_t c[] = {0xD6, 0x38, 0x02, 0x02}; memcpy(cmd, c, 4); break; }
        default: return 0;
    }
    cmd_cc(cmd, 4, rsp, 3);
    if (rsp[2] != 0x06) return 0;
    return le16(rsp);
}

// ─────────────────────────────────────────────
//  Overdischarge event count (types 0, 2, 3)
// ─────────────────────────────────────────────
uint8_t read_overdischarge_count(int batt_type) {
    uint8_t cmd[4];
    uint8_t rsp[2] = {0};
    switch (batt_type) {
        case 0: { uint8_t c[] = {0xD4, 0xBA, 0x00, 0x01}; memcpy(cmd, c, 4); break; }
        case 2: { uint8_t c[] = {0xD6, 0x8D, 0x05, 0x01}; memcpy(cmd, c, 4); break; }
        case 3: { uint8_t c[] = {0xD6, 0x09, 0x03, 0x01}; memcpy(cmd, c, 4); break; }
        default: return 0;
    }
    cmd_cc(cmd, 4, rsp, 2);
    if (rsp[1] != 0x06) return 0;
    return rsp[0];
}

// ─────────────────────────────────────────────
//  Overload counters (types 0, 2, 3)
//
//  Returns sum of all counters as BTC04 does.
//
//  Type 0: cc d4 8d 00 07 — 8 bytes, bit-packed 10-bit counters A/B/C:
//    A = byte0[7:6] (low 2) | byte1[7:0] (high 8)
//    B = byte3[7:0] (low 8) | byte4[1:0] (high 2)
//    C = byte5[7:4] (low 4) | byte6[5:0] (high 6)
//
//  Type 2: cc d6 5f 05 07 — 8 bytes, plain byte counters A(0) B(2) C(3) D(5) E(6)
//
//  Type 3: cc d6 5b 03 04 — 6 bytes, plain byte counters A(0) B(2) C(3)
// ─────────────────────────────────────────────
uint16_t read_overload_count(int batt_type) {
    switch (batt_type) {

        case 0: {
            uint8_t cmd[] = {0xD4, 0x8D, 0x00, 0x07};
            uint8_t rsp[8] = {0};
            cmd_cc(cmd, sizeof(cmd), rsp, 8);
            if (rsp[7] != 0x06) return 0;
            uint16_t a = ((uint16_t)(rsp[0] >> 6) & 0x03) | ((uint16_t)rsp[1] << 2);
            uint16_t b = (uint16_t)rsp[3]          | (((uint16_t)rsp[4] & 0x03) << 8);
            uint16_t c = ((uint16_t)(rsp[5] >> 4)) | (((uint16_t)rsp[6] & 0x3F) << 4);
            return a + b + c;
        }

        case 2: {
            uint8_t cmd[] = {0xD6, 0x5F, 0x05, 0x07};
            uint8_t rsp[8] = {0};
            cmd_cc(cmd, sizeof(cmd), rsp, 8);
            if (rsp[7] != 0x06) return 0;
            return (uint16_t)rsp[0] + rsp[2] + rsp[3] + rsp[5] + rsp[6];
        }

        case 3: {
            uint8_t cmd[] = {0xD6, 0x5B, 0x03, 0x04};
            uint8_t rsp[6] = {0};
            cmd_cc(cmd, sizeof(cmd), rsp, 6);
            if (rsp[5] != 0x06) return 0;
            return (uint16_t)rsp[0] + rsp[2] + rsp[3];
        }

        default: return 0;
    }
}

// ─────────────────────────────────────────────
//  Charge level / coulomb counter (types 0, 2, 3)
//  Same command for all three types.
// ─────────────────────────────────────────────
uint32_t read_charge_level() {
    uint8_t cmd[] = { 0xD7, 0x19, 0x00, 0x04 };
    uint8_t rsp[5] = {0};
    cmd_cc(cmd, sizeof(cmd), rsp, 5);
    if (rsp[4] != 0x06) return 0;
    return (uint32_t)rsp[0] | ((uint32_t)rsp[1] << 8) |
           ((uint32_t)rsp[2] << 16) | ((uint32_t)rsp[3] << 24);
}

// ─────────────────────────────────────────────
//  Temperature
//
//  Types 0/2/3: cc d7 0e 00 02 → 16-bit LE in 1/10 K → convert to C
//  Type 5:      cc 52          → 16-bit LE in 1/10 K → convert to C
//  Type 6:      d2             → single byte, t = (-40x + 9323) / 100 C
// ─────────────────────────────────────────────
float read_temperature(int batt_type) {
    switch (batt_type) {

        case 0: case 2: case 3: {
            uint8_t cmd[] = {0xD7, 0x0E, 0x00, 0x02};
            uint8_t rsp[3] = {0};
            cmd_cc(cmd, sizeof(cmd), rsp, 3);
            if (rsp[2] != 0x06) return INVALID_TEMP;
            return le16(rsp) / 10.0f - 273.15f;
        }

        case 5: {
            uint8_t cmd[] = {0x52};
            uint8_t rsp[2] = {0};
            cmd_cc(cmd, sizeof(cmd), rsp, 2);
            return le16(rsp) / 10.0f - 273.15f;
        }

        case 6: {
            uint8_t cmd[] = {0xD2};
            uint8_t rsp[1] = {0};
            cmd_cc(cmd, sizeof(cmd), rsp, 1);
            return (-40.0f * rsp[0] + 9323.0f) / 100.0f;
        }

        default: return INVALID_TEMP;
    }
}

// ─────────────────────────────────────────────
//  Voltages
//
//  Types 0/2/3: cc d7 00 00 0c → pack + up to 5 cells as 16-bit LE mV
//
//  Type 5: per-cell commands 0x31..0x35; no pack-voltage command exists,
//          so pack is derived as the sum of cell voltages.
//
//  Type 6: two-step — cc 10 21 enters voltage-read mode (no response),
//          then d4 returns 20 bytes (10 x 16-bit LE raw values).
//          Conversion: v_mV = 6000 - raw/10
//          Pack voltage derived as sum of all 10 cells.
// ─────────────────────────────────────────────
void read_voltages_type023(float *v_pack, float *cells, int cell_count) {
    uint8_t cmd[] = { 0xD7, 0x00, 0x00, 0x0C };
    uint8_t rsp[13] = {0};
    cmd_cc(cmd, sizeof(cmd), rsp, 13);
    if (rsp[12] != 0x06) return;
    *v_pack = le16(&rsp[0]) / 1000.0f;
    for (int i = 0; i < min(cell_count, 5); i++) {
        cells[i] = le16(&rsp[2 + i * 2]) / 1000.0f;
    }
}

void read_voltages_type5(float *v_pack, float *cells, int cell_count) {
    float pack = 0;
    for (int i = 0; i < min(cell_count, 5); i++) {
        uint8_t cmd[1] = { (uint8_t)(0x31 + i) };
        uint8_t rsp[2] = {0};
        cmd_cc(cmd, 1, rsp, 2);
        cells[i] = le16(rsp) / 1000.0f;
        pack += cells[i];
    }
    *v_pack = pack;
}

void read_voltages_type6(float *v_pack, float *cells, int cell_count) {
    uint8_t dummy[1] = {0};
    uint8_t rsp[20]  = {0};

    // Hold ENABLE HIGH across both steps — the battery must not see a bus drop
    // between the mode-enter command and the read command.
    enable_bus();
    {
        uint8_t cmd[] = { 0x10, 0x21 };
        cmd_cc_raw(cmd, sizeof(cmd), dummy, 0);
        delay(10);
    }
    {
        uint8_t cmd[] = { 0xD4 };
        cmd_cc_raw(cmd, sizeof(cmd), rsp, 20);
    }
    disable_bus();

    float pack = 0;
    for (int i = 0; i < min(cell_count, 10); i++) {
        uint16_t raw = le16(&rsp[i * 2]);
        cells[i] = (6000.0f - raw / 10.0f) / 1000.0f;
        pack += cells[i];
    }
    *v_pack = pack;
}

// ─────────────────────────────────────────────
//  Print diagnostic report
// ─────────────────────────────────────────────
void print_report(const BatteryInfo &info, float v_pack, float *cells, const uint8_t *data32) {

    Serial.print(F("Model          : ")); Serial.println(info.model);

    Serial.print(F("ROM ID         : "));
    for (int i = 0; i < 8; i++) {
        if (info.rom_id[i] < 0x10) Serial.print(F("0"));
        Serial.print(info.rom_id[i], HEX);
        if (i < 7) Serial.print(F(" "));
    }
    Serial.println();

    // ROM ID bytes 0-2 encode manufacture date: year (20xx), month, day — raw decimal.
    char date_buf[13];
    snprintf(date_buf, sizeof(date_buf), "%02d/%02d/20%02d",
             info.rom_id[2], info.rom_id[1], info.rom_id[0]);
    Serial.print(F("Mfg date       : ")); Serial.println(date_buf);

    Serial.print(F("Detected type  : ")); Serial.println(info.type);

    Serial.print(F("Battery type   : "));
    Serial.print(info.batt_type_raw);
    Serial.print(F("  ("));
    if      (info.batt_type_raw < 13) Serial.print(F("4 cell BL14xx"));
    else if (info.batt_type_raw < 30) Serial.print(F("5 cell BL18xx"));
    else                              Serial.print(F("10 cell BL36xx"));
    Serial.println(F(")"));

    Serial.print(F("Capacity       : ")); Serial.print(info.capacity_ah, 1); Serial.println(F(" Ah"));

    print_sep();
    Serial.print(F("Lock status    : ")); Serial.println(info.locked ? "LOCKED" : "UNLOCKED");
    Serial.print(F("Cell failure   : ")); Serial.println(info.cell_failure ? "YES" : "No");

    Serial.print(F("Failure code   : "));
    switch (info.failure_code) {
        case 0:  Serial.println(F("0 - OK")); break;
        case 1:  Serial.println(F("1 - Overloaded")); break;
        case 5:  Serial.println(F("5 - Warning")); break;
        case 15: Serial.println(F("15 - Critical (BMS dead)")); break;
        default: Serial.println(info.failure_code); break;
    }

    Serial.print(F("Checksum 0-15  : ")); Serial.println(info.checksums_ok[0] ? "OK" : "FAIL");
    Serial.print(F("Checksum 16-31 : ")); Serial.println(info.checksums_ok[1] ? "OK" : "FAIL");
    Serial.print(F("Checksum 32-40 : ")); Serial.println(info.checksums_ok[2] ? "OK" : "FAIL");
    Serial.print(F("Aux CSum 44-47 : ")); Serial.println(info.aux_checksums_ok[0] ? "OK" : "FAIL");
    Serial.print(F("Aux CSum 48-61 : ")); Serial.println(info.aux_checksums_ok[1] ? "OK" : "FAIL");

    print_sep();
    Serial.print(F("Cycle count    : ")); Serial.println(info.cycle_count);

    Serial.print(F("Health         : "));
    Serial.print(info.health_rating, 2);
    Serial.print(F(" / 4  ("));
    int h = (int)round(info.health_rating);
    for (int i = 0; i < 4; i++) Serial.print(i < h ? "#" : "-");
    Serial.println(F(")"));

    // Types 0/2/3: event counters and derived percentages.
    if (info.type == 0 || info.type == 2 || info.type == 3) {
        Serial.print(F("OD events      : ")); Serial.println(info.od_count);
        Serial.print(F("Overload cnt   : ")); Serial.println(info.overload_count);

        if (info.od_count > 0 && info.cycle_count > 0) {
            float p = 4.0f + 100.0f * info.od_count / info.cycle_count;
            Serial.print(F("OD %           : ")); Serial.print(p, 1); Serial.println(F(" %"));
        }
        if (info.overload_count > 0 && info.cycle_count > 0) {
            float p = 4.0f + 100.0f * info.overload_count / info.cycle_count;
            Serial.print(F("Overload %     : ")); Serial.print(p, 1); Serial.println(F(" %"));
        }
    }

    // Types 5/6: percentages calculated from raw basic-info fields.
    if (info.type == 5 || info.type == 6) {
        float od_pct = -5.0f * info.overdischarge + 160.0f;
        float ol_pct =  5.0f * info.overload      - 160.0f;
        Serial.print(F("OD %           : ")); Serial.print(od_pct, 1); Serial.println(F(" %"));
        Serial.print(F("Overload %     : ")); Serial.print(ol_pct, 1); Serial.println(F(" %"));
    }

    if (info.temperature_c != INVALID_TEMP) {
        Serial.print(F("Temperature    : ")); Serial.print(info.temperature_c, 1); Serial.println(F(" C"));
    }

    print_sep();
    print_float("Pack voltage   : ", v_pack, " V");

    float v_max = cells[0], v_min = cells[0];
    for (int i = 0; i < info.cell_count; i++) {
        char label[22];
        snprintf(label, sizeof(label), "Cell %2d        : ", i + 1);
        print_float(label, cells[i], " V");
        if (cells[i] > v_max) v_max = cells[i];
        if (cells[i] < v_min) v_min = cells[i];
    }
    print_float("Cell diff      : ", v_max - v_min, " V");

    // State of charge (types 0/2/3).
    if ((info.type == 0 || info.type == 2 || info.type == 3) &&
        info.charge_level > 0 && info.capacity_raw > 0) {
        float ratio = (float)info.charge_level / info.capacity_raw / 2880.0f;
        int soc = (ratio < 10.0f) ? 1 : min((int)(ratio / 10.0f), 7);
        print_sep();
        Serial.print(F("State of charge: ")); Serial.print(soc); Serial.println(F(" / 7"));
    }

    print_sep();
    Serial.print(F("Frame          : "));
    for (int i = 0; i < 32; i++) {
        if (data32[i] < 0x10) Serial.print(F("0"));
        Serial.print(data32[i], HEX);
        if (i < 31) Serial.print(F(" "));
    }
    Serial.println();
}

// ─────────────────────────────────────────────
//  Write corrected frame
//
//  Called when primary checksums are corrupt and DA 04 has failed to
//  self-repair them. Takes the live data32 frame, recalculates the three
//  primary checksum nybbles (41, 42, 43) and the two auxiliary checksum
//  nybbles (62, 63), then writes the corrected frame back using the
//  charger write sequence observed in the BTC04 / DC18RC:
//
//    1. enter test mode        (cc d9 96 a5)
//    2. charger-mode arm       (cc f0 00)   — arms BMS for write
//    3. write frame            (33 [ROM ID] 33 [32 bytes])
//    4. store / commit         (33 [ROM ID] 55 a5)
//    5. exit test mode         (cc d9 ff ff)
//    6. power-cycle bus        — lets BMS commit and re-initialise
//
//  The frame written is the battery's own live data with only the
//  checksum nybbles corrected — no other bytes are touched.
//
//  Returns true if a subsequent read-back shows all checksums passing.
// ─────────────────────────────────────────────
bool write_corrected_frame(uint8_t *data32) {
    // --- Step 1: recalculate all five checksum nybbles in a working copy ---
    uint8_t frame[32];
    memcpy(frame, data32, 32);

    set_nybble(frame, 41, calc_checksum(frame, 0,  15));
    set_nybble(frame, 42, calc_checksum(frame, 16, 31));
    set_nybble(frame, 43, calc_checksum(frame, 32, 40));
    // Auxiliary checksums — not used for lock state but write them correctly
    // so the frame is fully consistent.
    set_nybble(frame, 62, calc_checksum(frame, 44, 47));
    set_nybble(frame, 63, calc_checksum(frame, 48, 61));

    Serial.println(F("  Writing corrected frame..."));
    Serial.print(F("  Frame: "));
    for (int i = 0; i < 32; i++) {
        if (frame[i] < 0x10) Serial.print(F("0"));
        Serial.print(frame[i], HEX);
        if (i < 31) Serial.print(F(" "));
    }
    Serial.println();

    // --- Step 2: enter test mode ---
    enter_testmode();

    // --- Step 3: charger-mode arm (cc f0 00) ---
    // This is the form used by the BTC04/DC18RC before a write.
    // Per the spec, repeating f0 00 without cycling ENABLE will lock
    // the bus — so we issue it exactly once here.
    {
        uint8_t rsp[32] = {0};
        cmd_cc(CHARGER_CMD, sizeof(CHARGER_CMD), rsp, 32);
        delay(10);
    }

    // --- Step 4: write frame (33 [ROM ID read] 33 [32 bytes]) ---
    // cmd_33 reads the 8-byte ROM ID then sends the write opcode (0x33)
    // followed by the 32-byte payload. No response bytes are expected
    // from a write transaction.
    {
        uint8_t write_opcode_and_payload[1 + 32];
        write_opcode_and_payload[0] = 0x33;           // write opcode
        memcpy(&write_opcode_and_payload[1], frame, 32);

        uint8_t rsp[8] = {0};   // only ROM ID bytes come back
        cmd_33(write_opcode_and_payload, sizeof(write_opcode_and_payload), rsp, 0);
        delay(20);
    }

    // --- Step 5: store / commit (33 [ROM ID read] 55 a5) ---
    {
        uint8_t rsp[8] = {0};
        cmd_33(STORE_CMD, sizeof(STORE_CMD), rsp, 0);
        delay(50);
    }

    // --- Step 6: exit test mode ---
    exit_testmode();

    // --- Step 7: power-cycle so BMS reinitialises from committed values ---
    power_cycle_bus(150, 200);

    // --- Step 8: verify ---
    uint8_t verify[32] = {0};
    if (read_basic_info(verify) != 1) {
        Serial.println(F("  Write verify: no response after power cycle."));
        return false;
    }

    BatteryInfo check;
    memset(&check, 0, sizeof(check));
    parse_basic_info(verify, check);

    Serial.print(F("  Write verify checksum 0-15  : ")); Serial.println(check.checksums_ok[0] ? "OK" : "FAIL");
    Serial.print(F("  Write verify checksum 16-31 : ")); Serial.println(check.checksums_ok[1] ? "OK" : "FAIL");
    Serial.print(F("  Write verify checksum 32-40 : ")); Serial.println(check.checksums_ok[2] ? "OK" : "FAIL");

    if (!check.locked) {
        memcpy(data32, verify, 32);   // hand caller the fresh verified frame
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────
//  Charger-style unlock
//  Only valid for battery types 0, 2, and 3.
//  Types 5, 6, and unknown do not have documented
//  unlock commands — do not call for those types.
//
//  Sequence:
//    1. DA 04 reset — ask BMS to self-correct errors
//    2. Re-read basic info and check lock state
//    3a. If unlocked and checksums OK  → success
//    3b. If checksums corrupt          → try write_corrected_frame()
//    3c. If still locked               → cannot recover
// ─────────────────────────────────────────────
bool attempt_unlock(BatteryInfo &info, uint8_t *data32) {
    power_cycle_bus(150, 200);
    enter_testmode();
    reset_errors();
    exit_testmode();
    delay(50);

    if (read_basic_info(data32) != 1) {
        Serial.println(F("  No response after unlock attempt."));
        return false;
    }

    parse_basic_info(data32, info);
    Serial.print(F("  Checksum 0-15  : ")); Serial.println(info.checksums_ok[0] ? "OK" : "FAIL");
    Serial.print(F("  Checksum 16-31 : ")); Serial.println(info.checksums_ok[1] ? "OK" : "FAIL");
    Serial.print(F("  Checksum 32-40 : ")); Serial.println(info.checksums_ok[2] ? "OK" : "FAIL");

    if (!info.locked) {
        Serial.println(F("  -> UNLOCKED."));
        return true;
    }

    // DA 04 did not clear the lock. If checksums are corrupt the BMS cannot
    // self-repair them — attempt a single frame write.
    bool any_checksum_bad = !info.checksums_ok[0] ||
                            !info.checksums_ok[1] ||
                            !info.checksums_ok[2];
    if (any_checksum_bad) {
        Serial.println(F("  -> Checksums corrupt. DA 04 cannot fix this."));
        Serial.println(F("  -> Attempting frame write..."));
        led_purple();
        flash_leds(2);
        if (write_corrected_frame(data32)) {
            parse_basic_info(data32, info);
            Serial.println(F("  -> Frame write succeeded. UNLOCKED."));
            return true;
        }
        Serial.println(F("  -> Frame write failed."));
        return false;
    }

    Serial.println(F("  -> Still locked (non-checksum reason). Cannot recover."));
    return false;
}

// Returns true if this battery type supports the charger-style unlock sequence.
// Only types 0, 2, and 3 have documented test-mode and reset-error commands.
bool type_supports_unlock(int batt_type) {
    return (batt_type == 0 || batt_type == 2 || batt_type == 3);
}

// ─────────────────────────────────────────────
//  Main scan routine
//  Returns true on a complete scan, false if the battery
//  did not respond or was pulled mid-scan.
// ─────────────────────────────────────────────
bool run_scan() {
    // Defensive: exit any lingering test mode from a previous interrupted scan.
    exit_testmode();
    delay(50);

    led_off();
    Serial.println(F("============================================"));
    Serial.println(F("  Makita Monitor - Scanning..."));
    Serial.println(F("============================================"));

    BatteryInfo info;
    memset(&info, 0, sizeof(info));
    info.temperature_c = INVALID_TEMP;

    uint8_t data32[32] = {0};

    // Bounded retry (~4.5 s): cc aa 00 is universally supported (all types incl. F0513).
    int basic_tries = 0;
    int basic_result;
    while ((basic_result = read_basic_info(data32)) == 0) {
        if (++basic_tries > 15) {
            Serial.println(F("ERROR: Battery not responding. Aborting scan."));
            exit_testmode();
            led_off();
            disable_bus();
            return false;
        }
        delay(200);
    }

    if (basic_result == -1) {
        // Pre-type-0 HC08 (Freescale MC908JK3E BMS): all-0xFF response.
        // NO cell monitoring or protection — must NOT be charged on any charger.
        print_sep();
        Serial.println(F("WARNING: Pre-type-0 HC08 battery detected!"));
        Serial.println(F("         Freescale MC908JK3E BMS -- no cell protection."));
        Serial.println(F("         DO NOT charge on any charger."));
        print_sep();
        led_red();
        disable_bus();
        return true;
    }

    parse_basic_info(data32, info);
    read_rom_id(info.rom_id);

    // Detect type before reading model — type 5 uses a different model command.
    info.type = detect_battery_type(info.rom_id, data32);

    if (info.type == 5) {
        read_model_type5(info.model);
    } else {
        read_model(info.model);
    }

    print_sep();
    Serial.print(F("Battery found  : ")); Serial.println(info.model);
    led_green();
    // LED flash: only types 0/2/3 have documented LED commands.
    if (info.type == 0 || info.type == 2 || info.type == 3) {
        flash_leds(1);
    }

    // ── Voltages — completely type-specific ───────────────
    // Array sized for 10 cells (type 6); unused entries stay 0.
    float v_pack = 0;
    float cells[10] = {0};

    switch (info.type) {
        case 5: read_voltages_type5(&v_pack, cells, info.cell_count); break;
        case 6: read_voltages_type6(&v_pack, cells, info.cell_count); break;
        default: read_voltages_type023(&v_pack, cells, info.cell_count); break;
    }

    // ── Temperature ───────────────────────────────────────
    info.temperature_c = read_temperature(info.type);

    // ── Health, counters, charge level ───────────────────
    if (info.type == 5 || info.type == 6) {
        info.health_rating = calc_health_type56(info);
        // No counter commands for type 5/6; OD/OL% come from basic info.

    } else if (info.type == 0 || info.type == 2 || info.type == 3) {
        uint16_t health_raw = read_health(info.type);
        info.health_rating  = calc_health_type023(health_raw, info.capacity_raw);
        info.od_count       = read_overdischarge_count(info.type);
        info.overload_count = read_overload_count(info.type);
        info.charge_level   = read_charge_level();

    } else {
        // Old/unknown battery type — use damage-rating field from basic info.
        info.health_rating = calc_health_damage_rating(data32);
    }

    // If the battery was removed during secondary reads, all those responses
    // will be zero — don't print a report full of garbage.
    if (!battery_present()) {
        Serial.println(F("Battery removed during scan. Aborting."));
        led_off();
        disable_bus();
        return false;
    }

    print_sep();
    print_report(info, v_pack, cells, data32);
    print_sep();

    if (!info.locked) {
        Serial.println(F("Battery UNLOCKED."));
        led_blue();
        if (info.type == 0 || info.type == 2 || info.type == 3) {
            flash_leds(3);
        }
        led_blue();
    } else {
        Serial.print(F("Battery LOCKED. Failure code: "));
        Serial.println(info.failure_code);

        if (info.failure_code == 15) {
            // BMS is considered dead — nothing can recover this.
            Serial.println(F("CRITICAL: BMS considered dead. Cannot recover."));
            led_red();

        } else if (!type_supports_unlock(info.type)) {
            // Types 5, 6, and unknown have no documented unlock commands.
            // Sending type 0/2/3 test-mode commands to these batteries could
            // corrupt state or cause undefined behaviour — do not attempt.
            Serial.print(F("Unlock not supported for battery type "));
            Serial.print(info.type);
            Serial.println(F(". No unlock attempted."));
            led_red();

        } else {
            // Types 0, 2, 3: charger-style unlock is documented and safe.
            Serial.println(F("Attempting charger-style auto-unlock..."));
            led_yellow();
            flash_leds(2);

            bool unlocked = attempt_unlock(info, data32);

            if (unlocked) {
                Serial.println(F("Battery successfully unlocked."));
                led_blue();
                flash_leds(3);
                led_blue();
            } else {
                Serial.println(F("Could not unlock battery."));
                led_red();
            }
        }
    }

    print_sep();
    Serial.println(F("Complete."));
    disable_bus();
    return true;
}

// ─────────────────────────────────────────────
//  Hot-swap state machine
//
//  WAIT_BATTERY  — polling for a battery to be inserted
//  SCAN_NOW      — battery just detected (or manual rescan); run_scan() pending
//  IDLE          — scan complete; polling for removal
// ─────────────────────────────────────────────
enum ScanState { WAIT_BATTERY, SCAN_NOW, IDLE };
static ScanState     g_state      = WAIT_BATTERY;
static unsigned long g_lastPollMs = 0;
static unsigned long g_pulseMs    = 0;
static const unsigned long POLL_MS = 800UL;  // ms between presence polls

// ─────────────────────────────────────────────
//  Arduino entry points
// ─────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    pinMode(ENABLE_PIN, OUTPUT);
    digitalWrite(ENABLE_PIN, LOW);

    pixel.begin();
    pixel.setBrightness(80);
    led_off();

    delay(500);

    Serial.println(F("============================================"));
    Serial.println(F("  Makita Monitor"));
    Serial.println(F("  's' = rescan | auto-detects connect/disconnect"));
    Serial.println(F("============================================"));
    Serial.println(F("Waiting for battery..."));

    g_state      = WAIT_BATTERY;
    g_lastPollMs = 0;
}

void loop() {
    // ── Serial command ──────────────────────────────────────
    if (Serial.available()) {
        char c = Serial.read();
        if (c == 's' || c == 'S') {
            if (g_state != SCAN_NOW) {
                Serial.println(F("Manual rescan requested."));
                g_state = SCAN_NOW;
            }
        }
    }

    // ── Periodic battery-presence poll ──────────────────────
    // Only runs when not actively scanning (scan blocks this thread).
    unsigned long now = millis();
    if (now - g_lastPollMs >= POLL_MS) {
        g_lastPollMs = now;
        bool present = battery_present();

        if (g_state == WAIT_BATTERY && present) {
            Serial.println(F("Battery detected — starting scan."));
            g_state = SCAN_NOW;
        } else if (g_state == IDLE && !present) {
            Serial.println();
            Serial.println(F("============================================"));
            Serial.println(F("  Battery removed. Waiting..."));
            Serial.println(F("============================================"));
            led_off();
            g_state = WAIT_BATTERY;
        }
    }

    // ── Idle pulse (white) while waiting for battery ─────────
    if (g_state == WAIT_BATTERY && now - g_pulseMs >= 20UL) {
        g_pulseMs = now;
        led_pulse_white();
    }

    // ── Execute scan when triggered ─────────────────────────
    if (g_state == SCAN_NOW) {
        // Mark IDLE optimistically; reset to WAIT_BATTERY below if scan fails.
        g_state = IDLE;
        bool ok = run_scan();
        if (!ok) {
            // Comms error or battery pulled mid-scan — return to detection loop.
            led_off();
            g_state = WAIT_BATTERY;
            Serial.println(F("Waiting for battery..."));
        }
    }
}
