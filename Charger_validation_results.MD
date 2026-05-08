# Makita LXT Charger Validation Results

Exhaustive field-by-field test of every byte and nybble in the 32-byte BMS frame.  
Each field was set to its minimum (0x00 / 0x0) and maximum (0xFF / 0xF) value, with all other  
fields correct, CRC corrected when required. The battery was then inserted into a charger to determine whether it accepted or rejected.

**Battery used:** BL1860B China/Murata (type 0)  
**Total tests:** 200+

---

## Result Key

| Symbol | Meaning |
|--------|---------|
| ✅ | Charged — charger accepted |
| ❌ | Did not charge — charger rejected |
| ⚠️ | BMS went bus-silent after write (recovered on retry) |

---

## Conclusion

The charger validates **exactly three things**. Everything else is ignored completely.

| Field | Requirement | Effect if wrong |
|-------|-------------|-----------------|
| **Nybble 34** | Must be `0` | ❌ Charger rejects |
| **CS0 (Nybble 41)** | min(sum(nybbles 0–15), 0xFF) & 0x0F | ❌ Charger rejects |
| **CS2 (Nybble 43)** | min(sum(nybbles 32–40), 0xFF) & 0x0F | ❌ Charger rejects |

This was further confirmed by a garbage frame test — a fully zeroed 32-byte frame and two random  
garbage frames all charged successfully with only these three conditions met.

---

## Full Sweep Results — Bytes 0–31

| Byte | 0x00 | 0xFF | Notes |
|------|------|------|-------|
| 0 | ✅ | ✅ | BMS family identifier (0xF1 new, 0x50 old) |
| 1 | ✅ | ✅ | Manufacturing variant part 1 (0x26 China, 0x36 Vietnam) |
| 2 | ✅ | ✅ | Manufacturing variant part 2 (0xBD China, 0xB6 Vietnam) |
| 3 | ✅ | ✅ | Manufacturing variant part 3 (0x13 China, 0xC3 Vietnam) |
| 4 | ✅ | ✅ | Manufacturing variant part 4 (0x14 China, 0x18 Vietnam) |
| 5 | ✅ | ✅ | Universal constant 0x58 |
| 6 | ✅ | ✅ | Universal constant 0x00 |
| 7 | ✅ | ✅ | Universal constant 0x00 |
| 8 | ✅ | ✅ | Unknown / BMS state (always equals byte 9) |
| 9 | ✅ | ✅ | Unknown / BMS state (always equals byte 8) |
| 10 | ✅ | ✅ | Universal constant 0x40 |
| 11 | ✅ | ✅ | Battery type encoded (nybbles 22–23) |
| 12 | ✅ | ✅ | Manufacturing variant part 5 (0xD0 China, 0x01 Vietnam) |
| 13 | ✅ | ✅ | Universal constant 0x80 |
| 14 | ✅ | ✅ | Universal constant 0x02 |
| 15 | ✅ | ✅ | Capacity partial (nybbles 30–31) |
| 16 | ✅ | ✅ | Capacity main (nybbles 32–33) |
| **17** | ✅ | ❌ | **Lock (nybble 34) non-zero — charger rejects -validated. (Nybble 35, not validated)** |
| 18 | ✅ | ✅ | Universal constant 0x8E |
| 19 | ✅ | ✅ | Status code / variant ID (fixed at manufacture) |
| 20 | ❌ | ❌ | **FC (nybble 40, not validated)) + CS0 (nybble 41) — charger rejects -validated** |
| 21 | ❌ | ❌ | **CS1 (nybble 42, not validated) + CS2 (nybble 43) — charger rejects -validated** |
| 22 | ✅ | ⚠️ | Cell failure flag (nybble 44 bit 2). BMS went bus-silent on 0xFF (nybble 44=0xF) — recovered on retry, then ✅ |
| 23 | ✅ | ✅ | Damage rating (nybble 46 bits 1–3) + unknown (nybble 47) |
| 24 | ✅ | ✅ | Overdischarge counter (nybbles 48–49) |
| 25 | ✅ | ✅ | Overload counter (nybbles 50–51) |
| 26 | ✅ | ✅ | Cycle count bits 12–8 (nybbles 52–53) |
| 27 | ✅ | ✅ | Cycle count bits 7–0 (nybbles 54–55) |
| 28 | ✅ | ✅ | Unknown runtime data |
| 29 | ✅ | ✅ | Unknown runtime data |
| 30 | ✅ | ✅ | Unknown runtime data |
| 31 | ✅ | ✅ | AUX checksums (nybbles 62–63) |

