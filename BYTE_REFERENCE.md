# Makita LXT Frame — Complete Byte Reference
## New Family Only (byte 0 = 0xF1): BL1830B, BL1850B, BL1860B

Derived from analysis of 20 real battery frames across China/Murata and Vietnam/Samsung variants.

---

## Quick Reference

```
Byte  0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15
     F1  [1] [2] [3] [4]  58  00  00  **  **  40  21  [5]  80  02  **

Byte 16  17  18  19  20  21  22  23  24  25  26  27  28  29  30  31
     [6]  D0  8E  [7]  **  **  **  **  **  **  **  **  **  **  **  CS

[n] = variant-specific constant    ** = variable data    CS = checksum
```

---

## Byte-by-Byte Detail

---

### Byte 0 — BMS Family / Format Identifier
```
Value:    0xF1  (constant — all new-family batteries)
Nybble 0: 0x1   (low nybble)
Nybble 1: 0xF   (high nybble)
Min/Max:  fixed 0xF1
```
Type: Universal constant  
Repair action: Leave unchanged

---

### Byte 1 — Manufacturing Variant (part 1)
```
China/Murata:    0x26  (constant for all China batteries)
Vietnam/Samsung: 0x36  (constant for all Vietnam batteries)
Min/Max:         0x26 – 0x36
```
Type: Variant constant  
Repair action: Leave unchanged

---

### Byte 2 — Manufacturing Variant (part 2)
```
China/Murata:    0xBD
Vietnam/Samsung: 0xB6
Min/Max:         0xB6 – 0xBD
```
Type: Variant constant  
Repair action: Leave unchanged

---

### Byte 3 — Manufacturing Variant (part 3)
```
China/Murata:    0x13
Vietnam/Samsung: 0xC3
Min/Max:         0x13 – 0xC3

Nybble 6 (low):  0x3  — same on both variants
Nybble 7 (high): 0x1 (China) / 0xC (Vietnam)
```
Type: Variant constant  
Repair action: Leave unchanged  

---

### Byte 4 — Manufacturing Variant (part 4)
```
China/Murata:    0x14
Vietnam/Samsung: 0x18
Min/Max:         0x14 – 0x18
```
Type: Variant constant  
Repair action: Leave unchanged

---

### Byte 5 — Unknown Constant
```
Value:   0x58  (constant — all new-family batteries)
Min/Max: fixed 0x58
```
Type: Universal constant  
Purpose unknown. Constant across every battery tested regardless of model or variant.  
Repair action: Leave unchanged

---

### Byte 6 — Unknown Constant
```
Value:   0x00  (constant — all new-family batteries)
Min/Max: fixed 0x00
```
Type: Universal constant  
Purpose unknown. Always zero.  
Repair action: Leave unchanged

---

### Byte 7 — Unknown Constant
```
Value:   0x00  (constant — all new-family batteries)
Min/Max: fixed 0x00
```
Type: Universal constant  
Purpose unknown. Always zero.  
Repair action: Leave unchanged

---

### Bytes 8–9 — Unknown / BMS State
```
Values seen:  0x04 – 0xF3 (wide range, both bytes always equal)
Both bytes always match each other (e.g. B1 B1, D2 D2, 94 94)
```
Type: Unknown / BMS state  
Purpose unknown. Both bytes are always identical to each other.  
Appears to relate to battery charge state or BMS internal status.  
Repair action: Leave unchanged

---

### Byte 10 — Unknown Constant
```
Value:   0x40  (constant — all new-family batteries)
Min/Max: fixed 0x40
```
Type: Universal constant  
Purpose unknown. Constant across every battery tested.  
Repair action: Leave unchanged

---

### Byte 11 — Battery Type
```
Value:   0x21  (5-cell BL18xx — nybbles 22=1, 23=2 → type value 18)
Min/Max: varies by model
Nybble 22 (low):  high nybble of battery type
Nybble 23 (high): low nybble of battery type
Decoded value:
  < 13 = 4-cell BL14xx
  13–29 = 5-cell BL18xx (value 18 common on BL18xx)
  30 = 10-cell BL36xx
```
Type: Battery type  
Repair action: Leave unchanged

---

### Byte 12 — Manufacturing Variant (part 5)
```
China/Murata:    0xD0
Vietnam/Samsung: 0x01
Min/Max:         0x01 – 0xD0
```
Type: Variant constant  
Repair action: Leave unchanged

---

