# Capture file format

A capture file (`.thcapture`) holds one recording of a bus replay capture: the device's
data stream bytes exactly as the host read them, the control messages that set the
capture up and that arrived during it, and host events. Everything else (records, levels,
statistics, decoded traffic) is derived by parsing it with the protocol's stream rules
(`../../protocol/spec.tex`).

**Format version:** 1.0

## Conventions

- Integers are unsigned and little-endian unless stated otherwise.
- Sizes are in bytes. Fields follow one another with no padding and no alignment.
- **Times** are nanoseconds on the host's monotonic clock, counted from the recording's
  start (header, start time).
- **Ticks** are on the capture timeline of the spec: tick = grid count x C + offset.
- **Text** is UTF-8 with no terminator. Its length comes from the enclosing length field.
- **CRC-32** is CRC-32/ISO-HDLC, as zlib's `crc32()`: polynomial `0x04C11DB7` reflected,
  initial value `0xFFFFFFFF`, final XOR `0xFFFFFFFF`. The check value of the ASCII
  string `123456789` is `0xCBF43926`.
- A **control message** is stored as the spec defines it: the type byte and the body,
  without the control binding's framing (for SPI: no length and no CRC-16).

## Layout

| Part | Size |
|---|---|
| Header | header length |
| Chunks | to the trailer |
| Trailer (END chunk) | per its length |
| Footer | 16 |

The chunks, the trailer and the footer are written as the recording goes on. The trailer
and the footer are written last, when the recording ends.

## Header

### Fixed part

| Offset | Size | Field | Contents |
|---|---|---|---|
| 0 | 8 | Magic | `8E 54 48 43 0D 0A 1A 0A` |
| 8 | 2 | Format major | 1 |
| 10 | 2 | Format minor | 0 |
| 12 | 4 | Header length | bytes from the start of the file to the first chunk, including the header CRC |
| 16 | 1 | Spec major | the spec version major in the hello |
| 17 | 1 | Spec minor | the spec version minor in the hello |
| 18 | 2 | Flags | 0 |
| 20 | 4 | Reserved | 0 |
| 24 | 8 | Start time | the recording's start, as Unix time in nanoseconds (host wall clock) |
| 32 | 8 | First grid count | 0 for a recording from the start of the capture; otherwise the grid count where the recording starts (below) |
| 40 | per item | Header items | until 4 bytes before the header length |
| header length - 4 | 4 | Header CRC | CRC-32 over every header byte before this field |

The start time and the chunk times use the same instant as their zero: for a recording
from the start of the capture, the moment the host received the accepted reply to the
capture register write; for a recording that starts later, the moment recording began.

**A recording that starts later.** When the first grid count is not 0, the recording
was attached to a capture already running. The DATA on every data channel then begins
with the first byte of the heartbeat of that grid count. That heartbeat is taken as
good without its check byte, whose CRC-8 covers bytes before the recording, and the
check byte's CRC-8 starts again after it.

### Header items

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Item type |
| 2 | 4 | Length of the value |
| 6 | length | Value |

Items appear in the order they happened. A reader skips an item type it does not know.

| Type | Item | Value | Count |
|---|---|---|---|
| `0x0001` | Hello | the hello message as received | exactly 1 |
| `0x0002` | Configuration | a configuration message as sent | 0 or more |
| `0x0003` | Configuration reply | a configuration reply as received | 0 or more |
| `0x0004` | Capture info | below | exactly 1 |
| `0x0005` | Binding | text naming the bindings, such as `control=spi data0=serial:/dev/ttyAMA4:1000000` | 0 or 1 |
| `0x0006` | Host | text naming the host software and its version | 0 or 1 |
| `0x0007` | Note | text from the user | 0 or more |
| `0x0008` | Loss base | the last loss report received before the recording started | 0 or 1, only when the first grid count is not 0 |

Configuration messages and their replies alternate, each reply after its message,
including the capture register write that started the capture. A recording that starts
later carries the ones the host kept from the capture's start.

The loss base gives the lost-records counters and the sequence number at the
recording's start, so the loss reports in the recording are read against it as the spec
describes (Loss, Host behaviour). Without one, counters start from 0 and the sequence
from 0.

