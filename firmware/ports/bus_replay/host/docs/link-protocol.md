# Link protocol

This document describes the protocol between `bus-replay link` and its client, the remote
backend in libcapture (`capture/remote.h`).

The link runs as a separate process and owns the device: the reset line, the control
binding, the IRQ line and the data channels. It runs the capture session on the local
backend, parses the stream, and writes the recording when recording is enabled. The
client accesses libcapture's source operations (`capture/source.h`) over a socket. It
sends requests for hello, configuration and stop, and receives a stream of raw source
items. Each item has the same layout as a capture file chunk (`capture-file.md`), so the
client parses the stream in the same way as data read from the device or from a file.

**Protocol version:** 1.0

## Conventions

- Integers are unsigned and little-endian unless stated otherwise.
- Fields follow one another with no padding.
- **Status** is a signed 32-bit integer. It is 0 on success or a negative errno value
  (Linux numbering) on failure.
- **Text** is UTF-8 with no terminator. Its length is given by the enclosing length field.
- **Times** are nanoseconds on the link's monotonic clock. They count from when the link
  receives the accepted reply to the capture register write, so they match the times in a
  capture file recorded by the link.
- A **control message** is stored as the spec defines it: the type byte and the body,
  without the control binding's framing. This is the same as in a capture file.

## Socket

The link listens on a Unix domain stream socket.

The socket path is set with `--socket PATH` on the link and with `remote_options.path` on
the client. If neither is given, both sides use the first of:

1. `$BUS_REPLAY_LINK`
2. `$XDG_RUNTIME_DIR/bus-replay.sock`
3. `/tmp/bus-replay-UID/link.sock`, with the directory created as mode 0700

The link creates the socket with mode 0600 and removes it on exit. At startup, it removes
a stale socket that it cannot connect to. If another link is already listening on the
path, it refuses to start.

The link serves one client at a time. Any further client receives a BUSY message and is
disconnected. The connected client is not affected.

## Messages

Messages in both directions consist of a 16-byte header followed by a payload.

| Offset | Size | Field | Contents |
|---|---|---|---|
| 0 | 1 | Type | see below |
| 1 | 1 | Flags | type-specific; 0 if unused |
| 2 | 2 | Channel | type-specific; 0 if unused |
| 4 | 4 | Length | payload length, at most 64 MiB |
| 8 | 8 | Time | type-specific; 0 if unused |
| 16 | Length | Payload | type-specific |

The header is identical to a capture file chunk header. An item message is a chunk
without its CRC, because the socket does not corrupt data.

A receiver closes the connection if a message is malformed: for example, if its length
exceeds the limit or a known type has a payload that is too short. Messages of an unknown
type are skipped.

| Type | Message | Direction |
|---|---|---|
| `0x01` | DATA | link to client (item) |
| `0x02` | CONTROL | link to client (item) |
| `0x03` | EVENT | link to client (item) |
| `0x10` | HELLO | client to link (request) |
| `0x11` | CONFIG | client to link (request) |
| `0x12` | STOP | client to link (request) |
| `0x13` | ATTACH | client to link (request) |
| `0x14` | RECORD | client to link (request) |
| `0x15` | STATUS | client to link (request) |
| `0x20` | WELCOME | link to client, first message on a connection |
| `0x21` | BUSY | link to client, followed by disconnect |
| `0x90`–`0x95` | Replies | link to client; the request type with bit 7 set |

## Connection setup

The link sends WELCOME immediately after accepting a connection.

| Offset | Size | Field | Contents |
|---|---|---|---|
| 0 | 2 | Major version | 1 |
| 2 | 2 | Minor version | 0 |
| 4 | 1 | State | bit 0: a capture is running; bit 1: it is being recorded |
| 5 | 3 | Reserved | 0 |
| 8 | remainder | Link | text: the link's name and version |

A client must disconnect if it does not support the major version. A new minor version
may only add message types, fields at the end of request payloads, and state bits. An
older peer skips or ignores these.

The BUSY payload is a text reason, such as `another client is connected`.

## Requests and replies

The client sends one request at a time and waits for the reply before sending the next.
Replies arrive in request order. Items may arrive before a reply, and the client queues
them while it waits.

Every reply payload begins with a status, followed by the data the request returns.

| Request | Payload | Reply data after the status |
|---|---|---|
| HELLO | none | the device's hello message, unmodified |
| CONFIG | one configuration message | the device's configuration reply, unmodified |
| STOP | none | none |
| ATTACH | none | an attach record (see below) |
| RECORD | text: path for the next capture's recording; empty for none | none |
| STATUS | none | a status record (see below) |

### HELLO

Implements the source `hello` operation. The link resets the device and replies with the
hello message that the device sends after the reset. If a capture is running, the link
ends it first, in the same way as STOP.

### CONFIG

Implements the source `config` operation. The link sends the configuration message to the
device and replies with the device's reply.

The status is an error in these cases:

- `-ETIMEDOUT` or another error: the transaction with the device failed, for example on a
  timeout or a failed check.
- `-EBUSY`: a capture is already running.
- `-ENODATA`: no HELLO has been sent yet.

If the device rejects the message, the status is 0 and the device's reply carries the
rejection.

If the message is a capture register write that starts a capture, and the device accepts
it, the capture starts:

- The link's session takes the hello and all configuration messages and replies since the
  last HELLO as the capture's setup (`capture_attach()` with a first grid count of 0).
  Recording starts if a path has been set (see RECORD).