### Byte 13 — Unknown Constant
```
Value:    0x80  (constant — all new-family batteries)
Nybble 26 (low):  0x0
Nybble 27 (high): 0x8
Min/Max:  fixed 0x80
```
Type: Universal constant  
Purpose unknown. Constant across every battery tested.  
Repair action: Leave unchanged

---

### Byte 14 — Unknown Constant
```
Value:   0x02  (constant — all new-family batteries)
Min/Max: fixed 0x02
```
Type: Universal constant  
Purpose unknown. Constant across every battery tested.  
Repair action: Leave unchanged

---

### Byte 15 — Capacity (partial)
```
Values seen: 0x0C – 0x7E
Nybble 30 (low):  lower nybble of capacity encoding
Nybble 31 (high): upper nybble of capacity encoding
```
Type: Variable  
This byte works together with byte 16 to encode capacity.  
The capacity field is nybble_pair(32,33) which spans bytes 15–16 (high nybbles of 15, low nybble of 16... see byte 16).  
Variable — depends on battery model and specific pack capacity.  
Encodes actual battery capacity, cannot be guessed.  
Repair action: Leave unchanged

---

### Byte 16 — Capacity (main) + Battery Type
```
China BL1860B:  0xC3  (nybbles 32=3,33=C → capacity=0x3C=60 → 6.0Ah)
China BL1850B:  0x23  (nybbles 32=3,33=2 → capacity=0x32=50 → 5.0Ah)
China BL1830B:  0xC1 or 0xE1  (capacity ≈ 28–30 → 2.8–3.0Ah)
Vietnam all:    0x43  (constant for all Vietnam BL1850B tested)
Min/Max:        0x23 – 0xE1

Nybble 32 (low):  high digit of capacity
Nybble 33 (high): low digit of capacity
Capacity = (nybble32 << 4) | nybble33
Capacity in 1/10 Ah — divide by 10 for Ah value
```
Type: Variable  
Encodes actual battery capacity, cannot be guessed.  
Repair action: Leave unchanged

---

### Byte 17 — The Original Makita Charger Lock Nybble
```
Value:    0xD0  (constant — confirmed across all batteries tested)
Nybble 34 (low):  0x0  — charger lock nybble
Nybble 35 (high): 0xD  — unknown purpose
Min/Max:  fixed 0xD0
```
Nybble 34 stops the battery — **Will stop charging** if non-zero — charger-validated.  
Present in ALL Makita LXT batteries — old and new family — for charger compatibility.  
Per OBI author Jansson: *"The earliest batteries were locked by the charger setting a  
certain nybble to a non-zero value. This is still present in all newer batteries."*  
This is that nybble.  
Repair action: Zero nybble 34, preserve nybble 35 — `frame[17] = frame[17] & 0xF0` — **charger-validated**

---

### Byte 18 — Unknown Constant
```
Value:   0x8E  (constant — all new-family batteries)
Min/Max: fixed 0x8E
```
Type: Universal constant  
Purpose unknown. Constant across every battery tested.  
Repair action: Leave unchanged

---

### Byte 19 — Status Code / BMS Variant Identifier
```
China BL1860B:   0x67
China BL1850B:   0x45
China BL1830B:   0x67
Vietnam BL1850B: 0x1B
Dead cell (any): 0xA5
Min/Max:         0x1B – 0xA5

Nybble 38 (low):  varies
Nybble 39 (high): varies
```
Type: Variable  
Fixed at manufacture — identifies factory and cell supplier.  
Repair action: Leave unchanged

---

### Byte 20 — Failure Code + CS0 High
```
Nybble 40 (low):  failure code
  0x0 = OK (unlocked)
  0x1 = Overloaded
  0x5 = Warning
  0xF = Dead (FC_DEAD)
Nybble 41 (high): Checksum CS0 = min(sum(nybbles 0–15), 0xFF) & 0x0F

Failure code min: 0x0 (OK)
Failure code max: 0xF (dead)
```
Type: Variable  
Nybble 40 is informational only.  
Bad CS0 (nybble 41) **Will stop charging** — charger-validated.  
Repair action: Recalculate CS0 (nybble 41) after any frame change — **charger-validated**

---

### Byte 21 — CS1 + CS2
```
Nybble 42 (low):  Checksum CS1 = min(sum(nybbles 16–31), 0xFF) & 0x0F
Nybble 43 (high): Checksum CS2 = min(sum(nybbles 32–40), 0xFF) & 0x0F
```
Type: Variable  
Bad CS2 (nybble 43) **Will stop charging** — charger-validated.  
Repair action: Recalculate CS2 (nybble 43) after any frame change — **charger-validated**

