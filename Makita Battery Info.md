From https://github.com/rosvall/makita-lxt-protocol/tree/main

## Basic battery information command
As far as I know, all batteries support this command, and all return data in the same annoying format.

Command: One of
 * `cc aa 00`
 * `33 [read 8 bytes of rom id] aa 00`
 * `cc f0 00`
 * `33 [read 8 bytes of rom id] f0 00`

The BTC04 and the DC18RC uses the last form.

Note that repeating the command in a form that ends in `f0 00` will change some state, and cause the battery to stop responding. This state is cleared by pulling the enable pin low for a short while.

### Response (32 bytes)
Nybble oriented, with least significant nybble first, such that nybble 0 is the 4 LSB of byte 0.

| Nybble | Description                                                                                  |
| ------ | --------------------------------                                                             |
|  0..15 | ?                                                                                            |
| 16..21 | ?                                                                                            |
|     22 | Battery type, high 4 bits                                                                    |
|     23 | Battery type, low 4 bits                                                                     |
| 24..31 | ?                                                                                            |
|     32 | Capacity in 1/10Ah, high 4 bits                                                              |
|     33 | Capacity in 1/10Ah, low 4 bits                                                               |
|     34 | Flags, high 4 bits                                                                           |
|     35 | Flags, low 4 bits                                                                            |
|     36 | Some 16 bit value, must not be zero                                                          |
|     37 | Some 16 bit value, must not be zero                                                          |
|     38 | Some 16 bit value, must not be zero                                                          |
|     39 | Some 16 bit value, must not be zero                                                          |
|     40 | Failure code. 0=OK, 1=Overloaded, 5=Warning, 15=*TODO*. BMS considered dead if not 0 or 5.   |
|     41 | Checksum of nybbles 0..15  (battery locked if not matching)                                  |
|     42 | Checksum of nybbles 16..31 (battery locked if not matching)                                  |
|     43 | Checksum of nybbles 32..40 (battery locked if not matching)                                  |
|     44 | Bit 2 set on cell failure                                                                    |
|     45 | ?                                                                                            |
|     46 | Bits 1..3: Damage rating for old batteries                                                   |
|     47 | ?                                                                                            |
|     48 | Overdischarge, high 4 bits                                                                   |
|     49 | Overdischarge, low 4 bits                                                                    |
|     50 | Overload, high 4 bits                                                                        |
|     51 | Overload, low 4 bits                                                                         |
|     52 | Bit 0: Cycle count, bit 12                                                                   |
|     53 | Cycle count, bits 8..11                                                                      |
|     54 | Cycle count, bits 4..7                                                                       |
|     55 | Cycle count, bits 0..3                                                                       |
| 56..61 | ?                                                                                            |
|     62 | Checksum of nybbles 44..47                                                                   |
|     63 | Checksum of nybbles 48..61                                                                   |


#### Checksums / locked state
The checksums are calculated like this:

```python
min(sum(nybbles), 0xff) & 0xf
```

That doesn't look like a good checksum function, and it might not be, but it seems to be used as one by the BTC04:

If the checksums of nybbles 0..15, 16..31, and 32..40 doesn't match nybbles 41, 42, and 43 respectively, the battery is considered broken (locked?). The battery is also considered broken, if nybbles 40..43 equals 0xffff.

