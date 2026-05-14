# 🔋 (Automatic) Makita LXT Battery Monitor / Unlocker

> Plug in a battery. Get instant readouts on voltage, health, temperature, cycle count, lock state, and more.  
> Locked battery? — no phone or PC required. It unlocks itself automatically. RGB LED indicates current status.

---

## What It Does

Insert any Makita 18v or 36v LXT Li-Ion pack and within 2 seconds you get a full report:

- ✅ **Model & identity** — reads the battery's model string and unique ROM ID
- ⚡ **Voltage** — full pack voltage plus every individual cell, with imbalance flagging
- 🌡️ **Temperature** — live BMS Cell and Mosfet readings in °C
- 🔋 **Health** — degradation score from 0–4 with a visual bar (`####`, `###-`, etc.)
- 🔄 **Cycle count** — how many full charge/discharge cycles the pack has seen
- 🔒 **Lock status** — LOCKED or UNLOCKED, with the specific failure codes at fault listed
- 💡 **Status LED** — RP2040 Zero onboard NeoPixel LED shows device state at a glance
- 🛠️ **Auto-unlock** — performs a charger-style unlock sequence automatically on locked packs
- 🔧 **Frame repair** — comprehensively repairs corrupt frame fields whilst preserving all salvageable data
- 🔐 **Omega lock** — sets the original Makita charger lock (nybble 34) if GPIO2→GPIO3 bridged — Recoverable

No configuration. No button presses. Just insert the battery and it runs. Remove it and the device waits for the next one.

> Remember to have the PC/Phone plugged in and ready before inserting a battery if you want to see the results of the first scan!

## What It Does Not Do

- 🔥 **Fix blown fuses** — this tool cannot repair physical fuses inside the pack. After unlocking, verify your battery can actually output power. If not, open the pack and check the fuse — replace it if blown. Some battery's will have power on the outside pins but not the inside + which is also a blown fuse, it will work on a 2 pin tool but not a charger or 3 pin tool.

- 🔩 **Fix broken balancing tabs** — balancing tabs are the small metal strips connecting the PCB to each cell group. They commonly break from dropping the battery, mostly in 5Ah and 6Ah variants. If after a scan the status LED is flashing orange there is a good chance one is broken.
Lift the PCB at each tab to verify it is firmly connected, if not repair with some thick wire or solderbraid.

  A broken balancer tab typically shows a normal overall pack voltage, but one cell will read very low while an adjacent cell reads very high. This imbalance can force-blow the internal fuse via a heater circuit — the BMS sees simultaneous Overload and Overdischarge and triggers the thermal fuse as a last resort. The battery will continue to lock itself until the tab is repaired.


---

# Status Light (RP2040 Zero)

## Scan / Unlock Mode
| Colour      | Meaning                                                        |
|-------------|----------------------------------------------------------------|
| ⚪ White    | **Pulse**, No battery / idle                                   |
| 🔵 Blue     | Battery detected — scan starting                               |
| 🟡 Yellow   | Battery locked — error reset attempts in progress              |
| 🟣 Purple   | Frame corrupt — writing corrected frame to BMS                 |
| 🟢 Green    | Scan complete, battery healthy and unlocked                    |
| 🔴 Red      | Unlock failed or BMS dead                                      |
| 🟠 Orange   | **Blink**, Major cell imbalance or broken balancer tab         |

## Lock Mode (GPIO2→GPIO3 = Omega Lock) - Recoverable
| Colour      | Meaning                                                        |
|-------------|----------------------------------------------------------------|
| 🔴 Red      | **Pulse**, No battery / idle                                   |
| 🔵 Blue     | Battery detected — identifying                                 |
| 🟡 Yellow   | Unsupported battery type — skipping                            |
| 🟣 Purple   | Writing omega lock frame to BMS                                |
| 🟢 Green    | Battery successfully omega locked                              |
| 🔴 Red      | Already locked or lock failed                                  |

---

The included apps connect automatically over USB — no serial monitor setup needed.

| Platform | File | Notes |
|----------|------|-------|
| Windows  | `Makita_Battery_Monitor.exe` | Auto-detects USB serial, colourised output |
| Android  | `makita_battery_monitor_android_apk.zip` | Connect via USB OTG cable |
| Any OS   | `makita_battery_monitor.py` | Python script, same features |

