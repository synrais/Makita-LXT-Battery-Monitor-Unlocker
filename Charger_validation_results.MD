# Makita LXT Charger Validation Results

Exhaustive field-by-field test of every byte and nybble in the 32-byte BMS frame.  
Each field was set to its minimum (0x00 / 0x0) and maximum (0xFF / 0xF) value, with all other  
fields correct. The battery was then inserted into a charger to determine whether it accepted or rejected.

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
| **CS0 (Nybble 41)** | sum(nybbles 0–15) & 0x0F | ❌ Charger rejects |
| **CS2 (Nybble 43)** | sum(nybbles 32–40) & 0x0F | ❌ Charger rejects |

This was further confirmed by a garbage frame test — a fully zeroed 32-byte frame and two random  
garbage frames all charged successfully with only these three conditions met.

---

## Full Sweep Results — Bytes 0–31

| Byte | 0x00 | 0xFF | Notes |
|------|------|------|-------|
| 0 | ✅ | ✅ | |
| 1 | ✅ | ✅ | |
| 2 | ✅ | ✅ | |
| 3 | ✅ | ✅ | |
| 4 | ✅ | ✅ | |
| 5 | ✅ | ✅ | |
| 6 | ✅ | ✅ | |
| 7 | ✅ | ✅ | |
| 8 | ✅ | ✅ | |
| 9 | ✅ | ✅ | |
| 10 | ✅ | ✅ | |
| 11 | ✅ | ✅ | |
| 12 | ✅ | ✅ | |
| 13 | ✅ | ✅ | |
| 14 | ✅ | ✅ | |
| 15 | ✅ | ✅ | |
| 16 | ✅ | ✅ | |
| **17** | ✅ | ❌ | **Nybble 34 non-zero — charger rejects** |
| 18 | ✅ | ✅ | |
| 19 | ✅ | ✅ | |
| 20 | ❌ | ❌ | **Contains FC (nybble 40) + CS0 (nybble 41) — charger rejects** |
| 21 | ❌ | ❌ | **Contains CS1 + CS2 — charger rejects** |
| 22 | ✅ | ⚠️ | BMS went bus-silent on 0xFF (nybble 44=0xF) — recovered on retry, then ✅ |
| 23 | ✅ | ✅ | |
| 24 | ✅ | ✅ | |
| 25 | ✅ | ✅ | |
| 26 | ✅ | ✅ | |
| 27 | ✅ | ✅ | |
| 28 | ✅ | ✅ | |
| 29 | ✅ | ✅ | |
| 30 | ✅ | ✅ | |
| 31 | ✅ | ✅ | |

---

## Full Sweep Results — Nybbles 0–63

| Nybble | Field | 0x0 | 0xF | Notes |
|--------|-------|-----|-----|-------|
| 0 | Byte 0 low | ✅ | ✅ | |
| 1 | Byte 0 high | ✅ | ✅ | |
| 2 | Byte 1 low | ✅ | ✅ | |
| 3 | Byte 1 high | ✅ | ✅ | |
| 4 | Byte 2 low | ✅ | ✅ | |
| 5 | Byte 2 high | ✅ | ✅ | |
| 6 | Byte 3 low | ✅ | ✅ | |
| 7 | Byte 3 high | ✅ | ✅ | |
| 8 | Byte 4 low | ✅ | ✅ | |
| 9 | Byte 4 high | ✅ | ✅ | |
| 10 | Byte 5 low | ✅ | ✅ | |
| 11 | Byte 5 high | ✅ | ✅ | |
| 12 | Byte 6 low | ✅ | ✅ | |
| 13 | Byte 6 high | ✅ | ✅ | |
| 14 | Byte 7 low | ✅ | ✅ | |
| 15 | Byte 7 high | ✅ | ✅ | |
| 16 | Byte 8 low | ✅ | ✅ | |
| 17 | Byte 8 high | ✅ | ✅ | |
| 18 | Byte 9 low | ✅ | ✅ | |
| 19 | Byte 9 high | ✅ | ✅ | |
| 20 | Byte 10 low | ✅ | ✅ | |
| 21 | Byte 10 high | ✅ | ✅ | |
| 22 | Byte 11 low | ✅ | ✅ | |
| 23 | Byte 11 high | ✅ | ✅ | |
| 24 | Byte 12 low | ✅ | ✅ | |
| 25 | Byte 12 high | ✅ | ✅ | |
| 26 | Byte 13 low | ✅ | ✅ | |
| 27 | Byte 13 high | ✅ | ✅ | |
| 28 | Byte 14 low | ✅ | ✅ | |
| 29 | Byte 14 high | ✅ | ✅ | |
| 30 | Byte 15 low | ✅ | ✅ | |
| 31 | Byte 15 high | ✅ | ✅ | |
| 32 | Capacity high | ✅ | ✅ | |
| 33 | Capacity low | ✅ | ✅ | |
| **34** | **Charger lock nybble** | ✅ | ❌ | **Must be 0 — non-zero locks the battery** |
| 35 | Byte 17 high | ✅ | ✅ | |
| 36 | Byte 18 low | ✅ | ✅ | |
| 37 | Byte 18 high | ✅ | ✅ | |
| 38 | Status code low | ✅ | ✅ | |
| 39 | Status code high | ✅ | ✅ | |
| 40 | Failure code (FC) | ✅ | ✅ | |
| **41** | **CS0** | ❌ | ❌ | **Charger-validated — mismatch locks battery** |
| 42 | CS1 | ✅ | ✅ | |
| **43** | **CS2** | ❌ | ❌ | **Charger-validated — mismatch locks battery** |
| 44 | Cell failure flag | ✅ | ⚠️ | BMS went bus-silent on 0xF — recovered on retry, then ✅ |
| 45 | Unknown | ✅ | ✅ | |
| 46 | Damage rating | ✅ | ✅ | |
| 47 | Unknown | ✅ | ✅ | |
| 48 | Overdischarge high | ✅ | ✅ | |
| 49 | Overdischarge low | ✅ | ✅ | |
| 50 | Overload high | ✅ | ✅ | |
| 51 | Overload low | ✅ | ✅ | |
| 52 | Cycle count bit 12 | ✅ | ✅ | |
| 53 | Cycle count bits 8–11 | ✅ | ✅ | |
| 54 | Cycle count bits 4–7 | ✅ | ✅ | |
| 55 | Cycle count bits 0–3 | ✅ | ✅ | |
| 56 | Unknown | ✅ | ✅ | |
| 57 | Unknown | ✅ | ✅ | |
| 58 | Unknown | ✅ | ✅ | |
| 59 | Unknown | ✅ | ✅ | |
| 60 | Unknown | ✅ | ✅ | |
| 61 | Unknown | ✅ | ✅ | |
| 62 | AUX Checksum 0 | ✅ | ✅ | |
| 63 | AUX Checksum 1 | ✅ | ✅ | |

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
| 10 | Byte 19 = 0xA5 | ✅ | ✅ | ✅ PASS |

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