If the battery is put into *test mode*, some of the checksums will fail, and the battery is in fact locked (no tool power, won't charge). Exiting test mode restores functionality, and checksums match again.

The checksums of nybbles 44..47 and 48..61 are calculated and checked, but doesn't seem to factor into anything displayed on the BTC04.


#### Battery type (nybbles 22..33)
8 bit integer B:

0 <= B < 13: 4 cell battery. BL14xx?

13 <= B < 30: 5 cell battery. BL18xx.

B = 20: 5 cell battery, but special somehow. *TODO*

B = 30: 10 cell battery. Probably BL36xx.

Observed values in BL18xx batteries: 18 and 20.


#### Capacity (nybbles 32, 33)
8 bit integer.

Looks very much like batter capacity in units of 1/10 Ah, even though it's off by 10% for some batteries.

Used as is (the entire 8 bit integer) by BTC04 in various calculations.

Some observed values in BL18xx batteries:

 * BL1815N: 15
 * BL1830B: 28 or 30
 * BL1840: 36 or 40
 * BL1850B: 50 or 52


#### Flags (nybbles 34, 35)
Probably flags.

The battery is type 6 (a 10 cell non-xgt battery, likely BL36xx) if the value is 0x1e = 0b0001_1110.

Observed values in BL18xx batteries: 0x0d and 0x0e.


#### Damage rating (nybble 46, 3 MSB)
3 bit integer.

Possibly only applicable to very old battery types, i.e. not type 0, 2, 3, 5, or 6.
For these batteries, the BTC04 will convert this value to the 0-4 health rating.

A damage rating below 3 corresponds to 4/4 health, 7 corresponds to 0/4 health, etc.


#### Overdischarge (nybbles 48, 49) 
8 bit integer.

For type5 and type6 batteries, BTC04 calculates overdischarge percentage *p* as follows:
```python
p = -5*x + 160
```

Must be non-zero for the battery to be of type 0.

Some observed values in BL18xx batteries: 27, 30, 31, 32, 33


#### Overload (nybbles 50, 51)
8 bit integer.

For type5 and type6 batteries, BTC04 calculates overload percentage as follows:


```python
p = 5*x - 160
```

Must be non-zero for the battery to be of type 0.

Some observed values in BL18xx batteries: 26, 30, 31, 32, 34



#### Cycle count (nybbles 52..55)
13 bit integer.

Valid for all battery types.

Number of charge-discharge cycles the battery has been through.


#### Health calculation for type5 and type6
For batteries of type 5 (F0513 based) or type 6 (10 cell, likely BL36xx).

BTC04 calculates *h*, its health rating on a scale from 0 to 4,
from the above raw values for capacity, overdischarge, cycle count, and overload:

```python
f_ol = max(overload - 29, 0)
f_od = max(35 - overdischarge, 0)
dmg = cycles + cycles * (f_ol + f_od) / 32
scale = 1000 if capacity in (26, 28, 40, 50) else 600
h = 4 - dmg / scale
```

# Battery type 0

## Identification
Command: `cc dc 0b`

If battery supports this command, it is type 0.

The command is supported, if the last byte of the response is `06`.

### Response (17 bytes)
| First byte | Last byte | Description |
| ---------- | --------- | ----------- |
|         0  |        15 | ?           |
|        16  |        16 | Always `06` |

Note: For batteries that support both `cc dc 0a` and `cc dc 0b`, the response is the identical.

## Enter test mode *(same for type 0, 2 and 3)*
Command: `cc d9 96 a5`

### Response (1 byte)
Always `06`

## Exit test mode *(same for type 0, 2 and 3)*
Command: `cc d9 ff ff`

### Response (1 byte)
Always `06`

## Read model string
Command: `cc dc 0c`

### Response (16 bytes)
| First byte | Last byte | Description                                   |
| ---------- | --------- | --------------------------------------------- |
|         0  |        15 | Battery model as null-terminated ASCII string |


## Overdischarge count
Command: `cc d4 ba 00 01`

### Response (2 bytes)
| First byte | Last byte | Description                           |
| ---------- | --------- | ------------------------------------- |
|         0  |         0 | Integer count of overdischarge events |
|         1  |         1 | Always `06`                           |

BTC04 calculates overdischarge percentage *p* as:
```python
if overdischarge_count > 0 and cycle_count > 0:
  p = 4 + 100 * overdischarge_count / cycle_count
else:
  p = 0
```


## Overload counters
Command: `cc d4 8d 00 07`

### Response (8 bytes)
| Byte | Bits | Description                      |
| ---- | ---- | -------------------------------- |
|    0 | 0..5 | ?                                |
|    0 | 6..7 | Counter A (10 bit), low 2 bits   |
|    1 | 0..7 | Counter A (10 bit), high 8 bits  |
|    2 | 0..7 | ?                                |
|    3 | 0..7 | Counter B (10 bit), low 8 bits   |
|    4 | 0..1 | Counter B (10 bit), high 2 bits  |
|    4 | 2..7 | ?                                |
|    5 | 0..3 | ?                                |
|    5 | 4..7 | Counter C (10 bit), low 4 bits   |
|    6 | 0..5 | Counter C (10 bit), high 6 bits  |
|    6 | 6..7 | ?                                |
|    7 | 0..7 | Always `06`                      |

In the BTC04, the three counters are added together. They might count different types of overload events.
Counter C is stored separately though.

BTC04 calculates overload percentage *p* as:

```python
if sum(counters) > 0 and cycle_count > 0:
  p = 4 + 100 * sum(counters) / cycle_count
else:
  p = 0
```

## Health
Command: `cc d4 50 01 02`

### Response (3 bytes)
| First byte | Last byte | Description                            |
| ---------- | --------- | -------------------------------------- |
|         0  |         1 | Health as 16 bit little endian integer |
|         2  |         2 | Always `06`                            |

BTC04 calculates the health rating *h* on a scale from 0 to 4 as follows:

```python
ratio = health / capacity
if ratio > 80:
  h = 4
else:
  h = ratio / 10 - 5
```

where *health* is the 16 bit integer from the response, and *capacity* is that raw capacity value in units of 1/10Ah reported in the reponse to the basic battery information command `cc aa 00`

## Temperature *(same for type 0, 2 and 3)*
Command: `cc d7 0e 00 02`


### Response (3 bytes)
| First byte | Last byte | Description                                            |
| ---------- | --------- | ------------------------------------------------------ |
|         0  |         1 | Temperature in 1/10 K, as little endian 16 bit integer |
|         2  |         2 | Always `06`                                            |


## Charge level (coulomb counter?)
Command: `cc d7 19 00 04`

### Response (5 bytes)
| First byte | Last byte | Description                                  |
| ---------- | --------- | -------------------------------------------- |
|         0  |         3 | Charge level as 32 bit little endian integer |
|         4  |         4 | Always `06`                                  |

BTC04 calculates battery pack state of charge *sof* on a scale of 0 to 7 as follows:

```python
ratio = charge_level / capacity / 2880

if ratio == 0:
  sof = 0
elif ratio < 10:
  sof = 1
else:
  sof = min(ratio / 10, 7)
```

where *charge_level* is the 32 bit integer from the response, and *capacity* is that raw capacity value in units of 1/10Ah reported in the response to the basic battery information command `cc aa 00`


## Voltages *(same for type 0, 2 and 3)*
Command: `cc d7 00 00 0c`

### Response (13 bytes)
| First byte | Last byte | Description                                     |
| ---------- | --------- | ----------------------------------------------- |
|         0  |         1 | Pack voltage, as 16 bit little endian integer   |
|         2  |         3 | Cell 1 voltage, as 16 bit little endian integer |
|         4  |         5 | Cell 2 voltage, as 16 bit little endian integer |
|         6  |         7 | Cell 3 voltage, as 16 bit little endian integer |
|         8  |         9 | Cell 4 voltage, as 16 bit little endian integer |
|        10  |        11 | Cell 5 voltage, as 16 bit little endian integer |
|        12  |        12 | Always `06`                                     |

All voltages are given in millivolt.

# Battery type 2

## Identification
Command: `cc dc 0a`

If the battery does not support `cc dc 0b`, but after being put into test mode does support `cc dc 0a`, it is type 2.

The command is supported, if the last byte of the response is 0x06.

### Response (17 bytes)
| First byte | Last byte | Description |
| ---------- | --------- | ----------- |
|         0  |        15 | ?           |
|        16  |        16 | Always `06` |

Note: For batteries that support both `cc dc 0a` and `cc dc 0b`, the response is the identical.

## Enter test mode *(same for type 0, 2 and 3)*
Command: `cc d9 96 a5`

### Response (1 byte)
Always `06`

## Exit test mode *(same for type 0, 2 and 3)*
Command: `cc d9 ff ff`

### Response (1 byte)
Always `06`

## Read model string (unknown if supported)
Command: `cc dc 0c`

### Response (16 bytes)
| First byte | Last byte | Description                                   |
| ---------- | --------- | --------------------------------------------- |
|         0  |        15 | Battery model as null-terminated ASCII string |

## Overdischarge count
Command: `cc d6 8d 05 01`

### Response (2 bytes)
| Byte | Description                           |
| ---- | ------------------------------------- |
|    0 | Integer count of overdischarge events |
|    1 | Always `06`                           |

Overdischarge percentage is calculated the same way as for type0.


## Overload counters
Command: `cc d6 5f 05 07`

### Response (8 bytes)
| Byte | Description     |
| ---- | --------------- |
|    0 | Counter A       |
|    1 | ?               |
|    2 | Counter B       |
|    3 | Counter C       |
|    4 | ?               |
|    5 | Counter D       |
|    6 | Counter E       |
|    7 | Always `06`     |

In the BTC04, the 5 counters are added together. They might count different types of overload events.
Counter D is stored separately though.

Overload percentage is calculated the same way as for type0.


## Health
Command: `cc d6 04 05 02`

### Response (3 bytes)
| First byte | Last byte | Description |
| ---------- | --------- | ----------- |
|         0  |         1 | "Health" as 16 bit little endian integer |
|         2  |         2 | Always `06` |

Health on a scale from 0 to 4 is calculated in the same way as for type0.


## Temperature *(same for type 0, 2 and 3)*
Command: `cc d7 0e 00 02`

### Response (3 bytes)
| First byte | Last byte | Description |
| ---------- | --------- | ----------- |
|         0  |         1 | Temperature in 1/10 K, as little endian integer |
|         2  |         2 | Always `06` |




## Voltages *(same for type 0, 2 and 3)*
Command: `cc d7 00 00 0c`

### Response (13 bytes)
| First byte | Last byte | Description                                     |
| ---------- | --------- | ----------------------------------------------- |
|         0  |         1 | Pack voltage, as 16 bit little endian integer   |
|         2  |         3 | Cell 1 voltage, as 16 bit little endian integer |
|         4  |         5 | Cell 2 voltage, as 16 bit little endian integer |
|         6  |         7 | Cell 3 voltage, as 16 bit little endian integer |
|         8  |         9 | Cell 4 voltage, as 16 bit little endian integer |
|        10  |        11 | Cell 5 voltage, as 16 bit little endian integer |
|        12  |        12 | Always `06`                                     |

All voltages are given in millivolt.

# Battery type 3
If the battery fails the tests for type 0 and 2, but does respond correctly to `cc d4 2c 00 02`, it is type 3.

## Type 3 identifying command (unknown)
Command: `cc d4 2c 00 02`

### Response
| First byte | Last byte | Description |
| ---------- | --------- | ----------- |
|         0  |         1 | ?           |
|         2  |         2 | Always `06` |



## Overdischarge count
Command: `cc d6 09 03 01`
### Response (2 bytes)
| Byte | Description |
| ---- | ----------- |
|    0 | Integer count of overdischarge events |
|    1 | Always `06` |


## Overload counters
Command: `cc d6 5b 03 04`
### Response (6 bytes)
| Byte | Description     |
| ---- | --------------- |
|    0 | Counter A       |
|    1 | ?               |
|    2 | Counter B       |
|    3 | Counter C       |
|    4 | ?               |
|    6 | Always `06`     |

In the BTC04, the 3 counters are added together. They might count different types of overload events.
Counter C is stored separately though.

Overload percentage is calculated the same way as for type0.


## Health
Command: `cc d6 38 02 02`
### Response (3 bytes)
| First byte | Last byte | Description                              |
| ---------- | --------- | ---------------------------------------- |
|         0  |         1 | "Health" as 16 bit little endian integer |
|         2  |         2 | Always `06`                              |

Health on a scale from 0 to 4 is calculated in the same way as for type0.


## Temperature *(same for type 0, 2 and 3)*
Command: `cc d7 0e 00 02`
### Response (3 bytes)
| First byte | Last byte | Description |
| ---------- | --------- | ----------- |
|         0  |         1 | Temperature in 1/10 K, as little endian integer |
|         2  |         2 | Always `06` |


## Voltages *(same for type 0, 2 and 3)*
Command: `cc d7 00 00 0c`
### Response (13 bytes)
| First byte | Last byte | Description                                     |
| ---------- | --------- | ----------------------------------------------- |
|         0  |         1 | Pack voltage, as 16 bit little endian integer   |
|         2  |         3 | Cell 1 voltage, as 16 bit little endian integer |
|         4  |         5 | Cell 2 voltage, as 16 bit little endian integer |
|         6  |         7 | Cell 3 voltage, as 16 bit little endian integer |
|         8  |         9 | Cell 4 voltage, as 16 bit little endian integer |
|        10  |        11 | Cell 5 voltage, as 16 bit little endian integer |
|        12  |        12 | Always `06`                                     |

All voltages are given in millivolt.

# Battery type 5 (F0513 based)
If the value of byte 3 (counting from 0) of ROM id is less than 100, the battery is type 5.

## Read cell voltages
Command: `31` - Voltage for cell 1

Command: `32` - Voltage for cell 2

Command: `33` - Voltage for cell 3

Command: `34` - Voltage for cell 4

Command: `35` - Voltage for cell 5

### Response
| First byte | Last byte | Description                                      |
| ---------- | --------- | ------------------------------------------------ |
|         0  |         1 | Cell voltage in millivolt. Little endian integer |



## Temperature
Command: `cc 52`
### Response (2 bytes)
| First byte | Last byte | Description                                     |
| ---------- | --------- | ----------------------------------------------- |
|         0  |         1 | Temperature in 1/10 K, as little endian integer |

# Battery type 6 (10 cell battery, likely BL36xx)
If byte 17 (counting from 0) of the response to the basic battery information command `cc aa 00` equals 30 (decimal), the battery is type 6.

## Enter state to read out voltages
Command: `cc 10 21`
### Response (0 bytes)
None

## Read cell voltages
Command: `d4`

### Response (20 bytes)
| First byte | Last byte | Description |
| ---------- | --------- | ----------- |
|         0  |         1 | Scaled and offset cell 1 voltage. Little endian integer |
|         2  |         3 | Same for cell 2 |
|         4  |         5 | Same for cell 3 |
|         6  |         7 | Same for cell 4 |
|         8  |         9 | Same for cell 5 |
|        10  |        11 | Same for cell 6 |
|        12  |        13 | Same for cell 7 |
|        14  |        15 | Same for cell 8 |
|        16  |        17 | Same for cell 9 |
|        18  |        19 | Same for cell 10 |

Use the formula 

```python
v = 6000 - x/10
```

to convert to millivolt.

BTC04 uses the minimum cell voltage to calculate battery pack charge percentage.


## Temperature
Command: `d2`
### Response (1 byte)
Temperature as single byte integer.

Convert to degrees celcius with:

```python
t = (-40*x + 9323)/100
```
