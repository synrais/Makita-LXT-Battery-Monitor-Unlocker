// ============================================================
//  Makita Battery Monitor + Lock Utility — Dual-Mode Firmware
//
//  Scan mode    : full battery scan + auto-unlock (default)
//  Lock mode    : auto hard-lock sentinel for types 0 / 2 / 3
//
//  Mode select  : bridge GPIO 4 → GPIO 5
//                 Bridged  → LOCK mode  (orange pulse)
//                 Open     → SCAN mode  (white pulse)
//
//  Target: Waveshare RP2040 Zero + other boards
//  Build:  PlatformIO + Adafruit NeoPixel + Modified OneWire2
// ============================================================

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "OneWire2.h"

// ─── Pin definitions ─────────────────────────────────────────
static const uint8_t PIN_ONEWIRE      = 6;
static const uint8_t PIN_ENABLE       = 8;
static const uint8_t NEOPIXEL_OUT_PIN = 16;
static const uint8_t NUM_PIXELS       = 1;
static const uint8_t PIN_MODE_OUT     = 4;
static const uint8_t PIN_MODE_IN      = 5;

static const uint8_t LOCK_VAL = 0xF;

// ─── Protocol constants ───────────────────────────────────────
static const uint16_t BUS_BYTE_GAP_US   = 100;
static const uint16_t BUS_SETTLE_US     = 150;
static const uint16_t BUS_ENABLE_MS     = 100;
static const uint16_t BUS_PRESENCE_MS   = 50;
static const uint16_t POWERCYCLE_OFF_MS = 150;
static const uint16_t POWERCYCLE_ON_MS  = 200;

static const uint8_t CMD_BASIC_INFO[]      = { 0xAA, 0x00 };
static const uint8_t BASIC_INFO_LEN        = 32;
static const uint8_t TYPE_PROBE_MAGIC      = 0x06;

static const uint8_t CMD_MODEL[]           = { 0xDC, 0x0C };
static const uint8_t CMD_TESTMODE_ENTER[]  = { 0xD9, 0x96, 0xA5 };
static const uint8_t CMD_TESTMODE_EXIT[]   = { 0xD9, 0xFF, 0xFF };
static const uint8_t CMD_LEDS_ON[]         = { 0xDA, 0x31 };
static const uint8_t CMD_LEDS_OFF[]        = { 0xDA, 0x34 };
static const uint8_t LED_CMD_RSP_LEN       = 9;
static const uint8_t CMD_RESET_ERRORS[]    = { 0xDA, 0x04 };
// WARNING: issuing CMD_CHARGER_ARM twice without cycling ENABLE locks the bus.
// g_charger_arm_issued guards against re-issue within one session.
static const uint8_t CMD_CHARGER_ARM[]     = { 0xF0, 0x00 };
static const uint8_t FRAME_WRITE_OPCODE    = 0x0F;
static const uint8_t FRAME_WRITE_PAD       = 0x00;
static const uint8_t CMD_STORE[]           = { 0x55, 0xA5 };

static const float TEMP_INVALID   = -999.0f;
static const float KELVIN_OFFSET  = 273.15f;
static const float TEMP6_A        = 9323.0f;
static const float TEMP6_B        = -40.0f;

static const uint8_t  HEALTH_SCALE_EXTENDED_CODES[] = { 26, 28, 40, 50 };
static const uint16_t HEALTH_SCALE_STANDARD = 600;
static const uint16_t HEALTH_SCALE_EXTENDED = 1000;
static const float    HEALTH_MAX            = 4.0f;
static const float    SOC_DIVISOR           = 2880.0f;

// ─── Scan timing ─────────────────────────────────────────────
static const uint16_t SCAN_RETRY_DELAY_MS   = 200;
static const uint8_t  SCAN_MAX_RETRIES      = 15;
static const uint16_t POLL_INTERVAL_MS      = 800;
static const uint8_t  LED_PULSE_INTERVAL_MS = 20;

// ─── Return codes / types ─────────────────────────────────────
enum BasicInfoResult { BASIC_INFO_NO_RESPONSE = 0, BASIC_INFO_OK = 1, BASIC_INFO_PRE_TYPE0 = -1 };
enum BusResult       { BUS_OK = 0, BUS_NO_PRESENCE = 1 };
enum BatteryType     { BATT_TYPE_UNKNOWN = -1, BATT_TYPE_0 = 0, BATT_TYPE_2 = 2,
                       BATT_TYPE_3 = 3, BATT_TYPE_5 = 5, BATT_TYPE_6 = 6 };