---

## Full Sweep Results — Nybbles 0–63

| Nybble | Field | 0x0 | 0xF | Notes |
|--------|-------|-----|-----|-------|
| 0 | Byte 0 low | ✅ | ✅ | BMS family low nybble (0x1 on new family) |
| 1 | Byte 0 high | ✅ | ✅ | BMS family high nybble (0xF on new family) |
| 2 | Byte 1 low | ✅ | ✅ | Manufacturing variant (0x6 on both China and Vietnam) |
| 3 | Byte 1 high | ✅ | ✅ | Manufacturing variant (0x2 China, 0x3 Vietnam) |
| 4 | Byte 2 low | ✅ | ✅ | Manufacturing variant (0xD China, 0x6 Vietnam) |
| 5 | Byte 2 high | ✅ | ✅ | Manufacturing variant (0xB on both China and Vietnam) |
| 6 | Byte 3 low | ✅ | ✅ | Manufacturing variant (0x3 on both China and Vietnam) |
| 7 | Byte 3 high | ✅ | ✅ | Manufacturing variant (0x1 China, 0xC Vietnam) |
| 8 | Byte 4 low | ✅ | ✅ | Manufacturing variant (0x4 China, 0x8 Vietnam) |
| 9 | Byte 4 high | ✅ | ✅ | Manufacturing variant (0x1 China, 0x1 Vietnam) |
| 10 | Byte 5 low | ✅ | ✅ | Universal constant (0x8) |
| 11 | Byte 5 high | ✅ | ✅ | Universal constant (0x5) |
| 12 | Byte 6 low | ✅ | ✅ | Universal constant (0x0) |
| 13 | Byte 6 high | ✅ | ✅ | Universal constant (0x0) |
| 14 | Byte 7 low | ✅ | ✅ | Universal constant (0x0) |
| 15 | Byte 7 high | ✅ | ✅ | Universal constant (0x0) |
| 16 | Byte 8 low | ✅ | ✅ | Unknown / BMS state |
| 17 | Byte 8 high | ✅ | ✅ | Unknown / BMS state |
| 18 | Byte 9 low | ✅ | ✅ | Unknown / BMS state (always equals byte 8) |
| 19 | Byte 9 high | ✅ | ✅ | Unknown / BMS state (always equals byte 8) |
| 20 | Byte 10 low | ✅ | ✅ | Universal constant (0x0) |
| 21 | Byte 10 high | ✅ | ✅ | Universal constant (0x4) |
| 22 | Byte 11 low | ✅ | ✅ | Battery type high nybble |
| 23 | Byte 11 high | ✅ | ✅ | Battery type low nybble |
| 24 | Byte 12 low | ✅ | ✅ | Manufacturing variant (0x0 China, 0x1 Vietnam) |
| 25 | Byte 12 high | ✅ | ✅ | Manufacturing variant (0xD China, 0x0 Vietnam) |
| 26 | Byte 13 low | ✅ | ✅ | Universal constant (0x0) |
| 27 | Byte 13 high | ✅ | ✅ | Universal constant (0x8) |
| 28 | Byte 14 low | ✅ | ✅ | Universal constant (0x2) |
| 29 | Byte 14 high | ✅ | ✅ | Universal constant (0x0) |
| 30 | Byte 15 low | ✅ | ✅ | Capacity partial low nybble |
| 31 | Byte 15 high | ✅ | ✅ | Capacity partial high nybble |
| 32 | Capacity high | ✅ | ✅ | Capacity high nybble (1/10 Ah) |
| 33 | Capacity low | ✅ | ✅ | Capacity low nybble (1/10 Ah) |
| **34** | **Charger lock nybble** | ✅ | ❌ | **Must be 0 — non-zero locks the battery - Charger-validated. Original Makita charger lock mechanism** |
| 35 | Byte 17 high | ✅ | ✅ | Unknown purpose — constant 0xD across all batteries tested |
| 36 | Byte 18 low | ✅ | ✅ | Universal constant (0xE) |
| 37 | Byte 18 high | ✅ | ✅ | Universal constant (0x8) |
| 38 | Status code low | ✅ | ✅ | Status code / variant ID low nybble (fixed at manufacture) |
| 39 | Status code high | ✅ | ✅ | Status code / variant ID high nybble (fixed at manufacture) |
| 40 | Failure code (FC) | ✅ | ✅ | 0=OK, 1=Overloaded, 5=Warning, 15=Dead |
| **41** | **CS0** | ❌ | ❌ | **Charger-validated — mismatch locks battery. Covers nybbles 0–15** |
| 42 | CS1 | ✅ | ✅ | Covers nybbles 16–31 |
| **43** | **CS2** | ❌ | ❌ | **Charger-validated — mismatch locks battery. Covers nybbles 32–40** |
| 44 | Cell failure flag | ✅ | ⚠️ | Bit 2 = cell failure. BMS went bus-silent on 0xF — recovered on retry, then ✅ |
| 45 | Unknown | ✅ | ✅ | Unknown purpose |
| 46 | Damage rating | ✅ | ✅ | Bits 1–3 = damage rating (0–7). Below 3 = 4/4 health, 7 = 0/4 health |
| 47 | Unknown | ✅ | ✅ | Unknown purpose |
| 48 | Overdischarge high | ✅ | ✅ | Overdischarge counter high nybble |
| 49 | Overdischarge low | ✅ | ✅ | Overdischarge counter low nybble |
| 50 | Overload high | ✅ | ✅ | Overload counter high nybble |
| 51 | Overload low | ✅ | ✅ | Overload counter low nybble |
| 52 | Cycle count bit 12 | ✅ | ✅ | Bit 0 = cycle count bit 12 |
| 53 | Cycle count bits 8–11 | ✅ | ✅ | Cycle count bits 8–11 |
| 54 | Cycle count bits 4–7 | ✅ | ✅ | Cycle count bits 4–7 |
| 55 | Cycle count bits 0–3 | ✅ | ✅ | Cycle count bits 0–3 |
| 56 | Unknown | ✅ | ✅ | Unknown runtime data (byte 28 low) |
| 57 | Unknown | ✅ | ✅ | Unknown runtime data (byte 28 high) |
| 58 | Unknown | ✅ | ✅ | Unknown runtime data (byte 29 low) |
| 59 | Unknown | ✅ | ✅ | Unknown runtime data (byte 29 high) |
| 60 | Unknown | ✅ | ✅ | Unknown runtime data (byte 30 low) |
| 61 | Unknown | ✅ | ✅ | Unknown runtime data (byte 30 high) |
| 62 | AUX Checksum 0 | ✅ | ✅ | Covers nybbles 44–47 |
| 63 | AUX Checksum 1 | ✅ | ✅ | Covers nybbles 48–61 |