### Capture info

The capture as it runs, taken from the accepted configuration. The configuration
messages and replies are the record of what happened. A reader that parses them gets the
same values.

| Offset | Size | Field | Contents |
|---|---|---|---|
| 0 | 4 | Sample rate | Hz |
| 4 | 4 | C | ticks |
| 8 | 2 | Channel count | N, from the hello |
| 10 | 1 | Data channel count | D, from the hello |
| 11 | 1 | Reserved | 0 |
| 12 | ceil(N/8) | Enabled channels | channel set, as in the spec |
| 12 + ceil(N/8) | ceil(N/8) | Split | channel set, as in the spec |

The mapping of logical channels to data channels, W and the tag width follow from these
values by the spec's rules (Mapping, Record).

## Chunks

| Offset | Size | Field | Contents |
|---|---|---|---|
| 0 | 1 | Type | below |
| 1 | 1 | Flags | per type; 0 where the type defines none |
| 2 | 2 | Channel | the data channel number for DATA and for data channel events; 0 otherwise |
| 4 | 4 | Length | payload length |
| 8 | 8 | Time | per type, below |
| 16 | length | Payload | per type |
| 16 + length | 4 | CRC | CRC-32 over the chunk from its type to the end of its payload |

Chunks are in the order the host wrote them. Their times do not decrease. A reader skips
a chunk type it does not know.

| Type | Chunk | Time |
|---|---|---|
| `0x01` | DATA | when the host read the payload's last byte |
| `0x02` | CONTROL | when the message was received or sent |
| `0x03` | EVENT | when the event happened |
| `0x7F` | END | when the recording ended |

### DATA

- **Payload:** data channel bytes exactly as read, device to host. Bytes are never
  changed, removed or added, including bytes that turn out to be corrupt.
- **Channel:** the data channel number.
- **Flags:** 0.
- A data channel's stream is the payloads of its DATA chunks joined in file order.
  Chunk boundaries carry no meaning: a record or a heartbeat may be split across two
  chunks.

### CONTROL

- **Payload:** one control message.
- **Flags:** bit 0: 0 for device to host, 1 for host to device. Other bits are 0.
- Loss reports arrive as CONTROL chunks. So do any other messages received during the
  recording, including ones of a type the host does not know.

### EVENT

- **Payload:** an event code (2 bytes), then the event's data.
- **Channel:** the data channel the event concerns, for codes that name one; 0
  otherwise.
- A reader skips an event code it does not know.

| Code | Event | Data |
|---|---|---|
| `0x0001` | Stop | none. The host reset the device to end the capture. |
| `0x0002` | Data error | errno as a signed 4-byte integer, then text. Reading the data channel failed. |
| `0x0003` | Data silent | duration in milliseconds (4 bytes). No byte arrived on the data channel for that long. |
| `0x0004` | Overflow | bytes dropped (8), control messages dropped (4). The host fell behind (reading the data channel, or writing the file) and dropped data for this data channel. When only control messages were dropped, the bytes dropped are 0 and the channel is 0. |
| `0x0005` | Control error | text. A control transaction failed (for SPI: a failed check, a timeout). |
| `0x0006` | Note | text. |

**Overflow.** When its bytes dropped are not 0, the DATA before and after an overflow
event are not contiguous: the stream continues at an arbitrary byte. A reader handles it
as a transport loss detected at that point (spec, Transport loss): the logical channels on that data
channel are untrusted from the last good heartbeat check byte before it until the next
heartbeat confirmed good after it. Dropped control messages show as a sequence skip in
the loss reports.

### END (trailer)

Written once, as the last chunk. Its values are the host's counts when the recording
ended, from parsing the traffic it recorded. Parsing the file gives the same values,
except after an overflow event: the host parsed the bytes it then dropped from the
file.