// ─── Data structures ─────────────────────────────────────────
struct RawBasicInfo {
    uint8_t  batt_type, capacity, flags, failure_code, overdischarge, overload;
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

struct BatteryInfo {
    char          model[8];
    uint8_t       rom_id[8];
    BatteryType   type;
    RawBasicInfo  raw;
    HealthData    health;
    float         temperature_c, temperature_mosfet_c;
    uint8_t       cell_count;
    float         capacity_ah;
    bool          locked;
    bool          checksums_ok[3];
    bool          aux_checksums_ok[2];
};

// ─── Globals ──────────────────────────────────────────────────
static OneWire           g_ow(PIN_ONEWIRE);
static Adafruit_NeoPixel g_pixel(NUM_PIXELS, NEOPIXEL_OUT_PIN, NEO_GRB + NEO_KHZ800);
static bool              g_charger_arm_issued = false;
static bool              g_in_testmode        = false;

// ─── NeoPixel ────────────────────────────────────────────────
// Colour table: { R, G, B } — indexed by a small enum for clarity.
struct Colour { uint8_t r, g, b; };
static const Colour COL_OFF    = {  0,  0,  0 };
static const Colour COL_GREEN  = {  0, 80,  0 };
static const Colour COL_YELLOW = { 80, 60,  0 };
static const Colour COL_BLUE   = {  0,  0, 80 };
static const Colour COL_RED    = { 80,  0,  0 };
static const Colour COL_PURPLE = { 80,  0, 80 };
static const Colour COL_ORANGE = { 80, 30,  0 };

static void led_set(Colour c) {
    g_pixel.setPixelColor(0, g_pixel.Color(c.r, c.g, c.b));
    g_pixel.show();
}
static void led_off()    { led_set(COL_OFF);    }
static void led_green()  { led_set(COL_GREEN);  }
static void led_yellow() { led_set(COL_YELLOW); }
static void led_blue()   { led_set(COL_BLUE);   }
static void led_red()    { led_set(COL_RED);    }
static void led_purple() { led_set(COL_PURPLE); }
static void led_orange() { led_set(COL_ORANGE); }

// Triangle-wave pulse: 0→80→0 over 2 s.
static void led_pulse(Colour c) {
    uint32_t t = millis() % 2000UL;
    uint8_t  v = (t < 1000) ? (uint8_t)(t * 80 / 1000) : (uint8_t)((2000UL - t) * 80 / 1000);
    // Scale each channel proportionally to the target colour.
    Colour sc = { (uint8_t)(v * c.r / 80), (uint8_t)(v * c.g / 80), (uint8_t)(v * c.b / 80) };
    led_set(sc);
}

// ─── Mode detection ───────────────────────────────────────────
static bool lock_pin_read() { return digitalRead(PIN_MODE_IN) == HIGH; }

// ─── Bus control ─────────────────────────────────────────────
static void bus_enable(uint16_t ms = BUS_ENABLE_MS) { digitalWrite(PIN_ENABLE, HIGH); delay(ms); }
static void bus_disable()                            { digitalWrite(PIN_ENABLE, LOW);  }

static void power_cycle_bus(uint16_t off_ms = POWERCYCLE_OFF_MS, uint16_t on_ms = POWERCYCLE_ON_MS) {
    bus_disable();
    g_charger_arm_issued = false;
    delay(off_ms);
    bus_enable(on_ms);
}

static bool battery_present() {
    digitalWrite(PIN_ENABLE, HIGH);
    delay(BUS_PRESENCE_MS);
    bool p = (g_ow.reset() == 1);
    digitalWrite(PIN_ENABLE, LOW);
    return p;
}

// ─── Low-level 1-Wire transactions ───────────────────────────
// `managed`: if true, calls bus_enable/disable around the transaction.
// `rom_prefix`: 0xCC (skip ROM) or 0x33 (read ROM + store 8 bytes in rsp[0..7]).
// Caller must size rsp >= 8+rsp_len when rom_prefix==0x33.
static BusResult ow_transaction(bool managed, uint8_t rom_prefix,
                                 const uint8_t *cmd, uint8_t cmd_len,
                                 uint8_t *rsp,       uint8_t rsp_len) {
    if (managed) bus_enable();
    if (!g_ow.reset()) { if (managed) bus_disable(); return BUS_NO_PRESENCE; }
    g_ow.write(rom_prefix, 0);
    if (rom_prefix == 0x33) {
        for (uint8_t i = 0; i < 8; i++) { delayMicroseconds(BUS_BYTE_GAP_US); rsp[i] = g_ow.read(); }
        rsp += 8;   // payload bytes follow the ROM bytes
    }
    for (uint8_t i = 0; i < cmd_len; i++) { delayMicroseconds(BUS_BYTE_GAP_US); g_ow.write(cmd[i], 0); }
    delayMicroseconds(BUS_SETTLE_US);
    for (uint8_t i = 0; i < rsp_len; i++) { delayMicroseconds(BUS_BYTE_GAP_US); rsp[i] = g_ow.read(); }
    if (managed) bus_disable();
    return BUS_OK;
}

// Convenience wrappers preserving the original call-site API.
static BusResult cmd_cc    (const uint8_t *c, uint8_t cl, uint8_t *r, uint8_t rl) { return ow_transaction(true,  0xCC, c, cl, r, rl); }
static BusResult cmd_cc_raw(const uint8_t *c, uint8_t cl, uint8_t *r, uint8_t rl) { return ow_transaction(false, 0xCC, c, cl, r, rl); }
static BusResult cmd_33    (const uint8_t *c, uint8_t cl, uint8_t *r, uint8_t rl) { return ow_transaction(true,  0x33, c, cl, r, rl); }
static BusResult cmd_33_raw(const uint8_t *c, uint8_t cl, uint8_t *r, uint8_t rl) { return ow_transaction(false, 0x33, c, cl, r, rl); }

// ─── Nybble helpers (32-byte frame, LSN first) ───────────────
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
    uint16_t sum = 0;
    for (uint8_t i = s; i <= e; i++) sum += nybble_get(d, i);
    return (uint8_t)(min(sum, (uint16_t)0xFF) & 0x0F);
}
static inline uint16_t le16(const uint8_t *b) { return (uint16_t)b[0] | ((uint16_t)b[1] << 8); }

// ─── Utility ─────────────────────────────────────────────────
static void print_sep() { Serial.println(F("--------------------------------------------")); }

// ─── Test mode ───────────────────────────────────────────────
static BusResult enter_testmode() {
    uint8_t r[1] = {0};
    BusResult res = cmd_cc(CMD_TESTMODE_ENTER, sizeof(CMD_TESTMODE_ENTER), r, 1);
    if (res == BUS_OK) { g_in_testmode = true; delay(20); }
    return res;
}
static void exit_testmode() {
    if (!g_in_testmode) return;
    uint8_t r[1] = {0};
    cmd_cc(CMD_TESTMODE_EXIT, sizeof(CMD_TESTMODE_EXIT), r, 1);
    g_in_testmode = false;
    delay(20);
}

