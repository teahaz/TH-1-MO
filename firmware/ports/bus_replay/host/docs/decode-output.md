# Decoded output

What `bus-replay decode` writes. Every format is generated from
the item tree (`codec/item.h`), the same way for every protocol: a new decoder
gets all of them without code of its own.

| Format | For | Content |
|---|---|---|
| `text` (default) | people | per item a header line, its words, its children |
| `jsonl` | tools | one top-level item per line, with its whole tree; nothing left out |
| a decoder's export | files, pipes | the payload alone, as the decoder defines it (`raw` for UART) |

There is no decoded CSV: a tree does not flatten into one table. `export --csv` gives the
raw records.

## Common rules

- Items come out in order of their start tick, across all the decoders of a run.
- Times are seconds from the start of the capture (tick 0), from the capture's sample
  rate. Ticks are the capture's own ticks.
- An item is written once it can no longer change: no decoder can still emit one that
  starts earlier, and a loss report arriving late (up to one second of capture after the
  item's end) can no longer mark it. `--follow` output lags the capture by that second.
- `--from` and `--to` (seconds) keep the items that overlap the span. Parsing starts at the
  index entry at or before `--from`; the first items after it may be missing or cut.
- `--level N` writes children up to detail level N. Text writes level 0 by default, JSON
  lines every level.

## Text

```
  0.937966  i2c   0x44  W+R  8    ACK
             > 88 E0 00
             > 89
             < 60 D0 D0 A3 84 ED
  0.906282  uart  TX  397 bytes
             > "[02:05:36.360,290] \x1B[0m<inf> BUS_REPLAY_TARGET: Temperature 21.18 C\r\n"
               "[02:05:36.360,839] \x1B[0m<inf> BUS_REPLAY_TARGET: UART test 15070\r\n"
```

**Header line.** The start time (`%10.6f`), the decoder's label, the item's header fields
in the kind's order, each at least its column width, then its flags in brackets:

```
 12.345678  i2c   0x44  W    2    NAK  [loss, nak_data]
```

A field that is not set is left out, with its column.

**Words**, below the header, indented to column 14:

- Lanes taking turns (I2C, UART): a line per group of words, a group being the words of
  one lane until the lane changes or a word starts a new group (an I2C address byte).
  The first lane is marked `>`, the second `<`; a kind with more lanes uses their names.
- Lanes side by side (SPI, I2S): a line per lane, named, the lane's words in order.
- Words are hex, as many digits as their bits need, 16 to a line.
- Kinds read as text (UART) show words as a quoted string instead: printable ASCII as it
  is, `\n`, `\r`, `\t`, `\"`, `\\`, everything else `\xNN`. A line feed ends the line and
  the string goes on below; so does a line past 120 characters. `--hex` shows them as hex.

**Children** up to the level, indented two more: `=` before a child from another
decoder (a chip-level reading of the item), `-` before one from the same decoder (an I2C
segment), then its header fields and flags, then its words.

### Pretty

`--pretty` (`-p`) is a text layout with one line per reading, the most interpreted one
there is, for watching rather than checking:

```
  2.978530  uart   > "[02:05:38.432,525] \x1B[0m<inf> BUS_REPLAY_TARGET: Temperature 21.18 C, humidity 63.86 %RH\x1B[0m\r\n"
                     "[02:05:38.464,172] \x1B[0m<inf> BUS_REPLAY_TARGET: UPS 4.040 V, 0.006 A, 0.040 W, battery 87 %\x1B[0m\r\n"
  3.010206  SHT3x  read     fetch data   21.17 C  63.82 %RH
  3.014644  i2c    0x43  W+R  3    ACK
  3.017032  i2c    0x43  W    3    ACK
```

(`--i2c scl=0,sda=1 --uart tx=3,baud=115200 --chip sht3x@0x44`: the INA219 at 0x43 has no
chip bound, so its transactions show as headers.)

- An item another decoder read (an I2C transaction a chip decoded): that decoder's
  readings instead of the item, a line each, at the reading's own time, with its header
  fields and flags. The first field (a chip's name) takes the label column, which is wide
  enough for the labels of the decoders stacked on others too.
- An item nothing read: its header line, without words. A transaction to an address no
  chip is bound to stays visible this way.
- A kind read as text (UART): its words, after the label, with the lane mark and in the
  layout above (`--hex` as hex), without the header line. Its flags, if any, come before
  the lane mark.
- Children of the same decoder (I2C segments) are not shown, so there is no `--level`.
  `--pretty` is text only: `-f` with anything else, or `--level`, is refused.

The full text and JSON lines stay as they are; pretty only leaves out what is already
interpreted.

## JSON lines

One top-level item per line:

```json
{"decoder":"i2c","kind":"transaction","id":1,"start":468983,"end":471135,
 "start_s":0.937966,"end_s":0.94227,"fields":{"addr":68,"dir":"W+R","len":8,"status":"ACK"},
 "words":[{"lane":"master","value":136,"bits":8,"start":469037,"end":469208}, ...,
          {"lane":"target","value":237,"bits":8,"start":470909,"end":471081,"flags":["nak"]}],
 "children":[{"decoder":"i2c","kind":"segment","id":2,"start":468983,"end":469696,
              "start_s":0.937966,"end_s":0.939392,"fields":{"addr":68,"dir":"W","len":2}}, ...]}
```

| Key | Content |
|---|---|
| `decoder` | the decoder's label (`i2c`, or `name=` given, `i2c#2` for a second one) |
| `kind` | the item's kind |
| `id` | unique within the run, in order of emission; a child's is after its parent's |
| `start`, `end` | ticks, inclusive |
| `start_s`, `end_s` | seconds |
| `flags` | the item's flags by name; left out when none |
| `fields` | every field that is set, by name: numbers as numbers, text as strings |
| `words` | each word: lane by name, value, bits, start and end ticks, its flags by name (left out when none); left out when the item has no words |
| `children` | the children, as items; left out when none |

The span of every item and word is there, so `export --from --to` can always fetch the raw
records behind it.

## Flags

Every kind has these item flags:

| Flag | Meaning |
|---|---|
| `incomplete` | the item was cut off: the capture ended before it did |
| `loss` | records it depends on may be missing: a reported loss overlaps it, or a heartbeat corrected a level the records got wrong |

Word flags common to every kind: `error` (the word did not read cleanly: a UART framing
error).

## Decoders

### i2c (`scl=`, `sda=`)

A transaction from START to STOP, a segment per START (child, level 1).

| Field | Content |
|---|---|
| `addr` | the first segment's 7-bit address |
| `dir` | each segment's direction, joined: `W`, `R`, `W+R` |
| `len` | data bytes, all segments |
| `status` | `ACK`; `NAK addr` (an address not acknowledged); `NAK` (a data byte not acknowledged that should have been; the master's NAK on the last byte it reads is not one); `ERR` (a START or STOP inside a byte); `empty` (no complete byte) |

Flags: `nak_addr`, `nak_data`, `cut_byte`. Words: every byte, the address bytes and written
data on lane `master`, read data on `target`; word flag `nak`. Segment fields: `addr`,
`dir`, `len`.

Edges in the same tick are taken in the order valid I2C implies: SCL falls, SDA changes,
SCL rises.

### uart (`tx=`, `rx=`, `baud=`, `bits=`, `gap=`)

A burst: the bytes of one direction until the other direction sends, or the line is idle
for more than `gap` byte times (default 10). No parity, one stop bit; each bit read at the
middle of its bit time.

| Field | Content |
|---|---|
| `dir` | `TX` or `RX` |
| `bytes` | bytes in the burst |
| `errors` | framing errors; left out when none |

Words: the bytes, lane `tx` or `rx`, flag `error` on a framing error. Export `raw`: the
bytes alone, in order.

### activity (`lines=`, `gap=`, `max=`)

A packet: records on any of its lines with no idle gap longer than `gap` seconds (default
300 us), cut at `max` seconds if given. Fields: `records`, `lines` (the channels that
changed, joined by `+`), `length` (ms). No words.

## Chip-level decoders

Given as `--chip NAME@ADDR[,setting=value...]`, after an i2c decoder. Each reads the I2C
traffic to its address and adds a reading as a child of the transaction it reads: an `=`
line under it in the text, a child in JSON lines, at level 0. The transaction and its
bytes stay as they are. Every chip also takes `on=LABEL`, the i2c decoder it reads
(default the last one given), and refuses an address the chip cannot have.

```
  3.010206  i2c  0x44  W+R  8    ACK
             > 88 E0 00
             > 89
             < 60 CA 48 A3 63 BB
               = SHT3x   read     fetch data   21.17 C  63.82 %RH
  3.036602  i2c  0x43  W+R  3    ACK
             > 86 02
             > 87
             < 1F 9A
               = INA219  read     bus          4.044 V
```

A pointer write or command and the read after it in the same transaction are one reading,
spanning both. A segment whose address byte was not acknowledged gives no reading.

Fields every chip has:

| Field | Content |
|---|---|
| `chip` | the chip's label (`INA219`, `SHT3x`) |
| `op` | `read`, `write`, `pointer` (a pointer write with no read after it), `command`, `empty` (an address byte alone) |
| `reg` | the register's or command's name; its number in hex when it has none; left out when not known (a read with no pointer or command before it) |
| `raw` | the register's value, or the command (not on the text line) |
| `data` | the bytes, in hex, when nothing else shows them: an unknown register's value, a short or long transfer |

Flags every chip has: `short`, `long` (fewer or more bytes than the register or command
needs), `unknown` (a register or command not in the chip's table, or no pointer or command
known: none seen yet, or a loss since), `crc` (a data word's CRC is wrong; the values are
still shown). A reading also has the `loss` and `incomplete` flags of its transaction.

### ina219 (`shunt=`, `cal=`)

Registers `config`, `shunt`, `bus`, `power`, `current`, `calibration`; 16-bit, big-endian.

| Field | Register | Content |
|---|---|---|
| `reset`, `range` (V), `pga`, `bus_adc`, `shunt_adc`, `mode` | config | the configuration, as text (`32 V  /8 320 mV  bus 12 bit  shunt 32 samples  shunt+bus triggered`); `reset` only in JSON |
| `shunt` (mV) | shunt | signed, 10 uV a bit |
| `bus` (V), `cnvr`, `ovf` | bus | 4 mV a bit; the flags only in JSON, and `ovf` also as a flag |
| `current` (A), `power` (W), `current_lsb` (A) | current, power | once the current LSB is known: 0.04096 / (calibration * shunt), the calibration from the capture, else `cal=`; power is 20 LSB a bit |
| `count` (lsb) | current, power | the raw register, while the current LSB is not known; flag `uncalibrated` |
| `calibration` | calibration | the value, kept for current and power from then on |

### sht3x

Commands by name: `single shot high stretch` and the other single shot ones, `periodic 1
mps high` and the other periodic ones, `fetch data`, `art`, `break`, `soft reset`, `heater
on`, `heater off`, `status`, `clear status`. A read is read for the last command sent.

| Field | Content |
|---|---|
| `temp` (C), `rh` (%RH) | a measurement (single shot or fetch data): -45 + 175 * raw / 65535, and 100 * raw / 65535 |
| `temp_raw`, `rh_raw` | the words (JSON only) |
| `status` | the status register's set bits by name (`alert`, `heater`, `rh alert`, `t alert`, `reset seen`, `command failed`, `write crc failed`), or `clear` |