All three can trigger a scan, and colourise the output (lock state, health, errors).

---

# Supported Battery Types

| Type | MCU | Voltages | Temperature | Health | Counters | Extended Stats | Unlock |
|------|-----|----------|-------------|--------|----------|--------------|--------|
| 0 — Standard (newest) | STM32L051 (confirmed) / RAJ240 (inferred) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| 2 | Unknown — inferred from BTC04 traces | ✅ | ✅ | ✅ | ✅ | — | ✅ |
| 3 | Unknown — inferred from BTC04 traces | ✅ | ✅ | ✅ | ✅ | — | ✅ |
| 5 — F0513 based | NEC/Renesas F0513 | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| 6 — 10 cell | Renesas RL78 | ✅ | ✅ | ✅ | — | — | — |
| Unknown / Old | Freescale MC908JK3E | — | — | ✅ | — | — | — |

> **Type 5 (F0513)** — unlock is not supported. The F0513 flash write sequence carries a high risk of permanently bricking the pack. Type 5 batteries are read-only.

> **Extended Stats** — Type 0 batteries expose a full SRAM stats block via the D7 command, providing SOC %, remaining charge, live current draw, learned and best capacity, cell and mosfet temperatures, error status, error counters, and SOC recalibration timing.

---

## Cell Imbalance Warning

After any scan, if the spread between the highest and lowest cell voltage is **0.300v or greater**, the LED will give a brief **orange blink every half-second**

A large cell voltage spread almost always indicates a **broken balancing tab** — the small metal strip connecting the PCB to a cell group. The battery will continue locking itself until the tab is repaired.

---

## Example Readout

![pc exe](Pictures/PC_exe.png)

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
| 1    | Overcurrent / overload           |
| 3    | Charging fault (fuse blown, mosfet shorted, or cell overvoltage at 4.37V) |
| 4    | Fault (sub-cause not yet mapped) |
| 5    | Warning                          |
| 7    | NTC temp diff >50°C, EEPROM error, cell overvoltage at 4.22V, or cell imbalance >300mV at idle |
| 15   | Learned capacity under 70% of Rated capacity"|

### ⚡ Cell Imbalance

The `Cell diff` value is the spread between your highest and lowest cell voltage. Anything above **~0.050v** is worth watching — it indicates the cells are drifting and may need balancing.

### 🔌 State of Charge (Type 0)

Type 0 batteries report a live SOC % directly from the BMS, along with remaining charge in Ah calculated from the battery's own learned capacity reference. This is a precision fuel gauge — not a coarse estimate.

---

## Auto-Unlock

If a battery is locked, the monitor performs an unlock sequence automatically. It stops as soon as a pass declares success.

### Attempt 1 — Error reset (yellow)
The monitor power-cycles the bus, enters test mode, and sends the standard `DA 04` error-reset command. This tells the BMS to clear its internal error register and attempt its own self-repair. The bus is then re-read and all lock causes are checked. This handles the most common real-world lock conditions — overdischarge, overload, naturally triggered failure codes. If the battery unlocks, the sequence stops here.

### Attempts 1–6 — Frame repair (purple)
If still locked after the DA04, frame repair runs immediately as part of the same attempt — and continues for up to 6 attempts total if needed. Based on exhaustive testing of every field in the frame, the charger validates exactly three things:

- **Nybble 34** — charger lock nybble, must be 0
- **CS0** — checksum of nybbles 0–15, must be correct
- **CS2** — checksum of nybbles 32–40, must be correct

The repair zeros nybble 34 and recalculates CS0 and CS2. All other frame data — cycle count, OD counter, overload counter, capacity, health history, variant bytes, status code, CS1, AUX checksums — is left completely untouched.

In practice almost all batteries unlock on the first attempt. If all attempts are exhausted without success the LED turns red.

---

## Lock Mode

Bridging GPIO2 → GPIO3 switches the device into **Omega Lock mode** (idle LED pulses red). This mode sets the original Makita charger lock nybble — nybble 34 — to a non-zero value. The charger checks this nybble before allowing charging to begin, a mechanism present in every Makita LXT battery from the earliest protocol through to current production.

The frame remains internally consistent — all checksums are valid, nybble 34 is the only thing non-zero. The battery will report UNLOCKED to any software that only checks checksums and failure codes, but the charger rejects it immediately. DA04 alone cannot undo this lock on newer batteries — the omega lock must be reversed by zeroing nybble 34 and recalculating checksums, which the scan/unlock mode does automatically.