| Offset | Size | Field | Contents |
|---|---|---|---|
| 0 | 8 | Last tick | the latest tick the streams reached |
| 8 | 1 | Reason | why the recording ended, below |
| 9 | 1 | Data channel count | D, the number of data channel entries |
| 10 | 2 | Logical channel count | n, the number of logical channel entries |
| 12 | 4 | Reserved | 0 |
| 16 | 8 | Loss reports | loss reports received |
| 24 | 8 | Mismatches | heartbeats whose levels disagreed with the records, not explained by a loss |
| 32 | 8 | Excused | mismatches inside a reported loss |
| 40 | 8 | Overflow bytes | DATA bytes dropped by the host, all data channels |
| 48 | D x 48 | Data channels | one entry per data channel, in number order |
| 48 + D x 48 | n x 16 | Logical channels | one entry per enabled channel, in ascending channel number |

Data channel entry:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | Bytes |
| 8 | 8 | Records |
| 16 | 8 | Heartbeats |
| 24 | 8 | CRC errors |
| 32 | 8 | Gap errors |
| 40 | 8 | Resyncs |

Logical channel entry:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | Records |
| 8 | 8 | Lost records, by the loss reports, since the recording's start |

| Reason | Meaning |
|---|---|
| 0 | stopped by the user |
| 1 | the requested duration was reached |
| 2 | the host was signalled to quit |
| 3 | a data or control error |
| 4 | recording was turned off; the capture went on |

A reader takes the counts of the arrays from the entry counts, not from the header, and
ignores bytes after the logical channel entries.

## Footer

| Offset | Size | Field | Contents |
|---|---|---|---|
| 0 | 8 | END offset | file offset of the END chunk |
| 8 | 8 | End magic | `8E 54 48 43 45 4E 44 0A` |

## Reading

- **Complete file:** the last 16 bytes are a footer whose END offset points at an END
  chunk with a valid CRC.
- **Incomplete file:** anything else, such as a recording still running or one cut short
  by a crash or a power loss. A reader reads chunks from the start until one runs past
  the end of the file or fails its CRC. Every chunk before it is valid. Statistics come
  from parsing.
- A writer creates a new file: it never appends to an existing one or overwrites it.
- **Versions:** a reader refuses a format major it does not know. A minor version adds
  only header items, chunk types, event codes, or fields at the end of a payload, all of
  which an older reader skips or ignores. A reader refuses a spec major it does not know,
  as the spec requires of the host.

## Index file

A separate file for seeking. It is derived: a reader that finds it missing, unreadable
or not matching rebuilds it by parsing the capture file.

- **Name:** the capture file's name with `.index` appended: `bench.thcapture.index`.
- Written alongside the capture file as the recording goes on.

### Header

| Offset | Size | Field | Contents |
|---|---|---|---|
| 0 | 8 | Magic | `8E 54 48 49 0D 0A 1A 0A` |
| 8 | 2 | Format major | 1 |
| 10 | 2 | Format minor | 0 |
| 12 | 4 | Header CRC | the capture file's header CRC |
| 16 | 8 | Start time | the capture file's start time |
| 24 | 4 | Interval | K: grid points between entries |
| 28 | 4 | Reserved | 0 |

The header CRC and the start time tie the index to its capture file. An index whose
values differ from the capture file's does not belong to it.

The writer picks K. The default is the number of grid points in about one second,
sample rate / C rounded, at least 1.

### Entries

24 bytes each, after the header, in file offset order.

| Offset | Size | Field | Contents |
|---|---|---|---|
| 0 | 8 | Grid count | the heartbeat's grid count, unwrapped from the start of the capture |
| 8 | 8 | Chunk offset | file offset of the DATA chunk holding the heartbeat's first byte |
| 16 | 4 | Payload offset | offset of that byte in the chunk's payload |
| 20 | 2 | Data channel | the data channel |
| 22 | 2 | Reserved | 0 |

- One entry per data channel for the first heartbeat confirmed good at or after every
  grid count that is a multiple of K.
- Parsing can start at an entry: the heartbeat there gives every level of its data
  channel's logical channels, resets the gap reference, and is taken as good without
  checking its check byte. The check byte's CRC-8 starts again after it.
- The entry's tick is grid count x C.
- An incomplete last entry is ignored. Entries past the capture file's readable part are
  ignored.