---

### Byte 22 — Cell Failure Flag (BMS-managed)
```
Value:   0x00 on all tested batteries (healthy cells)
Min/Max: 0x00 (healthy) — non-zero set by BMS if cell failure detected
Nybble 44 (low):  bit 2 = cell failure flag
Nybble 45 (high): 0x0 on all batteries tested
```
Type: BMS-managed cell failure flag (bit 2)  
BMS-managed cell failure register. `0x00` on all healthy batteries.  
[WARNING] Writing byte 22 = `0xFF` (nybble 44 = 0xF) caused the BMS to go bus-silent  
during testing — BMS did not respond until power cycle. Avoid high values.  
Repair action: Leave unchanged — **Note: BMS corrects bit 2 on boot**

---

### Byte 23 — Damage Rating
```
Values seen: 0x01 – 0x83
Nybble 46 (low):  damage rating bits 1-3 (valid range 0–7)
Nybble 47 (high): unknown
Min/Max:          0x01 – 0x83
```
Type: Variable  
Damage rating used by charger to calculate 0–4 health score.  
A rating below 3 = 4/4 health, 7 = 0/4 health.  
BMS-written.  
Repair action: Leave unchanged. Safe to reset nybble 46 to `0` (4/4 health) only if value is clearly corrupt (e.g. outside 0–7 range).

---

### Byte 24 — Overdischarge Counter (high)
```
Values seen: 0x02 – 0xF2
Nybble 48 (low):  overdischarge count high nybble
Nybble 49 (high): overdischarge count low nybble
Min/Max:          0x02 – 0xF2 (0x00 when no OD events)
```
Type: Variable  
BMS-written. Counts overdischarge events.  
Repair action: Leave unchanged — real battery history. Safe to zero out only if value is clearly corrupt (e.g. 0xFF with no corresponding history).

---

### Byte 25 — Overload Counter (high)
```
Values seen: 0x02 – 0x22
Nybble 50 (low):  overload count high nybble
Nybble 51 (high): overload count low nybble
Min/Max:          0x00 – 0x22
```
Type: Variable  
BMS-written. Counts overload events.  
Repair action: Leave unchanged — real battery history. Safe to zero out only if value is clearly corrupt.

---

### Byte 26 — Cycle Count (bits 12-8)
```
Values seen: 0x0E – 0x2E
Nybble 52 (low):  bit 0 = cycle count bit 12
Nybble 53 (high): cycle count bits 8–11
Min/Max:          0x0E – 0x2E
```
Type: Variable  
Part of the 13-bit cycle count field.  
Full decode: `((N52 & 1) << 12) | (N53 << 8) | (N54 << 4) | N55`  
BMS-written.  
Repair action: Leave unchanged — real battery history.

---

### Byte 27 — Cycle Count (bits 7-0)
```
Values seen: 0x00 – 0xD4  (wide range — increases with use)
Nybble 54 (low):  cycle count bits 4–7
Nybble 55 (high): cycle count bits 0–3
Min/Max:          0x00 – 0xD4
```
Type: Variable  
Part of the 13-bit cycle count. BMS-written.  
Repair action: Leave unchanged — real battery history. See byte 26.

---

### Byte 28 — Unknown Runtime Data
```
Values seen: 0x00 – 0x10
Min/Max:     0x00 – 0x10
```
Type: Unknown runtime data  
Purpose unknown. Varies per battery. BMS accepts all values (tested).  
Repair action: Leave unchanged

---

### Byte 29 — Unknown Runtime Data
```
Values seen: 0x00 – 0xF6  (wide range)
Min/Max:     0x00 – 0xF6
```
Type: Unknown runtime data  
Purpose unknown. Varies significantly per battery. BMS accepts all values (tested).  
Repair action: Leave unchanged

---

### Byte 30 — Unknown Runtime Data
```
Values seen: 0x00 (15 batteries), 0x01–0x04 (5 batteries)
Min/Max:     0x00 – 0x04
```
Type: Unknown runtime data  
Mostly zero. Non-zero values seen on healthy batteries — not a fault indicator.  
Charger confirmed does NOT validate this byte. BMS accepts all values (tested).  
Repair action: Leave unchanged

---

### Byte 31 — AUX Checksums
```
Values seen: 0x0B – 0xF5  (derived from data)
Nybble 62 (low):  AUX Checksum 0 = min(sum(nybbles 44–47), 0xFF) & 0x0F
Nybble 63 (high): AUX Checksum 1 = min(sum(nybbles 48–61), 0xFF) & 0x0F
```
Type: Variable  
AUX checksums do NOT affect lock state — mismatch does not lock the battery.  
Repair action: Leave unchanged