// ─── Battery LED flash ────────────────────────────────────────
static bool flash_battery_leds(uint8_t times, uint16_t on_ms = 150, uint16_t off_ms = 100) {
    uint8_t tm[8 + LED_CMD_RSP_LEN], on[8 + LED_CMD_RSP_LEN], of[8 + LED_CMD_RSP_LEN];
    for (uint8_t i = 0; i < times; i++) {
        bus_enable(BUS_PRESENCE_MS);
        if (cmd_33_raw(CMD_TESTMODE_ENTER, sizeof(CMD_TESTMODE_ENTER), tm, LED_CMD_RSP_LEN) != BUS_OK ||
            cmd_33_raw(CMD_LEDS_ON,        sizeof(CMD_LEDS_ON),        on, LED_CMD_RSP_LEN) != BUS_OK)
            { bus_disable(); return false; }
        delay(on_ms);
        if (cmd_33_raw(CMD_TESTMODE_ENTER, sizeof(CMD_TESTMODE_ENTER), tm, LED_CMD_RSP_LEN) != BUS_OK ||
            cmd_33_raw(CMD_LEDS_OFF,       sizeof(CMD_LEDS_OFF),       of, LED_CMD_RSP_LEN) != BUS_OK)
            { bus_disable(); return false; }
        bus_disable();
        delay(off_ms);
    }
    power_cycle_bus();
    return true;
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
static void read_rom_id(uint8_t rom_out[8]) {
    uint8_t rsp[8 + BASIC_INFO_LEN] = {0};
    cmd_33(CMD_BASIC_INFO, sizeof(CMD_BASIC_INFO), rsp, BASIC_INFO_LEN);
    memcpy(rom_out, rsp, 8);
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
    static const uint8_t enter[] = { 0x99 };
    uint8_t ign[1] = {0};
    if (cmd_cc(enter, sizeof(enter), ign, 0) != BUS_OK) { snprintf(out, 8, "BLxxxx"); return false; }
    static const uint8_t cmd[] = { 0x31 };
    uint8_t rsp[2] = {0};
    if (cmd_cc(cmd, sizeof(cmd), rsp, 2) != BUS_OK) { snprintf(out, 8, "BLxxxx"); return false; }
    snprintf(out, 8, "BL%02X%02X", rsp[1], rsp[0]);
    return true;
}

// ─── Battery type detection ───────────────────────────────────
// Priority: 5 (ROM[3]<100) → 6 (data[17]==30) → 0 → 2 → 3 → default 0.
// lock_only: if true, returns -1 for types 5/6 (write path unconfirmed).
static int8_t detect_battery_type_raw(const uint8_t rom[8], const uint8_t d[BASIC_INFO_LEN],
                                       bool lock_only) {
    if (rom[3] < 100)  return lock_only ? -1 : BATT_TYPE_5;
    if (d[17]  == 30)  return lock_only ? -1 : BATT_TYPE_6;

    { static const uint8_t c[] = {0xDC,0x0B}; uint8_t r[17]={0};
      if (cmd_cc(c,sizeof(c),r,17)==BUS_OK && r[16]==TYPE_PROBE_MAGIC) return 0; }

    { if (enter_testmode()==BUS_OK) {
          static const uint8_t c[] = {0xDC,0x0A}; uint8_t r[17]={0};
          BusResult br = cmd_cc(c,sizeof(c),r,17);
          exit_testmode(); delay(150);
          if (br==BUS_OK && r[16]==TYPE_PROBE_MAGIC) return 2; } }

    { static const uint8_t c[] = {0xD4,0x2C,0x00,0x02}; uint8_t r[3]={0};
      if (cmd_cc(c,sizeof(c),r,3)==BUS_OK && r[2]==TYPE_PROBE_MAGIC) return 3; }

    return 0;
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
    o.flags        = nybble_pair(d, 34, 35);
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
    bool all_ff = (r.checksum_n[0]==0xF && r.checksum_n[1]==0xF &&
                   r.checksum_n[2]==0xF && r.failure_code==0xF);
    info.locked = all_ff || !info.checksums_ok[0] || !info.checksums_ok[1] || !info.checksums_ok[2];
}

// ─── Health calculations ──────────────────────────────────────
static float calc_health_type56(const RawBasicInfo &r) {
    int16_t f_ol = max((int16_t)((int16_t)r.overload    - 29), (int16_t)0);
    int16_t f_od = max((int16_t)(35 - (int16_t)r.overdischarge), (int16_t)0);
    float   dmg  = r.cycles + r.cycles * (f_ol + f_od) / 32.0f;
    uint16_t scale = HEALTH_SCALE_STANDARD;
    for (uint8_t i = 0; i < sizeof(HEALTH_SCALE_EXTENDED_CODES); i++)
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
// Generic helper: send a 4-byte command, return rsp[0..rsp_len-2] if
// the last byte equals TYPE_PROBE_MAGIC.
static bool cc_probe(const uint8_t *cmd, uint8_t *rsp, uint8_t rsp_len) {
    if (cmd_cc(cmd, 4, rsp, rsp_len) != BUS_OK) return false;
    return rsp[rsp_len - 1] == TYPE_PROBE_MAGIC;
}

// Map type 0→0, 2→1, 3→2 for indexing into 3-entry command tables.
static uint8_t type023_idx(BatteryType t) {
    return (t == BATT_TYPE_2) ? 1 : (t == BATT_TYPE_3) ? 2 : 0;
}

static uint16_t read_health_raw(BatteryType t) {
    static const uint8_t cmds[3][4] = {
        {0xD4,0x50,0x01,0x02}, {0xD6,0x04,0x05,0x02}, {0xD6,0x38,0x02,0x02}
    };
    if (t!=BATT_TYPE_0 && t!=BATT_TYPE_2 && t!=BATT_TYPE_3) return 0;
    uint8_t r[3]={0};
    return cc_probe(cmds[type023_idx(t)], r, 3) ? le16(r) : 0;
}

static uint8_t read_od_count(BatteryType t) {
    static const uint8_t cmds[3][4] = {
        {0xD4,0xBA,0x00,0x01}, {0xD6,0x8D,0x05,0x01}, {0xD6,0x09,0x03,0x01}
    };
    if (t!=BATT_TYPE_0 && t!=BATT_TYPE_2 && t!=BATT_TYPE_3) return 0;
    uint8_t r[2]={0};
    return cc_probe(cmds[type023_idx(t)], r, 2) ? r[0] : 0;
}

static uint16_t read_overload_count(BatteryType t) {
    switch (t) {
        case BATT_TYPE_0: {
            static const uint8_t c[] = {0xD4,0x8D,0x00,0x07}; uint8_t r[8]={0};
            if (!cc_probe(c,r,8)) return 0;
            uint16_t a = ((uint16_t)(r[0]>>6)&0x03)|((uint16_t)r[1]<<2);
            uint16_t b =  (uint16_t) r[3]           |(((uint16_t)r[4]&0x03)<<8);
            uint16_t c2= ((uint16_t)(r[5]>>4))      |(((uint16_t)r[6]&0x3F)<<4);
            return a+b+c2;
        }
        case BATT_TYPE_2: {
            static const uint8_t c[] = {0xD6,0x5F,0x05,0x07}; uint8_t r[8]={0};
            return cc_probe(c,r,8) ? (uint16_t)(r[0]+r[2]+r[3]+r[5]+r[6]) : 0;
        }
        case BATT_TYPE_3: {
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

// ─── Temperature (types 5 and 6; 0/2/3 comes from voltage read) ──
static float read_temperature_type56(BatteryType t) {
    if (t == BATT_TYPE_5) {
        static const uint8_t c[] = {0x52}; uint8_t r[2]={0};
        return (cmd_cc(c,sizeof(c),r,2)==BUS_OK) ? le16(r)/10.0f - KELVIN_OFFSET : TEMP_INVALID;
    }
    if (t == BATT_TYPE_6) {
        static const uint8_t c[] = {0xD2}; uint8_t r[1]={0};
        return (cmd_cc(c,sizeof(c),r,1)==BUS_OK) ? (TEMP6_A + TEMP6_B*r[0])/100.0f : TEMP_INVALID;
    }
    return TEMP_INVALID;
}

// ─── Voltage reads ────────────────────────────────────────────
// Types 0/2/3: extended read (D7 00 00 FF) returns pack+cells+temps in one shot;
// falls back to short read (D7 00 00 0C) if extended fails.
static void read_voltages_type023(float *vpack, float cells[10], uint8_t ncells,
                                   float *t_cell, float *t_mosfet) {
    static const uint8_t ext[]   = {0xD7,0x00,0x00,0xFF};
    static const uint8_t short_[] = {0xD7,0x00,0x00,0x0C};
    uint8_t r[27]={0};
    bool ok = (cmd_cc(ext, sizeof(ext), r, 27)==BUS_OK) && (le16(&r[0])>0);
    if (ok) {
        *vpack = le16(&r[0]) / 1000.0f;
        for (uint8_t i = 0; i < min(ncells,(uint8_t)5); i++)
            cells[i] = le16(&r[2+i*2]) / 1000.0f;
        uint16_t rc = le16(&r[14]), rm = le16(&r[16]);
        *t_cell   = (rc > 1000) ? rc  /10.0f - KELVIN_OFFSET : TEMP_INVALID;
        *t_mosfet = (rm > 1000) ? rm  /10.0f - KELVIN_OFFSET : TEMP_INVALID;
    } else {
        uint8_t s[13]={0};
        if (cmd_cc(short_,sizeof(short_),s,13)!=BUS_OK || s[12]!=TYPE_PROBE_MAGIC) return;
        *vpack = le16(&s[0]) / 1000.0f;
        for (uint8_t i = 0; i < min(ncells,(uint8_t)5); i++)
            cells[i] = le16(&s[2+i*2]) / 1000.0f;
    }
}

// Type 5 (F0513): bare single-byte writes, no ROM prefix.
static void read_voltages_type5(float *vpack, float cells[10], uint8_t ncells) {
    float pack = 0;
    bus_enable();
    for (uint8_t i = 0; i < min(ncells,(uint8_t)5); i++) {
        if (!g_ow.reset()) continue;
        delayMicroseconds(BUS_BYTE_GAP_US);
        g_ow.write((uint8_t)(0x31+i), 0);
        delayMicroseconds(BUS_SETTLE_US);
        uint8_t r[2]={0}; r[0]=g_ow.read(); delayMicroseconds(BUS_BYTE_GAP_US); r[1]=g_ow.read();
        cells[i] = le16(r)/1000.0f;
        pack += cells[i];
    }
    bus_disable();
    *vpack = pack;
}

// Type 6: two-step read; ENABLE must stay HIGH across both steps.
static void read_voltages_type6(float *vpack, float cells[10], uint8_t ncells) {
    uint8_t ign[1]={0}, r[20]={0};
    bus_enable();
    { static const uint8_t c[]={0x10,0x21}; cmd_cc_raw(c,sizeof(c),ign,0); delay(10); }
    { static const uint8_t c[]={0xD4};      cmd_cc_raw(c,sizeof(c),r,20); }
    bus_disable();
    float pack=0;
    for (uint8_t i=0; i<min(ncells,(uint8_t)10); i++) {
        cells[i] = (6000.0f - le16(&r[i*2])/10.0f)/1000.0f;
        pack += cells[i];
    }
    *vpack = pack;
}

// ─── Diagnostic report ───────────────────────────────────────
static void print_report(const BatteryInfo &info, float vpack,
                           const float cells[10], const uint8_t d[BASIC_INFO_LEN]) {
    Serial.print(F("Model          : ")); Serial.println(info.model);
    Serial.print(F("ROM ID         : "));
    for (uint8_t i=0;i<8;i++) { if(info.rom_id[i]<0x10) Serial.print('0'); Serial.print(info.rom_id[i],HEX); if(i<7) Serial.print(' '); }
    Serial.println();
    char db[13]; snprintf(db,sizeof(db),"%02d/%02d/20%02d",info.rom_id[2],info.rom_id[1],info.rom_id[0]);
    Serial.print(F("Mfg date       : ")); Serial.println(db);
    Serial.print(F("Detected type  : ")); Serial.println((int)info.type);
    Serial.print(F("Battery type   : ")); Serial.print(info.raw.batt_type); Serial.print(F("  ("));
    if      (info.raw.batt_type<13) Serial.print(F("4 cell BL14xx"));
    else if (info.raw.batt_type<30) Serial.print(F("5 cell BL18xx"));
    else                            Serial.print(F("10 cell BL36xx"));
    Serial.println(')');
    Serial.print(F("Capacity       : ")); Serial.print(info.capacity_ah,1); Serial.println(F(" Ah"));

    print_sep();
    Serial.print(F("Lock status    : ")); Serial.println(info.locked ? F("LOCKED") : F("UNLOCKED"));
    Serial.print(F("Cell failure   : ")); Serial.println(info.raw.cell_failure ? F("YES") : F("No"));
    Serial.print(F("Failure code   : "));
    switch (info.raw.failure_code) {
        case  0: Serial.println(F("0 - OK")); break;
        case  1: Serial.println(F("1 - Overloaded")); break;
        case  5: Serial.println(F("5 - Warning")); break;
        case 15: Serial.println(F("15 - Critical fault (hard-locked by charger)")); break;
        default: Serial.println(info.raw.failure_code); break;
    }
    Serial.print(F("Checksum 0-15  : ")); Serial.println(info.checksums_ok[0] ? F("OK") : F("FAIL"));
    Serial.print(F("Checksum 16-31 : ")); Serial.println(info.checksums_ok[1] ? F("OK") : F("FAIL"));
    Serial.print(F("Checksum 32-40 : ")); Serial.println(info.checksums_ok[2] ? F("OK") : F("FAIL"));
    Serial.print(F("Aux CSum 44-47 : ")); Serial.println(info.aux_checksums_ok[0] ? F("OK") : F("FAIL"));
    Serial.print(F("Aux CSum 48-61 : ")); Serial.println(info.aux_checksums_ok[1] ? F("OK") : F("FAIL"));

    print_sep();
    Serial.print(F("Cycle count    : ")); Serial.println(info.raw.cycles);
    Serial.print(F("Health         : ")); Serial.print(info.health.rating,2); Serial.print(F(" / 4  ("));
    int h=(int)round(info.health.rating);
    for (int i=0;i<4;i++) Serial.print(i<h ? '#' : '-');
    Serial.println(')');

    if (info.type==BATT_TYPE_0 || info.type==BATT_TYPE_2 || info.type==BATT_TYPE_3) {
        Serial.print(F("OD events      : ")); Serial.println(info.health.od_count);
        Serial.print(F("Overload cnt   : ")); Serial.println(info.health.overload_count);
        if (info.health.od_count>0 && info.raw.cycles>0) {
            Serial.print(F("OD %           : ")); Serial.print(4.0f+100.0f*info.health.od_count/info.raw.cycles,1); Serial.println(F(" %"));
        }
        if (info.health.overload_count>0 && info.raw.cycles>0) {
            Serial.print(F("Overload %     : ")); Serial.print(4.0f+100.0f*info.health.overload_count/info.raw.cycles,1); Serial.println(F(" %"));
        }
    }
    if (info.type==BATT_TYPE_5 || info.type==BATT_TYPE_6) {
        Serial.print(F("OD %           : ")); Serial.print(-5.0f*info.raw.overdischarge+160.0f,1); Serial.println(F(" %"));
        Serial.print(F("Overload %     : ")); Serial.print( 5.0f*info.raw.overload     -160.0f,1); Serial.println(F(" %"));
    }
    if (info.temperature_c != TEMP_INVALID) {
        Serial.print(F("Temp (Cells)   : ")); Serial.print(info.temperature_c,1); Serial.println(F(" C"));
    }
    if (info.temperature_mosfet_c != TEMP_INVALID) {
        Serial.print(F("Temp (Mosfet)  : ")); Serial.print(info.temperature_mosfet_c,1); Serial.println(F(" C"));
    }

    print_sep();
    Serial.print(F("Pack voltage   : ")); Serial.print(vpack,3); Serial.println(F(" V"));
    float vmax=cells[0], vmin=cells[0];
    for (uint8_t i=0;i<info.cell_count;i++) {
        char lb[22]; snprintf(lb,sizeof(lb),"Cell %2u        : ",(unsigned)i+1);
        Serial.print(lb); Serial.print(cells[i],3); Serial.println(F(" V"));
        if (cells[i]>vmax) vmax=cells[i];
        if (cells[i]<vmin) vmin=cells[i];
    }
    Serial.print(F("Cell diff      : ")); Serial.print(vmax-vmin,3); Serial.println(F(" V"));

    if ((info.type==BATT_TYPE_0||info.type==BATT_TYPE_2||info.type==BATT_TYPE_3)
        && info.health.charge_level>0 && info.raw.capacity>0) {
        float ratio = (float)info.health.charge_level / info.raw.capacity / SOC_DIVISOR;
        uint8_t soc = (ratio<10.0f) ? 1 : (uint8_t)min((int)(ratio/10.0f),7);
        print_sep();
        Serial.print(F("State of charge: ")); Serial.print(soc); Serial.println(F(" / 7"));
    }

    print_sep();
    Serial.print(F("Frame          : "));
    for (uint8_t i=0;i<BASIC_INFO_LEN;i++) { if(d[i]<0x10) Serial.print('0'); Serial.print(d[i],HEX); if(i<BASIC_INFO_LEN-1) Serial.print(' '); }
    Serial.println();
}

// ─── Frame repair / write ────────────────────────────────────
// Shared charger-write sequence used by both unlock and lock paths.
// Sends: arm → 33-write → 33-store.  Caller must have g_charger_arm_issued == false.
static bool charger_write_frame(const uint8_t src[BASIC_INFO_LEN]) {
    if (g_charger_arm_issued) {
        Serial.println(F("  Arm already issued — power-cycle required."));
        return false;
    }
    { uint8_t r[BASIC_INFO_LEN]={0};
      if (cmd_cc(CMD_CHARGER_ARM,sizeof(CMD_CHARGER_ARM),r,BASIC_INFO_LEN)!=BUS_OK)
          { Serial.println(F("  Arm: no presence.")); return false; }
      g_charger_arm_issued=true; delay(50); }

    { uint8_t payload[2+BASIC_INFO_LEN]; payload[0]=FRAME_WRITE_OPCODE; payload[1]=FRAME_WRITE_PAD;
      memcpy(&payload[2],src,BASIC_INFO_LEN);
      uint8_t r[8]={0};
      if (cmd_33(payload,sizeof(payload),r,0)!=BUS_OK)
          { Serial.println(F("  Write: no presence.")); return false; }
      delay(50); }

    { uint8_t r[8]={0};
      if (cmd_33(CMD_STORE,sizeof(CMD_STORE),r,0)!=BUS_OK)
          { Serial.println(F("  Store: no presence.")); return false; }
      delay(100); }
    return true;
}

// Recompute all checksums in a frame copy, write it, power-cycle, and verify.
static bool write_corrected_frame(uint8_t data32[BASIC_INFO_LEN]) {
    uint8_t frame[BASIC_INFO_LEN]; memcpy(frame,data32,BASIC_INFO_LEN);
    nybble_set(frame,40,0);
    nybble_set(frame,41,checksum_calc(frame, 0, 15));
    nybble_set(frame,42,checksum_calc(frame,16, 31));
    nybble_set(frame,43,checksum_calc(frame,32, 40));
    nybble_set(frame,62,checksum_calc(frame,44, 47));
    nybble_set(frame,63,checksum_calc(frame,48, 61));

    Serial.print(F("  Frame to write: "));
    for (uint8_t i=0;i<BASIC_INFO_LEN;i++) { if(frame[i]<0x10) Serial.print('0'); Serial.print(frame[i],HEX); if(i<BASIC_INFO_LEN-1) Serial.print(' '); }
    Serial.println();

    if (enter_testmode()!=BUS_OK) { Serial.println(F("  No presence on test-mode enter.")); return false; }
    if (!charger_write_frame(frame)) { exit_testmode(); return false; }
    exit_testmode();
    power_cycle_bus();

    uint8_t verify[BASIC_INFO_LEN]={0};
    if (read_basic_info(verify)!=BASIC_INFO_OK) { Serial.println(F("  No response during verify.")); return false; }

    BatteryInfo chk; memset(&chk,0,sizeof(chk));
    parse_basic_info(verify,chk.raw); derive_battery_info(verify,chk);
    Serial.print(F("  Verify checksum 0-15  : ")); Serial.println(chk.checksums_ok[0] ? F("OK"):F("FAIL"));
    Serial.print(F("  Verify checksum 16-31 : ")); Serial.println(chk.checksums_ok[1] ? F("OK"):F("FAIL"));
    Serial.print(F("  Verify checksum 32-40 : ")); Serial.println(chk.checksums_ok[2] ? F("OK"):F("FAIL"));

    if (!chk.locked) { memcpy(data32,verify,BASIC_INFO_LEN); return true; }
    return false;
}

// ─── Unlock sequence (types 0, 2, 3 only) ────────────────────
static bool attempt_unlock(BatteryInfo &info, uint8_t data32[BASIC_INFO_LEN]) {
    power_cycle_bus();
    if (enter_testmode()!=BUS_OK) { Serial.println(F("  Unlock: no presence entering test mode.")); return false; }
    { uint8_t r[8+9]={0}; cmd_33(CMD_RESET_ERRORS,sizeof(CMD_RESET_ERRORS),r,9); delay(50); }
    exit_testmode(); delay(50);

    if (read_basic_info(data32)!=BASIC_INFO_OK) { Serial.println(F("  Unlock: no response after DA 04.")); return false; }
    parse_basic_info(data32,info.raw); derive_battery_info(data32,info);

    Serial.print(F("  Checksum 0-15  : ")); Serial.println(info.checksums_ok[0] ? F("OK"):F("FAIL"));
    Serial.print(F("  Checksum 16-31 : ")); Serial.println(info.checksums_ok[1] ? F("OK"):F("FAIL"));
    Serial.print(F("  Checksum 32-40 : ")); Serial.println(info.checksums_ok[2] ? F("OK"):F("FAIL"));

    if (!info.locked) { Serial.println(F("  -> UNLOCKED by DA 04.")); return true; }

    bool bad = !info.checksums_ok[0] || !info.checksums_ok[1] || !info.checksums_ok[2];
    if (bad) {
        Serial.println(F("  -> Checksums corrupt; trying frame write..."));
        led_purple(); flash_battery_leds(2);
        if (write_corrected_frame(data32)) {
            parse_basic_info(data32,info.raw); derive_battery_info(data32,info);
            Serial.println(F("  -> Frame write succeeded. UNLOCKED.")); return true;
        }
        Serial.println(F("  -> Frame write failed.")); return false;
    }
    Serial.println(F("  -> Still locked (non-checksum reason). Cannot recover."));
    return false;
}

static bool type_supports_unlock(BatteryType t) {
    return t==BATT_TYPE_0 || t==BATT_TYPE_2 || t==BATT_TYPE_3;
}

// ─── Scan steps ───────────────────────────────────────────────
static BasicInfoResult step_read_basic_info(uint8_t d[BASIC_INFO_LEN]) {
    for (uint8_t i=0; i<SCAN_MAX_RETRIES; i++) {
        BasicInfoResult r = read_basic_info(d);
        if (r!=BASIC_INFO_NO_RESPONSE) return r;
        delay(SCAN_RETRY_DELAY_MS);
    }
    return BASIC_INFO_NO_RESPONSE;
}

static void step_identify(BatteryInfo &info, const uint8_t d[BASIC_INFO_LEN]) {
    read_rom_id(info.rom_id);
    info.type = detect_battery_type(info.rom_id, d);
    (info.type==BATT_TYPE_5) ? read_model_type5(info.model) : read_model(info.model);
}

static void step_read_voltages(const BatteryInfo &info, float *vp, float cells[10],
                                float *tc, float *tm) {
    switch (info.type) {
        case BATT_TYPE_5: read_voltages_type5(vp, cells, info.cell_count); break;
        case BATT_TYPE_6: read_voltages_type6(vp, cells, info.cell_count); break;
        default:          read_voltages_type023(vp, cells, info.cell_count, tc, tm); break;
    }
}

static void step_read_health(BatteryInfo &info, const uint8_t d[BASIC_INFO_LEN]) {
    if (info.type==BATT_TYPE_5 || info.type==BATT_TYPE_6) {
        info.health.rating=calc_health_type56(info.raw);
    } else if (info.type==BATT_TYPE_0 || info.type==BATT_TYPE_2 || info.type==BATT_TYPE_3) {
        info.health.rating        = calc_health_type023(read_health_raw(info.type), info.raw.capacity);
        info.health.od_count      = read_od_count(info.type);
        info.health.overload_count= read_overload_count(info.type);
        info.health.charge_level  = read_charge_level();
    } else {
        info.health.rating = calc_health_damage_rating(d);
    }
}

static void step_handle_lock(BatteryInfo &info, uint8_t d[BASIC_INFO_LEN]) {
    if (!info.locked) {
        Serial.println(F("Battery UNLOCKED."));
        led_green();
        if (type_supports_unlock(info.type)) flash_battery_leds(3);
        led_green();
        return;
    }
    Serial.print(F("Battery LOCKED. Failure code: ")); Serial.println(info.raw.failure_code);
    if (!type_supports_unlock(info.type)) {
        Serial.print(F("Unlock not supported for type ")); Serial.print((int)info.type); Serial.println(F(". Skipping."));
        led_red(); return;
    }
    Serial.println(F("Attempting DA 04 unlock..."));
    led_yellow(); flash_battery_leds(2);
    if (attempt_unlock(info, d)) {
        Serial.println(F("Battery successfully unlocked."));
        led_green(); flash_battery_leds(3); led_green();
    } else {
        Serial.println(F("Could not unlock battery."));
        led_red();
    }
}

// ═══════════════════════════════════════════════════════════════
//  LOCK MODE — hard-lock sentinel writer
// ═══════════════════════════════════════════════════════════════
static bool lock_nybbles_ok(const uint8_t *buf) {
    for (uint8_t n=40; n<=43; n++) if (nybble_get(buf,n)!=LOCK_VAL) return false;
    return true;
}

static bool lock_read_frame(uint8_t frame[BASIC_INFO_LEN], uint8_t retries=15) {
    for (uint8_t i=0; i<retries; i++) {
        memset(frame,0,BASIC_INFO_LEN);
        if (cmd_cc(CMD_BASIC_INFO,sizeof(CMD_BASIC_INFO),frame,BASIC_INFO_LEN)!=BUS_OK) { delay(200); continue; }
        uint8_t orv=0,andv=0xFF;
        for (uint8_t j=0;j<BASIC_INFO_LEN;j++) { orv|=frame[j]; andv&=frame[j]; }
        if (orv==0x00 || andv==0xFF) { delay(200); continue; }
        return true;
    }
    return false;
}

static bool lock_apply(uint8_t frame[BASIC_INFO_LEN]) {
    for (uint8_t n=40; n<=43; n++) nybble_set(frame,n,LOCK_VAL);

    // Pass 1
    Serial.println(F("  [Lock] Pass 1: writing..."));
    led_orange();
    if (!charger_write_frame(frame)) { power_cycle_bus(); return false; }
    power_cycle_bus();

    uint8_t v[BASIC_INFO_LEN];
    if (!lock_read_frame(v,10)) { Serial.println(F("  [Lock] Pass 1 verify: no response.")); return false; }
    if (!lock_nybbles_ok(v))   { Serial.println(F("  [Lock] Pass 1 verify: nybbles did not stick.")); return false; }

    // Pass 2: confirm after another power-cycle
    power_cycle_bus();
    if (!lock_read_frame(v,10)) { Serial.println(F("  [Lock] Pass 2 verify: no response.")); return false; }
    if (lock_nybbles_ok(v)) { Serial.println(F("  [Lock] Lock held across power-cycle.")); return true; }

    // BMS self-repaired — re-lock from repaired frame
    Serial.println(F("  [Lock] Pass 2: BMS repaired — re-locking..."));
    for (uint8_t n=40; n<=43; n++) nybble_set(v,n,LOCK_VAL);
    led_orange();
    if (!charger_write_frame(v)) { power_cycle_bus(); return false; }
    power_cycle_bus();

    if (!lock_read_frame(v,10)) { Serial.println(F("  [Lock] Pass 3 verify: no response.")); return false; }
    if (lock_nybbles_ok(v)) { Serial.println(F("  [Lock] Pass 3: lock solid.")); return true; }
    Serial.println(F("  [Lock] Pass 3: BMS still resisting."));
    return false;
}

static bool run_lock() {
    exit_testmode(); power_cycle_bus();
    led_blue();
    Serial.println(F("============================================"));
    Serial.println(F("  [Lock] Battery detected — identifying..."));
    Serial.println(F("============================================"));

    uint8_t rom_id[8]={0}; read_rom_id(rom_id);

    uint8_t frame[BASIC_INFO_LEN];
    if (!lock_read_frame(frame)) {
        Serial.println(F("  [Lock] ERROR: No valid frame.")); led_red(); bus_disable(); return false;
    }
    { uint8_t andv=0xFF; for (uint8_t i=0;i<BASIC_INFO_LEN;i++) andv&=frame[i];
      if (andv==0xFF) { Serial.println(F("  [Lock] Pre-type-0 HC08 — unsupported.")); led_yellow(); bus_disable(); return false; } }

    int8_t type = lock_detect_type(rom_id, frame);
    Serial.print(F("  [Lock] Type: "));
    if (type<0) { Serial.println(F("unsupported (5, 6, or unknown). Skipping.")); led_yellow(); bus_disable(); return false; }
    Serial.println(type);

    if (lock_nybbles_ok(frame)) {
        Serial.println(F("  [Lock] Battery is already locked."));
        for (uint8_t i=0;i<3;i++) { led_red(); delay(150); led_off(); delay(100); }
        led_red(); bus_disable(); return true;
    }

    flash_battery_leds(2);
    Serial.println(F("  [Lock] Applying hard-lock sentinel..."));
    bool ok = lock_apply(frame);

    Serial.println(F("============================================"));
    Serial.println(ok ? F("  [Lock] LOCKED.") : F("  [Lock] FAILED."));
    for (uint8_t i=0;i<3;i++) { ok ? led_green() : led_red(); delay(200); led_off(); delay(100); }
    ok ? led_green() : led_red();
    Serial.println(F("============================================"));
    bus_disable();
    return ok;
}

// ─── Main scan ───────────────────────────────────────────────
static bool run_scan() {
    exit_testmode(); power_cycle_bus();
    led_blue();
    Serial.println(F("============================================"));
    Serial.println(F("  Makita Monitor - Scanning..."));
    Serial.println(F("============================================"));

    BatteryInfo info; memset(&info,0,sizeof(info));
    info.temperature_c = info.temperature_mosfet_c = TEMP_INVALID;
    info.type = BATT_TYPE_UNKNOWN;
    uint8_t data32[BASIC_INFO_LEN]={0};

    BasicInfoResult br = step_read_basic_info(data32);
    if (br==BASIC_INFO_NO_RESPONSE) {
        Serial.println(F("ERROR: Battery not responding after retries. Aborting."));
        led_off(); bus_disable(); return false;
    }
    if (br==BASIC_INFO_PRE_TYPE0) {
        print_sep();
        Serial.println(F("WARNING: Pre-type-0 HC08 battery detected!"));
        Serial.println(F("         Freescale MC908JK3E BMS -- no cell protection."));
        Serial.println(F("         DO NOT charge on any charger."));
        print_sep();
        led_red(); bus_disable(); return true;
    }

    parse_basic_info(data32,info.raw); derive_battery_info(data32,info);
    step_identify(info,data32);
    print_sep();
    Serial.print(F("Battery found  : ")); Serial.println(info.model);
    if (type_supports_unlock(info.type)) flash_battery_leds(1);

    float vpack=0, cells[10]={0};
    step_read_voltages(info,&vpack,cells,&info.temperature_c,&info.temperature_mosfet_c);
    if (info.type==BATT_TYPE_5 || info.type==BATT_TYPE_6)
        info.temperature_c = read_temperature_type56(info.type);
    step_read_health(info,data32);

    if (!battery_present()) {
        Serial.println(F("Battery removed during scan. Aborting."));
        led_off(); bus_disable(); return false;
    }

    print_sep(); print_report(info,vpack,cells,data32); print_sep();
    step_handle_lock(info,data32);
    print_sep(); Serial.println(F("Complete.")); bus_disable();
    return true;
}

// ─── Hot-swap state machine ───────────────────────────────────
enum ScanState { WAIT_BATTERY, SCAN_PENDING, IDLE };
static ScanState  g_state          = WAIT_BATTERY;
static uint32_t   g_last_poll      = 0;
static uint32_t   g_last_pulse     = 0;
static bool       g_last_lock_mode = false;

static const uint16_t MODE_DEBOUNCE_MS    = 50;
static const uint8_t  MODE_DEBOUNCE_COUNT = 4;
static uint8_t        g_mode_debounce     = 0;
static uint32_t       g_last_mode_poll    = 0;

// ─── Arduino entry points ─────────────────────────────────────
void setup() {
    Serial.begin(115200);
    pinMode(PIN_ENABLE, OUTPUT); digitalWrite(PIN_ENABLE, LOW);
    pinMode(PIN_MODE_OUT, OUTPUT); digitalWrite(PIN_MODE_OUT, HIGH);
    pinMode(PIN_MODE_IN, INPUT_PULLDOWN);
    g_pixel.begin(); g_pixel.setBrightness(80); led_off();
    delay(500);

    Serial.println(F("============================================"));
    if (lock_pin_read()) {
        Serial.println(F("  Makita LOCK Utility — Types 0 / 2 / 3 only"));
        Serial.println(F("  Bridge GPIO4-GPIO5 detected → LOCK MODE"));
        Serial.println(F("  Remove bridge to switch to scan/unlock mode."));
    } else {
        Serial.println(F("  Makita Monitor — Scan / Unlock Mode"));
        Serial.println(F("  's' = rescan | auto-detects connect/disconnect"));
        Serial.println(F("  Bridge GPIO4-GPIO5 to enter lock mode."));
    }
    Serial.println(F("============================================"));
    Serial.println(F("Waiting for battery..."));
    g_state = WAIT_BATTERY;
    g_last_lock_mode = lock_pin_read();
}

void loop() {
    uint32_t now = millis();
    bool lock_mode = g_last_lock_mode;

    // Mode-change detection (debounced, 4×50 ms ≈ 200 ms)
    if (now - g_last_mode_poll >= MODE_DEBOUNCE_MS) {
        g_last_mode_poll = now;
        bool raw = lock_pin_read();
        if (raw == g_last_lock_mode) {
            g_mode_debounce = 0;
        } else if (++g_mode_debounce >= MODE_DEBOUNCE_COUNT) {
            g_mode_debounce = 0; g_last_lock_mode = raw; lock_mode = raw;
            if (g_state==IDLE || g_state==WAIT_BATTERY) {
                led_off(); g_state = WAIT_BATTERY;
                Serial.println(F("============================================"));
                if (lock_mode) {
                    Serial.println(F("  Mode changed → LOCK MODE"));
                    Serial.println(F("  Bridge GPIO4-GPIO5 detected. Insert battery to lock."));
                } else {
                    Serial.println(F("  Mode changed → SCAN / UNLOCK MODE"));
                    Serial.println(F("  Bridge removed. Insert battery to scan."));
                }
                Serial.println(F("============================================"));
            }
        }
    }

    // Serial command (scan mode only)
    if (!lock_mode && Serial.available()) {
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
            Serial.println(lock_mode ? F("[Lock] Battery detected — locking...") : F("Battery detected — starting scan."));
            g_state = SCAN_PENDING;
        } else if (g_state==IDLE && !present) {
            Serial.println();
            Serial.println(F("============================================"));
            Serial.println(lock_mode ? F("  [Lock] Battery removed. Waiting for next...") : F("  Battery removed. Waiting..."));
            Serial.println(F("============================================"));
            led_off(); g_state = WAIT_BATTERY;
        }
    }

    // Idle pulse
    if (g_state==WAIT_BATTERY && now-g_last_pulse>=LED_PULSE_INTERVAL_MS) {
        g_last_pulse = now;
        led_pulse(lock_mode ? COL_ORANGE : Colour{80,80,80});
    }

    // Execute scan or lock
    if (g_state==SCAN_PENDING) {
        bool ok = lock_mode ? run_lock() : run_scan();
        g_state = ok ? IDLE : WAIT_BATTERY;
        if (!ok) { led_off(); Serial.println(F("Waiting for battery...")); }
    }
}
