# Auto Makita Battery Monitor/Unlocker

A 1-Wire battery interrogation tool for Makita 18 V / 36 V Li-Ion packs.  
Reads model, ROM ID, voltages, temperature, health, cycle count, lock state, and more.  
Automatically detects insertion and removal. Performs charger-style auto-unlock on locked batteries.

The included **`makita_monitor.py`** script, **`Makita_Monitor.exe`** (Windows), and **`Makita_Monitor.apk`** (Android) are the recommended ways to view output. The PC versions auto-detect the device over USB serial, colourise the readout (lock state, health, errors, separators), and trigger a scan automatically on connect. The Android APK does the same over USB OTG — plug the monitor into your phone or tablet via an OTG cable and it connects and scans automatically. No serial monitor configuration needed on any platform, just run the app.

Runs on **Arduino Uno, Arduino Nano, ESP32-C3, and RP2040** — select your board in `platformio.ini`.
Pre-built UF2 supplied for RP2040 Zero.
---

## Supported Boards

| Board                   | PlatformIO env              | Notes                                  |
|-------------------------|-----------------------------|----------------------------------------|
| Arduino Uno             | `uno`                       |                                        |
| Arduino Nano            | `nano`                      |                                        |
| ESP32-C3 SuperMini      | `esp32-c3-devkitm-1`        | Pull-ups to 3.3 V only — see below     |
| Waveshare RP2040 Zero   | `waveshare_rp2040_zero`     | Recommended — UF2 available, see below |

---

## Hardware

The ENABLE pin switches a transistor or FET that powers the battery's BMS logic rail.  
Toggling it LOW for ≥ 300 ms fully discharges the BMS capacitors and resets the battery's internal firmware state — this is what a real charger does on insertion.

**Pull-up resistors must be connected to 3.3 V on all boards.** Even on the Arduino Uno/Nano, the battery's 1-Wire bus must not be pulled to 5 V.

### Pin assignments by board

| Signal       | Uno / Nano | ESP32-C3 SuperMini | RP2040 Zero |
|--------------|------------|--------------------|-------------|
| 1-Wire data  | 6          | 1                  | 6           |
| Bus enable   | 8          | 0                  | 8           |
| NeoPixel     | —          | —                  | 16          |

ESP32-C3 pin assignments can be overridden in `platformio.ini` via `ESP_EN_PIN` and `ESP_OW_PIN` build flags if your wiring differs from the SuperMini layout.

---

## NeoPixel LED Guide

> Applies to the RP2040 Zero only. Uno and Nano do not have an onboard NeoPixel.

| Colour    | Meaning                                              |
|-----------|------------------------------------------------------|
| Off       | No battery / idle between polls                      |
| 🟢 Green  | Battery model read successfully — scan starting      |
| 🔵 Blue   | Scan complete, battery unlocked and healthy          |
| 🟡 Yellow | Battery locked — unlock attempts in progress         |
| 🔴 Red    | Unlock failed, or failure code 15 (BMS dead)         |

---

## Prerequisites