When a battery is detected the device reads the frame and identifies the battery type. Types 5, 6, and unknown are rejected immediately (yellow LED) — only types 0, 2, and 3 are supported. If the battery is already omega locked, the device reports it and stops.

Lock is attempted up to six times. After each write the frame is read back to confirm nybble 34 is non-zero. On success the LED flashes green. On failure it flashes red. Remove the bridge to return to scan/unlock mode.

---

## Manual Rescan

Hit `s` over the serial monitor at any time to trigger a fresh scan of the currently inserted battery. Handy after a tool draw-down or thermal event to check updated state.

---
---

# Technical Reference

## Supported Boards

| Board                   | PlatformIO env              | Notes                                  |
|-------------------------|-----------------------------|----------------------------------------|
| Arduino Uno             | `uno`                       | Pull-ups to 5v — see below            |
| Arduino Nano            | `nano`                      | Pull-ups to 5v — see below            |
| ESP32-C3 SuperMini      | `esp32-c3-devkitm-1`        | Pull-ups to 3.3v only — see below     |
| Waveshare RP2040 Zero   | `waveshare_rp2040_zero`     | Pull-ups to 3.3v only — UF2 available |

---

## Hardware

**Two 4.7 kΩ pull-up resistors are required** — one from the 1-Wire data pin to supply voltage, and one from the bus enable pin to supply voltage. Using the default pin assignments this means **pin 6 → 4.7 kΩ → VCC** and **pin 8 → 4.7 kΩ → VCC**.

This is the standard 1-Wire pull-up value — lower values overdrive the bus and higher values cause slow rise times that break timing.

**Pull-up voltage depends on your board:**

- **Arduino Uno / Nano** — pull up to **5v**. The ATmega328P runs at 5v and its logic HIGH threshold (~3.0v) leaves almost no margin when pulling up to 3.3v, causing unreliable reads. The battery's BMS data pin can tolerate 5v in practice.
- **ESP32-C3 / RP2040 Zero** — pull up 4.7 kΩ to **3.3v only**. These are 3.3v-native devices and 5v on a GPIO will damage them.

> **Optional** — A 1 kΩ load resistor may be required across the battery's main power terminals (B+ to B−), as some batteries enter a deep sleep state and will not respond on the 1-Wire bus until they detect current draw on the power terminals. Plugging into a Makita charger or 2 pin device will also wake a battery from this state.

> Also as precaution connect the Bus enable and 1-Wire of your battery to your chip via 120 Ω resistors to mitigate risk of a pin getting stuck high or low or blowing (It helps alot!). Make sure they are directly connected to the GPIO pins and your battery Bus enable and 1-Wire pins! Dying pins will fail to show type 5 voltages at first, then fail for everything, RP2040 only. Nano is fine.

> **Note** — A battery will accept charge from any DC power source in any state (deep sleep or error). Loading the battery with a 1 kΩ resistor then briefly grounding the enable pin should make any locked BMS wake up and output power without clearing errors, provided the battery is above ~8v.

### Pin Assignments

| Signal       | Uno / Nano | ESP32-C3 SuperMini | RP2040 Zero |
|--------------|------------|--------------------|-------------|
| 1-Wire data  | 6          | 1                  | 6           |
| Bus enable   | 8          | 0                  | 8           |

ESP32-C3 pin assignments can be overridden in `platformio.ini` via `ESP_EN_PIN` and `ESP_OW_PIN` build flags if your wiring differs from the SuperMini layout.

![wiring diagram](Pictures/wiring.png)
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

### Option B — Locate precompiled firmware for your board in the Firmwares folder and upload

### Option C — Build from source (all boards)

1. Clone or download this repository and open the folder in VS Code.
2. PlatformIO will auto-detect the project via `platformio.ini`.
3. In the PlatformIO sidebar select the environment for your board (e.g. `waveshare_rp2040_zero`).
4. Click **Build**, then **Upload** with your board connected via USB.
5. Open the serial monitor at **115200 baud** to see output.


Many thanks to the original project, check it out there is probably far more info and regular updates.
- [open-battery-information](https://github.com/mnh-jansson/open-battery-information/tree/main)
