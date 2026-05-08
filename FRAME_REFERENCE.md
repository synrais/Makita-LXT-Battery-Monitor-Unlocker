# Makita LXT Battery — Frame & Protocol Reference
## Compiled from OBI documentation, hands-on testing, and community frame data

---

## 1. The Frame — What It Is

Every Makita LXT battery contains a BMS chip that stores a **32-byte data frame**.  
This frame holds the battery's state — identity, health, cycle count, failure codes, and checksums.

The frame is read over a 1-Wire bus using the `AA 00` command.  
It is written back using a specific write sequence that requires entering test mode first.

### Nybble orientation

The frame is **nybble-oriented** — a nybble is 4 bits (half a byte).

```
Byte 0  = nybble 0 (low nybble, bits 0-3) + nybble 1 (high nybble, bits 4-7)
Byte 1  = nybble 2 (low) + nybble 3 (high)
...
Byte 31 = nybble 62 (low) + nybble 63 (high)
```

Total: **64 nybbles across 32 bytes**.

---

## 2. BMS Families — Old vs New

There are two fundamentally different BMS firmware families. **Do not mix values between them.**

| Family | Byte 0 | Example models | Notes |
|--------|--------|---------------|-------|
| **Old** | `0x50` | BL1815N, BL1840 (no B), early type 2 batteries | Pre-B suffix models, different frame layout |
| **New** | `0xF1` | BL1830B, BL1840B, BL1850B, BL1860B | All current production B-suffix models |

Old family batteries also have different values in bytes 1-2, byte 3, byte 12, and byte 13.  
Everything in this document from section 3 onwards applies to the **new family only** unless stated.

**Note on repair:** Byte 0 is never modified during frame repair regardless of family.  
The old/new family distinction is for identification purposes only — repair applies the same full sequence to all batteries.

Old family example (BL1840, 2015):
```
50 28 ED 83 10 96 00 00 B1 B1 71 41 A2 71 01 5B 82 E0 8E 47 50 65...
```

New family example (BL1860B, 2019):
```
F1 26 BD 13 14 58 00 00 D2 D2 40 21 D0 80 02 5E C3 D0 8E 67 60 FF...
```

---

## 3. Manufacturing Variant Encoding (New Family)

Within the new family, the **manufacturing origin** is encoded in bytes 1, 2, and 12.  
These are set at the factory and should **never be changed by frame repair**.

| Bytes 1-2 | Byte 12 | Status code | Factory | Cells | BMS chip |
|-----------|---------|-------------|---------|-------|----------|
| `26 BD` | `D0` | `45` / `67` | China | Murata | LIPW014/015/017 |
| `36 B6` | `01` | `1B` / `A5` | Vietnam | Samsung | Unknown |

The charger accepts **both variants** and does not validate any of these bytes.  
Frame repair leaves all variant bytes untouched — they are not modified during repair.

---

## 4. Status Code (Byte 19)

Byte 19 is the OBI tool "Status code" field (displayed as hex without `0x` prefix).

**Confirmed values per model and variant:**

| Status | Model | Variant | Notes |
|--------|-------|---------|-------|
| `67` | BL1860B | China/Murata | Universal on all healthy BL1860B tested |
| `67` | BL1830B | China/Murata | Healthy |
| `45` | BL1850B | China/Murata | Healthy |
| `1B` | BL1850B | Vietnam/Samsung | Healthy |
| `A5` | BL1830B | Any | Seen on batteries with dead cells |
| `47` | BL1840 (old) | Old family | Healthy |

**Important findings:**
- Status code is **fixed at manufacture** for healthy batteries — does not change with cycle count, OD events, or charge level
- `A5` appears on batteries with physically dead cells regardless of variant — likely a fault code, not a variant identifier
- The doc says bytes 38-39 "must not be zero" — consistent with this being a fixed identifier

---

## 5. Complete Nybble Map (New Family)