---

## Targeted Confirmation Tests

Following the full sweep, targeted tests were run to confirm the findings and disprove previously assumed lock causes.

| # | Test | Expected | Result | |
|---|------|----------|--------|-|
| 1 | Nybble 34 = 0x4 | ❌ | ❌ | ✅ PASS |
| 2 | CS0 wrong (nybble 41 off by 1) | ❌ | ❌ | ✅ PASS |
| 3 | CS2 wrong (nybble 43 off by 1) | ❌ | ❌ | ✅ PASS |
| 4 | FC = 15 / FC_DEAD (nybble 40) | ✅ | ✅ | ✅ PASS |
| 5 | CS1 wrong (nybble 42 off by 1) | ✅ | ✅ | ✅ PASS |
| 6 | Byte 1 = 0x00 | ✅ | ✅ | ✅ PASS |
| 7 | Byte 1 = 0x36 (Vietnam code in China battery) | ✅ | ✅ | ✅ PASS |
| 8 | Byte 18 = 0x00 | ✅ | ✅ | ✅ PASS |
| 9 | Byte 19 = 0x00 | ✅ | ✅ | ✅ PASS |
| 10 | Byte 19 = 0xA5 (Dead code?) | ✅ | ✅ | ✅ PASS |

---

## Garbage Frame Theory Confirmation

To prove the charger only checks nybble 34, CS0, and CS2, three garbage frames were written and tested.

| # | Frame | N34 | CS0 | CS2 | Result |
|---|-------|-----|-----|-----|--------|
| 1 | All zeros (0x00 × 32) | 0 | ✅ | ✅ | ✅ Charged |
| 2 | Random garbage A | 0 | ✅ | ✅ | ✅ Charged |
| 3 | Random garbage B | 0 | ✅ | ✅ | ✅ Charged |
| 4 | All zeros — N34 = 0x4 | 4 | ✅ | ✅ | ❌ Rejected |
| 5 | Random garbage A — bad CS0 | 0 | ❌ | ✅ | ❌ Rejected |
| 6 | Random garbage B — bad CS2 | 0 | ✅ | ❌ | ❌ Rejected |