- The item stream to the client starts directly after the reply, beginning with the
  capture's first bytes. Times count from when the link received the device's reply.

### STOP

Implements the source `stop` operation. The link ends the capture by resetting the device
and reads the hello message that follows, which releases the IRQ line. It closes the
recording with reason 0 (stopped by the user). No more items are sent until the next
capture starts.

If no capture is running, STOP replies with status 0.

### ATTACH

Joins a running capture at its current position. If no capture is running, the status is
`-ENOENT`.

The link replies immediately with the grid count that follows the newest heartbeat it has
received on any data channel. Every data channel's stream reaches that grid count within
one heartbeat interval (C ticks).

After the reply, the link sends DATA for each data channel, starting at the first byte of
the heartbeat for that grid count. If a data channel misses that heartbeat, its DATA
starts at the next one. Control messages and events are sent once DATA has started on
every data channel. Nothing from before that point is sent, so the client only sees the
capture from when it attached.

The client parses the stream like a capture file that starts mid-capture
(`capture-file.md`, Header). It accepts the first heartbeat without checking its check
byte.

Attach record:

| Offset | Size | Field | Contents |
|---|---|---|---|
| 0 | 8 | First grid count | the grid count at which every data channel's stream begins |
| 8 | 8 | Time | the link's time when it sent the reply (see Conventions); the client's item times continue from this value |
| 16 | 4 | Hello length | H |
| 20 | H | Hello | the hello message the capture was started after |
| 20 + H | 4 | Message count | M |
| … | variable | Messages | M message entries (see below) |
| … | 4 | Loss base length | L; 0 if there is none |
| … | L | Loss base | the last loss report before the first grid count |

Each message entry consists of:

| Size | Field | Contents |
|---|---|---|
| 2 | Type | `0x0002` configuration message or `0x0003` configuration reply, as in the capture file header |
| 4 | Length | length of the message |
| Length | Message | the message |

The entries are the capture's configuration messages and replies in the order they
occurred. Each reply follows its message.

### RECORD

Sets the path of the file the link records the next capture to, or disables recording if
the path is empty. Until a client sends RECORD, the link uses the path given with
`--record`, if any.

The path is opened when the capture starts. If the link cannot create the file, the
capture runs without a recording. The error is reported in STATUS and on the link's
standard error.

While a capture is running, RECORD fails with `-EBUSY`, because a recording always starts
together with its capture.

### STATUS

Status record:

| Offset | Size | Field | Contents |
|---|---|---|---|
| 0 | 1 | State | as in WELCOME |
| 1 | 3 | Reserved | 0 |
| 4 | 4 | Record error | status of the recording; 0 if there is no error |
| 8 | 8 | Recorded | bytes written to the recording |
| 16 | 8 | Dropped bytes | DATA bytes dropped for this client (see Slow clients) |
| 24 | 4 | Dropped control | control messages dropped for this client |
| 28 | 4 | Path length | P |
| 32 | P | Path | path of the recording; empty if there is none |

## Items

DATA, CONTROL and EVENT messages carry the local backend's source items. Their fields
match the chunk fields in `capture-file.md`:

- **DATA:** the channel field is the data channel and the payload is the bytes as read.
- **CONTROL:** the flags give the direction.
- **EVENT:** the payload is the event code followed by its data.

Items are only sent while a capture is running, either from its start or from an ATTACH.
They are sent in the order the link's session received them. The link's own parsing and
recording never wait for the client.

## Slow clients

The link queues up to 32 MiB of items for the client (configurable with `--queue`). If the
client falls further behind than the queue and the socket buffers can hold:

- The link drops DATA bytes, per data channel, and drops control messages.
- Once the queue has room again, the link sends an Overflow event (`0x0004`) on each data
  channel that lost bytes, before any more DATA on that channel. The event gives the
  number of bytes and control messages dropped. If only control messages were dropped,
  the link sends an Overflow event on channel 0 with a byte count of 0, before the next
  control message or event.
- The client handles an Overflow event as described in `capture-file.md`: as a transport
  loss from the last good heartbeat to the next one confirmed good, and dropped control
  messages as a sequence skip in the loss reports.
- STATUS reports the number of dropped bytes and control messages.

The link's own parsing and recording are not affected and lose no data.

## Disconnection

- **Client disconnects:** the capture and its recording continue. The link discards the
  client's queue and accepts the next client. That client can join the capture with
  ATTACH, or end it with HELLO.
- **Link exits** (on SIGINT or SIGTERM): the link ends a running capture as STOP does,
  closes the recording with reason 2 (signal), and removes the socket. The client sees
  end of stream, and its reads fail with `-ECONNRESET`.
- **Device error during a capture** (a backend error): the link ends the capture and
  closes the recording with reason 3 (error). The client's item stream stops.

## Running the link

```
bus-replay link [--socket PATH] [--dummy] [--record FILE] [--queue BYTES]
```

| Option | Description |
|---|---|
| `--socket PATH` | socket path (see Socket) |
| `--dummy` | use the dummy backend instead of the device, for development and testing without hardware |
| `--record FILE` | record to FILE, as if a RECORD had been sent before the first capture |
| `--queue BYTES` | size of the client queue (see Slow clients); intended for tests |

The link starts with no capture running. It only resets the device when a client sends
HELLO.