```
Byte  Nybbles   Field
────────────────────────────────────────────────────────────────────────
  0    0–1      INFORMATIONAL ONLY — NOT modified during repair
                New family = 0xF1, Old family = 0x50
                Byte 0 has no bearing on charging state.
                Never overwritten during repair.

  1    2–3      MANUFACTURING VARIANT — DO NOT OVERWRITE
                China/Murata: byte 1 = 0x26
                Vietnam/Samsung: byte 1 = 0x36

  2    4–5      MANUFACTURING VARIANT — DO NOT OVERWRITE
                China/Murata: byte 2 = 0xBD
                Vietnam/Samsung: byte 2 = 0xB6

  3    6–7      MANUFACTURING VARIANT — DO NOT OVERWRITE
                Nybble 6 = 3 on all new-family batteries tested (both variants).
                Nybble 7 varies by variant (1 = China, C = Vietnam).
                China byte 3 = 0x13, Vietnam byte 3 = 0xC3

  4    8–9      UNKNOWN — varies by charge state, leave unchanged during repair

  5   10–11     CONFIRMED CONSTANT: byte 5 = 0x58 across all new-family batteries

  6   12–13     CONFIRMED CONSTANT: bytes 6-7 = 0x00 0x00 across all new-family batteries

  7   14–15     UNKNOWN — leave unchanged during repair

  8   16–17     UNKNOWN — potentially BMS-managed (see Section 12)
  9   18–19     Writing wrong values here caused FC3 + checksum corruption
                after full physical battery power cycle in testing.
                Leave unchanged during repair.

 10   20–21     CONFIRMED CONSTANT: byte 10 = 0x40 across all new-family batteries

 11   22–23     DOC: Battery type (8-bit integer) — varies by model
                Nybble 22 = high nybble, nybble 23 = low nybble
                Decoded value:
                  < 13 = 4-cell BL14xx
                  13–29 = 5-cell BL18xx (value 18 common on BL18xx)
                  30 = 10-cell BL36xx

 12   24–25     MANUFACTURING VARIANT — DO NOT OVERWRITE
                China/Murata: byte 12 = 0xD0
                Vietnam/Samsung: byte 12 = 0x01

 13   26–27     CONFIRMED CONSTANT: byte 13 = 0x80 across all new-family batteries
                Nybble 26 = 0, Nybble 27 = 8
                Leave unchanged during repair.

 14   28–29     CONFIRMED CONSTANT: byte 14 = 0x02 across all new-family batteries

 15   30–31     UNKNOWN — leave unchanged during repair

 16   32–33     DOC: Capacity in 1/10 Ah (8-bit)
                Nybble 32 = high, nybble 33 = low
                BL1815N ≈ 15, BL1830B ≈ 28-30, BL1840B ≈ 36-40
                BL1850B ≈ 50-52, BL1860B = 60

 17   34–35     THE ORIGINAL MAKITA CHARGER LOCK NYBBLE
                Byte 17 = 0xD0 — constant across all batteries tested.
                Nybble 34 = 0  (low nybble) — present in ALL Makita LXT batteries
                Nybble 35 = D  (high nybble) — unknown purpose
                Any non-zero nybble 34 stops the battery from charging.
                Per Jansson: the original lock mechanism from the earliest protocol,
                carried forward in every subsequent battery for charger compatibility.
                Set byte 17 to 0xD0 during repair. — charger-validated.

 18   36–37     CONFIRMED CONSTANT: byte 18 = 0x8E across all new-family batteries
                DOC: "Some 16-bit value, must not be zero" — unknown purpose

 19   38–39     DOC: "Some 16-bit value, must not be zero"
                This is the status code.
                Fixed at manufacture — identifies factory/cell variant.
                MANUFACTURING VARIANT — DO NOT OVERWRITE

 20   40–41     Nybble 40 = Failure code
                  0  = OK
                  1  = Overloaded
                  5  = Warning
                  15 = Dead (FC_DEAD)
                Nybble 41 = Checksum CS0 (sum of nybbles 0–15 & 0x0F) — charger-validated.

 21   42–43     Nybble 42 = Checksum CS1 (sum of nybbles 16–31 & 0x0F)
                Nybble 43 = Checksum CS2 (sum of nybbles 32–40 & 0x0F) — charger-validated.

 22   44–45     DOC: Nybble 44 bit 2 = cell failure flag (1 = failed)
                Nybble 45 = UNKNOWN (BMS accepts all values, charger validation unknown)

 23   46–47     DOC: Nybble 46 bits 1–3 = damage rating (old family only)
                  Not applicable to type 0/2/3/5/6 batteries
                Nybble 47 = UNKNOWN (BMS accepts all values, charger validation unknown)

 24   48–49     DOC: Overdischarge counter (8-bit, nybble 48 high, 49 low)

 25   50–51     DOC: Overload counter (8-bit, nybble 50 high, 51 low)

 26   52–53     DOC: Cycle count bits
                Nybble 52 bit 0 = cycle count bit 12
                Nybble 53 = cycle count bits 8–11

 27   54–55     DOC: Cycle count bits
                Nybble 54 = cycle count bits 4–7
                Nybble 55 = cycle count bits 0–3
                Full decode: ((N52 & 1) << 12) | (N53 << 8) | (N54 << 4) | N55

 28   56–57     UNKNOWN — varies per battery, BMS accepts all values tested

 29   58–59     UNKNOWN — varies per battery, BMS accepts all values tested

 30   60–61     UNKNOWN — varies per battery, BMS accepts all values tested

 31   62–63     Nybble 62 = AUX Checksum 0 (sum of nybbles 44–47 & 0x0F)
                Nybble 63 = AUX Checksum 1 (sum of nybbles 48–61 & 0x0F)
                AUX checksums do NOT affect lock state
```