---

## Summary Table

| Byte | Value | Type | Repair action |
|------|-------|------|--------------|
| 0 | `0xF1` | Universal constant | Leave unchanged |
| 1 | variable | Variant constant | Leave unchanged |
| 2 | variable | Variant constant | Leave unchanged |
| 3 | variable | Variant constant | Leave unchanged |
| 4 | variable | Variant constant | Leave unchanged |
| 5 | `0x58` | Universal constant | Leave unchanged |
| 6 | `0x00` | Universal constant | Leave unchanged |
| 7 | `0x00` | Universal constant | Leave unchanged |
| 8 | variable | Unknown / BMS state | Leave unchanged |
| 9 | variable | Unknown / BMS state (= byte 8) | Leave unchanged |
| 10 | `0x40` | Universal constant | Leave unchanged |
| 11 | variable | Battery type | Leave unchanged |
| 12 | variable | Variant constant | Leave unchanged |
| 13 | `0x80` | Universal constant | Leave unchanged |
| 14 | `0x02` | Universal constant | Leave unchanged |
| 15 | variable | Capacity (partial) | Leave unchanged |
| 16 | variable | Capacity (main) | Leave unchanged |
| 17 | `0xD0` | Charger lock nybble (nybble 34) | Preserve nybble 35, zero nybble 34 - **charger-validated** |
| 18 | `0x8E` | Universal constant | Leave unchanged |
| 19 | variable | Status / variant ID | Leave unchanged |
| 20 | variable | Fault state (nybble 40) + CS0 (nybble 41) | Recalculate CS0 - **charger-validated** |
| 21 | variable | CS1 (nybble 42) + CS2 (nybble 43) | Recalculate CS2 - **charger-validated** |
| 22 | variable | BMS-managed cell failure flag (bit 2) | **Note - BMS corrects bit 2 on boot** |
| 23 | variable | Damage rating (nybble 46) + unknown | Leave unchanged |
| 24 | variable | Overdischarge counter | Leave unchanged |
| 25 | variable | Overload counter | Leave unchanged |
| 26 | variable | Cycle count bits 12-8 | Leave unchanged |
| 27 | variable | Cycle count bits 7-0 | Leave unchanged |
| 28 | variable | Unknown runtime data | Leave unchanged |
| 29 | variable | Unknown runtime data | Leave unchanged |
| 30 | variable | Unknown runtime data | Leave unchanged |
| 31 | variable | AUX checksums | Leave unchanged |

---

## Charger-Validated Fields

**[CONFIRMED by exhaustive systematic testing — 193 individual field tests + 6-test garbage frame theory confirmation]**

The charger validates exactly **three things**. Everything else is ignored.

| Field | Required value | Effect if wrong |
|-------|---------------|-----------------|
| Nybble 34 (byte 17 low) | `0x0` | Battery LOCKED |
| Nybble 41 / CS0 | min(sum(nybbles 0–15), 0xFF) & 0x0F | Battery LOCKED |
| Nybble 43 / CS2 | min(sum(nybbles 32–40), 0xFF) & 0x0F | Battery LOCKED |

**Theory confirmation:** A fully zeroed 32-byte frame (all `0x00`) with only these three  
conditions met charged successfully. Two random garbage frames with the same three  
conditions met also charged. Nybble 34≠0 or a wrong checksum locked immediately.

**Fields confirmed NOT to stop charging:**

| Field | Tested values | Result |
|-------|--------------|--------|
| Nybble 40 (FC) | `0xF` (FC_DEAD) | Accepted |
| Nybble 42 (CS1) | Deliberately wrong | Accepted |
| Nybble 62-63 (AUX) | Deliberately wrong | Accepted |
| Byte 0 | `0x00`, `0xFF` | Accepted |
| Byte 1 | `0x00`, `0xFF`, `0x36` | Accepted |
| Bytes 2, 3, 4, 12 | Opposite variant | Accepted |
| Byte 13 nybble 27 | `9` | Accepted |
| Byte 18 | `0x00`, `0xFF` | Accepted |
| Byte 19 | `0x00`, `0xFF`, `0xA5` | Accepted |
| Bytes 5–16, 22–31 | `0x00` and `0xFF` | All accepted |

---

*Derived from Python analysis of 20 real battery frames: 8× BL1860B, 4× BL1850B China, 5× BL1850B Vietnam, 2× BL1830B, 1× BL1830B fault.*