- [VS Code](https://code.visualstudio.com/)
- [PlatformIO extension](https://platformio.org/install/ide?install=vscode) for VS Code
- Git (optional — you can also download as ZIP)

---

## Build and Flash

### Option A — Pre-built UF2 (RP2040 Zero only, quickest)

1. Download the latest `.uf2` file from the Releases page.
2. Hold the BOOT button on the RP2040 Zero while plugging it in via USB — it mounts as a USB drive.
3. Drag and drop the `.uf2` file onto the drive. It will reboot and start automatically.
4. Open any serial terminal at **115200 baud** to see output.

### Option B — Build from source (all boards)

1. Clone or download this repository and open the folder in VS Code.
2. PlatformIO will auto-detect the project via `platformio.ini`.
3. In the PlatformIO sidebar select the environment for your board (e.g. `waveshare_rp2040_zero`).
4. Click **Build**, then **Upload** with your board connected via USB.
5. Open the serial monitor at **115200 baud** to see output.

---

## Automated Cycle

The firmware runs a non-blocking state machine. You never need to press reset or manually trigger a scan — insert a battery and it runs; remove it and it waits for the next one.

### States

```
WAIT_BATTERY  ──►  SCAN_NOW  ──►  IDLE
     ▲                │              │
     │                │ scan failed  │ battery removed
     └────────────────┘◄─────────────┘
```

| State          | What is happening                                                    |
|----------------|----------------------------------------------------------------------|
| `WAIT_BATTERY` | Polling the bus every 800 ms for a presence pulse                   |
| `SCAN_NOW`     | Battery detected (or `s` pressed) — full scan about to execute      |
| `IDLE`         | Scan complete, polling every 800 ms to detect removal               |

### Presence Polling

Every 800 ms the firmware pulses `ENABLE_PIN` HIGH for 50 ms and fires a 1-Wire reset.  
If the battery answers with a presence pulse the state advances to `SCAN_NOW`.  
If the battery stops answering while in `IDLE` the state returns to `WAIT_BATTERY` and "Battery removed" is printed.

The poll does **not** run during an active scan — the scan blocks the main thread, so there is no risk of a stray reset pulse interrupting a transaction.

### Scan Sequence

When a scan is triggered the following steps execute in order:

#### 1 — Defensive test-mode exit
Before anything else, `exit_testmode()` is called. This sends the test-mode exit command (`0xCC 0xD9 0xFF 0xFF`) regardless of whether the battery is in test mode or not. If a previous scan was interrupted mid-flight (e.g. battery pulled during an unlock attempt) this clears any lingering firmware state on the new battery before the scan begins.

#### 2 — Model read (bounded retry)
`cmd_cc(0xDC 0x0C)` reads up to 7 ASCII characters of the model string (e.g. `BL1850`).  
The firmware retries up to 15 times (≈ 3 seconds) to allow the BMS to finish waking up.  
If no valid model is returned after 15 attempts the scan aborts and the state returns to `WAIT_BATTERY`.

#### 3 — Welcome flash
The NeoPixel turns **green** and the battery's own LEDs are flashed once via 1-Wire.  
This involves briefly entering test mode, sending the LED-on command, delaying, sending LED-off, then **exiting test mode** and power-cycling the bus. The battery is in a clean state before step 4.

#### 4 — ROM ID read
`cmd_33(0xAA 0x00)` issues a Read ROM command. The 8-byte ROM ID is captured from the first 8 bytes of the response and stored for type detection and display.

#### 5 — Basic info frame
`cmd_cc(0xAA 0x00)` retrieves a 32-byte nybble-packed frame containing:
- Battery type, cell count, capacity
- Flags, failure code
- Overdischarge and overload raw values
- Cycle count
- Three primary checksums and two auxiliary checksums

A battery is considered **locked** if any primary checksum fails, or if the failure-code nybbles are all `0xF` (BMS dead indicator).

#### 6 — Battery type detection
Type is determined in priority order using the ROM ID and basic-info frame:

| Priority | Type | Detection method                                                                    |
|----------|------|-------------------------------------------------------------------------------------|
| 1        | 5    | ROM ID byte 3 < 100 (F0513 chip variant)                                           |
| 2        | 6    | Basic-info byte 17 == 30 (BL36xx 10-cell pack)                                     |
| 3        | 0    | `cmd_cc(0xDC 0x0B)` last byte == 0x06                                              |
| 4        | 2    | Enter test mode → `cmd_cc(0xDC 0x0A)` last byte == 0x06 → **exit test mode**       |
| 5        | 3    | `cmd_cc(0xD4 0x2C 0x00 0x02)` last byte == 0x06                                    |
| 6        | 0    | Default fallback                                                                    |

Test mode is always exited at priority 4, even if the probe succeeded, so the battery cannot be left in test mode by this step.

#### 7 — Voltages
Voltage reads are type-specific:

- **Types 0 / 2 / 3** — `cmd_cc(0xD7 0x00 0x00 0x0C)` returns pack voltage + up to 5 cell voltages as 16-bit LE millivolts.
- **Type 5** — Five individual commands `0x31`–`0x35`, one per cell. Pack voltage is derived as the sum.
- **Type 6** — Two-step: `cmd_cc(0x10 0x21)` enters voltage-read mode (no response), then `cmd_cc(0xD4)` returns 20 bytes (10 × 16-bit LE raw values). Conversion: `V = (6000 − raw/10) / 1000`.

#### 8 — Temperature
- **Types 0 / 2 / 3** — `cmd_cc(0xD7 0x0E 0x00 0x02)` → 16-bit LE in tenths of Kelvin → convert to °C.
- **Type 5** — `cmd_cc(0x52)` → same format.
- **Type 6** — `cmd_cc(0xD2)` → single byte, formula: `(-40x + 9323) / 100`.

#### 9 — Health, counters, charge level

| Battery type | Health source                                                                      |
|--------------|------------------------------------------------------------------------------------|
| 5 / 6        | Calculated from cycle count, overload, and overdischarge fields in basic info      |
| 0 / 2 / 3    | Dedicated health command; also reads OD event count, overload counters, coulomb counter |
| Other        | Damage rating field from basic-info nybble 46                                      |

#### 10 — Lock state and auto-unlock
If the battery is unlocked the NeoPixel turns **blue** and the battery LEDs flash three times.

If the battery is **locked** and the failure code is 15 (BMS dead) the NeoPixel turns **red** and no unlock is attempted.

Otherwise the firmware attempts a charger-style unlock up to 5 times:
1. Power-cycle the bus (300 ms off / 600 ms on) — resets the BMS firmware
2. Enter test mode
3. Send the error-reset command (`0x33 0xDA 0x04`)
4. Exit test mode
5. Re-read and re-parse the basic info frame
6. Check all three primary checksums — if all pass, battery is unlocked

Each failed attempt waits progressively longer before retrying (`200 ms × attempt number`).  
If all 5 attempts fail the NeoPixel stays **red**.

If unlock succeeds the NeoPixel turns **blue** and the battery LEDs flash three times.

#### 11 — Scan complete
`disable_bus()` is called, ENABLE_PIN goes LOW, and the state transitions to `IDLE`.  
The firmware now polls every 800 ms for removal.

---

## Serial Monitor Readouts

Open the PlatformIO serial monitor at **115200 baud** (`pio device monitor`).  
All output is plain ASCII, one value per line, separated by `----` dividers.

### Startup banner
```
============================================
  Makita Monitor
  's' = rescan | auto-detects connect/disconnect
============================================
Waiting for battery...
```

### Battery detected
```
Battery detected — starting scan.
============================================
  Makita Monitor - Scanning...
============================================
```

### Identity block
```
--------------------------------------------
Battery found  : BL1850
--------------------------------------------
Model          : BL1850
ROM ID         : 28 4A F1 03 00 00 00 C2
Detected type  : 0
Battery type   : 18  (5 cell BL18xx)
Capacity       : 5.0 Ah
```

| Field           | Meaning                                                                 |
|-----------------|-------------------------------------------------------------------------|
| `Model`         | ASCII model string read directly from the battery (7 chars max)        |
| `ROM ID`        | 8-byte 1-Wire ROM identifier in hex — unique per cell                  |
| `Detected type` | Internal protocol type (0, 2, 3, 5, or 6) — determines which commands are used |
| `Battery type`  | Raw type byte from basic info + decoded cell count and series           |
| `Capacity`      | Rated capacity in Ah (raw value ÷ 10)                                  |

**Battery type raw ranges:**

| Raw value | Decoded        |
|-----------|----------------|
| 0 – 12    | 4-cell BL14xx  |
| 13 – 29   | 5-cell BL18xx  |
| 30+       | 10-cell BL36xx |

---

### Status block
```
--------------------------------------------
Lock status    : UNLOCKED
Cell failure   : No
Failure code   : 0 - OK
Checksum 0-15  : OK
Checksum 16-31 : OK
Checksum 32-40 : OK
Aux CSum 44-47 : OK
Aux CSum 48-61 : OK
```

| Field             | Meaning                                                               |
|-------------------|-----------------------------------------------------------------------|
| `Lock status`     | `UNLOCKED` = battery will supply power to tools. `LOCKED` = BMS is blocking output |
| `Cell failure`    | `YES` if nybble 44 bit 2 is set — indicates a cell-level fault detected by the BMS |
| `Failure code`    | `0` OK · `1` Overloaded · `5` Warning · `15` Critical (BMS dead)     |
| `Checksum 0-15`   | Primary checksum over basic-info nybbles 0–15. `FAIL` = data corrupt or battery left in test mode |
| `Checksum 16-31`  | Primary checksum over nybbles 16–31                                   |
| `Checksum 32-40`  | Primary checksum over nybbles 32–40. All three must be `OK` for UNLOCKED |
| `Aux CSum 44-47`  | Informational only — does not affect lock state                       |
| `Aux CSum 48-61`  | Informational only — does not affect lock state                       |

> **Why checksums fail:** The most common cause is the battery being left in test mode. A correctly timed charger handshake and explicit test-mode exit resolves this. The auto-unlock sequence handles it automatically.

---

### Metrics block
```
--------------------------------------------
Cycle count    : 47
Health         : 3.72 / 4  (###-)
OD events      : 2
Overload cnt   : 0
OD %           : 8.3 %
Temperature    : 24.6 C
```

| Field          | Meaning                                                                   |
|----------------|---------------------------------------------------------------------------|
| `Cycle count`  | Full charge/discharge cycles recorded by the BMS                         |
| `Health`       | Degradation rating 0.00–4.00. Bar shows nearest integer out of 4 (`#` = good, `-` = lost) |
| `OD events`    | Number of overdischarge events (types 0/2/3 only — read from dedicated counter) |
| `Overload cnt` | Sum of all overload event counters (types 0/2/3 only)                    |
| `OD %`         | `4 + 100 × OD_events / cycle_count` — relative overdischarge stress      |
| `Overload %`   | Same formula for overload events                                          |
| `Temperature`  | BMS thermistor reading in °C. Omitted if the battery type does not support it |

**Health rating scale:**

| Rating    | Bar    | Condition                       |
|-----------|--------|---------------------------------|
| 3.5 – 4.0 | `####` | Excellent                       |
| 2.5 – 3.5 | `###-` | Good                            |
| 1.5 – 2.5 | `##--` | Fair — consider retiring soon   |
| 0.5 – 1.5 | `#---` | Poor                            |
| 0.0 – 0.5 | `----` | Dead / not recoverable          |

For **types 5 and 6** there are no dedicated counter commands. OD % and Overload % are calculated directly from the raw fields in the basic-info frame using the formulas `(-5 × overdischarge + 160)` and `(5 × overload − 160)` respectively.

---

### Voltage block
```
--------------------------------------------
Pack voltage   : 19.843 V
Cell  1        : 3.969 V
Cell  2        : 3.971 V
Cell  3        : 3.967 V
Cell  4        : 3.970 V
Cell  5        : 3.966 V
Cell diff      : 0.005 V
```

| Field          | Meaning                                                                  |
|----------------|--------------------------------------------------------------------------|
| `Pack voltage` | Total stack voltage in volts                                             |
| `Cell N`       | Individual cell voltage. 4-cell packs show cells 1–4; 10-cell packs show 1–10 |
| `Cell diff`    | Max cell voltage minus min cell voltage. Values above ~0.050 V indicate imbalance |

---

### State of charge (types 0 / 2 / 3 only)
```
--------------------------------------------
State of charge: 6 / 7
```

Derived from the coulomb counter (`cmd_cc(0xD7 0x19 0x00 0x04)`).  
Scale is 0–7 where 7 is full. This is a coarse BMS-internal estimate, not a precise fuel gauge.

---

### Unlock output (locked batteries only)
```
Battery LOCKED. Failure code: 1
Attempting charger-style auto-unlock...
  Unlock attempt 1 / 5
  Checksum 0-15  : FAIL
  Checksum 16-31 : FAIL
  Checksum 32-40 : FAIL
  -> Still locked, retrying...
  Unlock attempt 2 / 5
  Checksum 0-15  : OK
  Checksum 16-31 : OK
  Checksum 32-40 : OK
  -> UNLOCKED.
Battery successfully unlocked.
```

Each attempt shows the three primary checksums so you can see exactly which pass first. A battery that unlocks on attempt 2 is normal — the BMS sometimes needs two power cycles to fully clear a fault latch.

---

### Removal detection
```
============================================
  Battery removed. Waiting...
============================================
```
Printed when the presence poll returns no response while in `IDLE` state.

---

## Manual Rescan

Send `s` (or `S`) over the serial monitor at any time to trigger an immediate rescan of the currently inserted battery. Useful after a tool draw-down or thermal event to check updated state.

---

## OneWire Library

This project uses a locally patched version of the OneWire library (`lib/OneWire/`).

Bit-level timing has been widened from the standard 1-Wire spec to match Makita's BMS timing requirements (reset pulse 750 µs, write-1 recovery 120 µs, write-0 low 100 µs). These values are the same across all supported boards.

Each platform uses its own optimised GPIO path:

| Platform | GPIO implementation                                                            |
|----------|--------------------------------------------------------------------------------|
| AVR (Uno / Nano) | Direct port register access — same as the upstream OneWire library    |
| ESP32-C3 | Direct register access via the ESP32 GPIO hardware block                       |
| RP2040   | Direct **Single-cycle IO (SIO)** register access at `0xD0000000`, giving single-clock-cycle GPIO transitions. `delayMicroseconds` is mapped to `busy_wait_us` from the Pico SDK for hardware-timer-backed delays within interrupt-disabled sections |