---

## 6. The Checksum System

Three primary checksums protect the frame:

```
checksum = (sum of nybbles in range) & 0x0F
```

| Checksum | Location | Covers | Effect if wrong |
|----------|----------|--------|-----------------|
| CS0 | Nybble 41 | Nybbles 0–15 | Battery LOCKED |
| CS1 | Nybble 42 | Nybbles 16–31 | No lock effect |
| CS2 | Nybble 43 | Nybbles 32–40 | Battery LOCKED |
| AUX0 | Nybble 62 | Nybbles 44–47 | No lock effect |
| AUX1 | Nybble 63 | Nybbles 48–61 | No lock effect |

**Always recalculate all five checksums after any frame change.**

After writing, the BMS goes bus-silent for ~3 seconds during flash commit, then wakes with committed data in place. First response = committed data. No intermediate state.

---

## 7. What Stops a Battery Charging

**[CONFIRMED by systematic testing]**

The charger validates exactly **three things**:

| Cause | Field | Notes |
|-------|-------|-------|
| Nybble 34 non-zero | Byte 17 low nybble must be `0` | The original Makita charger lock mechanism |
| CS0 mismatch | Nybble 41 ≠ sum(nybbles 0–15) & 0x0F | |
| CS2 mismatch | Nybble 43 ≠ sum(nybbles 32–40) & 0x0F | |

AUX checksums (nybbles 62-63) do NOT stop charging.

**Everything else is ignored by the charger**, confirmed by exhaustive testing of all 193 fields including:
- Nybble 40 (FC=15) — accepted
- Nybble 42 (CS1 wrong) — accepted
- Byte 1 (`0x00`, `0xFF`, `0x36` Vietnam in China battery) — all accepted
- Byte 18 (`0x00`, `0xFF`) — accepted
- Byte 19 (`0x00`, `0xFF`, `0xA5`) — all accepted
- All bytes 0–31 set to `0x00` and `0xFF` — all accepted except byte 17=0xFF (nybble 34≠0)

**Theory confirmation:** A fully zeroed frame and two random garbage frames all charged with only nybble 34=0, CS0 and CS2 correct.

Per Jansson: *"The earliest batteries were locked by the charger setting a certain nybble  
to a non-zero value. This is still present in all newer batteries so the charger remains  
compatible."* That nybble is **nybble 34**.

---

### 8 DA04 command sequence
```
1. Enter test mode: CC D9 96 A5  →  response 1 byte
2. Send DA04 via ROM: 33 [8 ROM bytes] DA 04  →  response 9 bytes
3. Exit test mode: CC D9 FF FF  →  response 1 byte
4. Power cycle bus
5. Poll until battery responds (~175ms)
```

---

## 9. Frame Write Safety

### Safe to write
- Nybble 34 → set to 0 (byte 17 = 0xD0)
- Nybble 40 → set to `0` to unlock
- Nybbles 41-43 → always recalculate
- Nybbles 44-55 → health, OD, overload, cycle count
- Nybbles 56-61 → unknown purpose but BMS accepts all tested values
- Nybbles 62-63 → always recalculate

### Never overwrite with hardcoded values
- Byte 0 → never touched during repair, no bearing on charging state
- Bytes 1-2 (nybbles 2-5) → manufacturing variant
- Byte 12 (nybbles 24-25) → manufacturing variant
- Byte 19 (nybbles 38-39) → status code / variant identifier

Always read these from the existing frame and preserve them exactly.

### Potentially BMS-managed (write with caution)
- Bytes 8-9 (nybbles 16-19) → writing specific values caused FC3 and checksum corruption after physical power cycle. Exact BMS behaviour not fully confirmed. Leave unchanged during repair.

---

## 10. Old Battery Behaviour (Type 2, Pre-2015)

- **Byte 0 = `0x50`** — completely different from `0xF1` new family
- **No model string** — `read_model()` returns empty
- **Health/cycle/OD/overload = 0** — old firmware never implemented these registers. Correct and expected, not corruption.
- **Damage rating** in nybble 46 applies to this family (not type 0/2/3/5/6 new family)
- **Aux checksums = 0x21** — correctly populated despite other health fields being zero
- **Type detection requires testmode** — probe enters testmode briefly then power cycles. Do NOT poll after this — let the chip settle.

