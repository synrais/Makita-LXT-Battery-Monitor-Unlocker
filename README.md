# 🔋 (Automatic) Makita Battery Monitor / Unlocker

> Plug in a battery. Get instant readouts on voltage, health, temperature, cycle count, lock state, and more.  
> Locked battery? — no phone or PC required. It unlocks itself automatically.

---

## What It Does

Insert any Makita 18 V or 36 V Li-Ion pack and within seconds you get a full report:

- ✅ **Model & identity** — reads the battery's model string and unique ROM ID
- ⚡ **Voltage** — full pack voltage plus every individual cell, with imbalance flagging
- 🌡️ **Temperature** — live BMS thermistor reading in °C
- 🔋 **Health** — degradation score from 0–4 with a visual bar (`####`, `###-`, etc.)
- 🔄 **Cycle count** — how many full charge/discharge cycles the pack has seen
- 🔒 **Lock status** — LOCKED or UNLOCKED, with the specific failure code
- 🛠️ **Auto-unlock** — performs a charger-style unlock sequence automatically on locked packs

No configuration. No button presses. Just insert the battery and it runs. Remove it and the device waits for the next one.

---

## Status Light (RP2040 Zero)

| Colour    | Meaning                                              |
|-----------|------------------------------------------------------|
| Off       | No battery / idle                                    |
| 🟢 Green  | Battery detected — scan starting                     |
| 🔵 Blue   | Scan complete, battery healthy and unlocked          |
| 🟡 Yellow | Battery locked — unlock attempts in progress         |
| 🔴 Red    | Unlock failed, or BMS dead (failure code 15)         |

---

## Apps & Software

The included apps connect automatically over USB — no serial monitor setup needed.

| Platform | File | Notes |
|----------|------|-------|
| Windows  | `Makita_Battery_Monitor.exe` | Auto-detects USB serial, colourised output |
| Android  | `makita_battery_monitor_android_apk.zip` | Connect via USB OTG cable |
| Any OS   | `makita_battery_monitor.py` | Python script, same features |

All three trigger a scan automatically on connect and colourise the output (lock state, health, errors).

---

## Reading the Output

### 🔋 Health Rating

| Rating    | Bar    | What it means                   |
|-----------|--------|---------------------------------|
| 3.5 – 4.0 | `####` | Excellent                       |
| 2.5 – 3.5 | `###-` | Good                            |
| 1.5 – 2.5 | `##--` | Fair — consider retiring soon   |
| 0.5 – 1.5 | `#---` | Poor                            |
| 0.0 – 0.5 | `----` | Dead / not recoverable          |

### 🔒 Lock / Failure Codes

| Code | Meaning                          |
|------|----------------------------------|
| 0    | OK — no fault                    |
| 1    | Overloaded                       |
| 5    | Warning                          |
| 15   | Critical — BMS dead (no unlock attempted) |

### ⚡ Cell Imbalance

The `Cell diff` value is the spread between your highest and lowest cell voltage. Anything above **~0.050 V** is worth watching — it indicates the cells are drifting and may need balancing.

### 🔌 State of Charge (0–7 scale)

Available on most battery types. This is the BMS's own internal coarse estimate, not a precision fuel gauge — treat it as a rough indicator.

---

## Example Readout

```
============================================
  Makita Monitor - Scanning...
============================================
--------------------------------------------
Battery found  : BL1850
--------------------------------------------
Model          : BL1850
ROM ID         : 28 4A F1 03 00 00 00 C2
Capacity       : 5.0 Ah
--------------------------------------------
Lock status    : UNLOCKED
Failure code   : 0 - OK
Checksum 0-15  : OK
Checksum 16-31 : OK
Checksum 32-40 : OK
--------------------------------------------
Cycle count    : 47
Health         : 3.72 / 4  (###-)
OD events      : 2
Temperature    : 24.6 C
--------------------------------------------
Pack voltage   : 19.843 V
Cell  1        : 3.969 V
Cell  2        : 3.971 V
Cell  3        : 3.967 V
Cell  4        : 3.970 V
Cell  5        : 3.966 V
Cell diff      : 0.005 V
--------------------------------------------
State of charge: 6 / 7
```

---

## Auto-Unlock

If a battery is locked (and not dead), the monitor performs a charger-style unlock sequence automatically:

1. Power-cycles the bus to reset the BMS firmware
2. Sends the standard error-reset command
3. Re-checks all three checksums

It tries up to **5 times**, waiting a little longer between each attempt. You can watch it work in real time — each attempt prints the checksum results so you can see exactly when it clears. A battery that unlocks on attempt 2 is completely normal.

If all 5 attempts fail, the LED turns **red**. Failure code 15 (BMS dead) skips unlock entirely — those packs cannot be recovered this way.

---

## Manual Rescan

Send `s` (or `S`) over the serial monitor at any time to trigger a fresh scan of the currently inserted battery. Handy after a tool draw-down or thermal event to check updated state.

---
---

# Technical Reference

## Supported Boards

| Board                   | PlatformIO env              | Notes                                  |
|-------------------------|-----------------------------|----------------------------------------|
| Arduino Uno             | `uno`                       | Pull-ups to 3.3 V only — see below     |
| Arduino Nano            | `nano`                      | Pull-ups to 3.3 V only — see below     |
| ESP32-C3 SuperMini      | `esp32-c3-devkitm-1`        | Pull-ups to 3.3 V only — see below     |
| Waveshare RP2040 Zero   | `waveshare_rp2040_zero`     | Recommended — UF2 available            |

---

## Hardware

**Two 4.7 kΩ pull-up resistors are required** — one from the 1-Wire data pin to 3.3 V, and one from the bus enable pin to 3.3 V. Using the default pin assignments this means **pin 6 → 4.7 kΩ → 3.3 V** and **pin 8 → 4.7 kΩ → 3.3 V**. This is the standard 1-Wire pull-up value — lower values overdrive the bus and higher values cause slow rise times that break timing. Even on the Arduino Uno/Nano, both resistors must go to 3.3 V, not 5 V.

** **Optional** ** — A 1 kΩ load resistor may required across the battery's main power terminals (B+ to B−) as some batteries enter a deep sleep state and will not respond on the 1-Wire bus until they detect current draw on the power terminals — toggling ENABLE alone is not sufficient to wake them. A 1 kΩ resistor draws enough to reliably trigger the BMS wake-up while remaining cool enough for a standard ¼ W or ½ W resistor. Values above ~1.5 kΩ have been found insufficient to wake some batteries.
Plugging into a Makita charger should also wake a battery from this state.

**Something to note**, a battery will accept charge from any DC powersource in any state (Deep-sleep/Error), loading the battery with a 1 kΩ resistor then grounding the enable pin for a moment should make any locked BMS wake up and output power without clearing errors if the battery is above 8v charge.

### Pin Assignments

| Signal       | Uno / Nano | ESP32-C3 SuperMini | RP2040 Zero |
|--------------|------------|--------------------|-------------|
| 1-Wire data  | 6          | 1                  | 6           |
| Bus enable   | 8          | 0                  | 8           |
| NeoPixel     | —          | —                  | 16          |

ESP32-C3 pin assignments can be overridden in `platformio.ini` via `ESP_EN_PIN` and `ESP_OW_PIN` build flags if your wiring differs from the SuperMini layout.

---

## Prerequisites

- [VS Code](https://code.visualstudio.com/)
- [PlatformIO extension](https://platformio.org/install/ide?install=vscode) for VS Code
- Git (optional — you can also download as ZIP)

---

## Build and Flash

### Option A — Pre-built UF2 (RP2040 Zero only, quickest)

1. Download the `.uf2` file.
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