---

## 11. Confirmed Values by Model

### Capacity class (nybble 34) — revised
Byte 17 = `0xD0` is confirmed constant across all batteries tested.
Nybble 34 = `0`, nybble 35 = `D` for all tested BL1830B, BL1850B, BL1860B.
Safe repair target: **nybble 34 = 0** for all batteries.

### Vietnam variant — byte 16 is constant
Vietnam/Samsung BL1850B batteries consistently show byte 16 = `0x43` regardless of
cycle count or charge state. China variant byte 16 varies by capacity and state.

### Confirmed frame constants (from Python analysis of 25+ batteries)

**Universal — same across ALL new-family batteries regardless of model or variant:**
```
Byte  5 = 0x58
Byte  6 = 0x00
Byte  7 = 0x00
Byte 10 = 0x40
Byte 11 = 0x21
Byte 13 = 0x80
Byte 14 = 0x02
Byte 17 = 0xD0
Byte 18 = 0x8E
```

**Variant-specific constants:**
```
China/Murata:   Byte 1=0x26, Byte 2=0xBD, Byte 3=0x13, Byte 4=0x14, Byte 12=0xD0
Vietnam/Samsung: Byte 1=0x36, Byte 2=0xB6, Byte 3=0xC3, Byte 4=0x18, Byte 12=0x01
                 Byte 16=0x43 (constant for all Vietnam BL1850B tested)
```

**Variable fields (battery-specific runtime data):**
```
Byte 0:      family identifier (0xF1 new, 0x50 old) — never modified during repair
Bytes 8-9:   charge-state related, varies
Byte 15:     capacity + other data, varies
Byte 16:     capacity (China varies by model, Vietnam constant at 0x43)
Byte 19:     status code (model+variant identifier)
Bytes 20-21: failure code + CS0 checksum
Bytes 21-23: CS1, CS2, cell failure flag
Bytes 23-30: health counters, cycle count, unknown runtime data
Byte 31:     AUX checksums
```

### Status code by model and variant
| Status | Model | Variant | Notes |
|--------|-------|---------|-------|
| `67` | BL1860B | China | Confirmed across 8+ samples |
| `67` | BL1830B | China | Confirmed |
| `45` | BL1850B | China | Confirmed across multiple samples |
| `1B` | BL1850B | Vietnam | Confirmed across multiple samples |
| `A5` | Any | Any | Dead cell condition — fault code |
| `47` | BL1840 | Old family | Confirmed |

### BL1860B — China/Murata (confirmed charges)
```
F1 26 BD 13 14 58 00 00 D2 D2 40
21 D0 80 02 5E C3 D0 8E 67 60 FF
00 83 F2 02 2E D4 10 CF 00 0B
```

### BL1850B — China/Murata
```
F1 26 BD 13 14 58 00 00 B1 B1 40
21 D0 80 02 1D 23 D0 8E 45 60 14
0A 03 02 02 0E 11 00 01 00 5D
```

### BL1850B — Vietnam/Samsung
```
F1 36 B6 C3 18 58 00 00 94 94 40
21 01 80 02 2A 43 D0 8E 1B F0 6B
00 23 02 02 0E 68 00 E6 00 49
```

### BL1830B — China/Murata
```
F1 26 BD 13 14 58 00 00 74 74 40
21 D0 80 02 5B C1 D0 8E 67 60 D4
00 63 02 02 0E CC 00 EB 00 39
```

### OBI Clean Frame (BL1850B reference)
```
F1 26 BD 13 14 58 00 00 94 94 40
21 D0 80 02 4E 23 D0 8E 45 60 1A
00 03 02 02 0E 20 00 30 01 83
```

---

## 12. Full Repair Sequence for a Non-Charging Battery

```
1. Read current frame
2. Zero nybble 34: frame[17] = frame[17] & 0xF0  (preserve nybble 35)
3. Recalculate CS0 (nybble 41) and CS2 (nybble 43)
4. Enter testmode → write → exit → power cycle → poll (~3s)
5. Send DA04 to clear internal BMS error register
6. Battery charges on first insert
```

The charger only checks nybble 34, CS0, and CS2. All other frame data is irrelevant to charging — the frame is written back with original data intact except for those fields.

---


*Compiled from OBI protocol documentation, hands-on testing of BL1815N/BL1830B/BL1840/BL1850B/BL1860B batteries, OBI clean frame source, Jansson protocol notes, and community frame data from 30+ batteries.*
