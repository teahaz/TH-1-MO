# Lua API

The UI script's view of the session (`ui/session/session.h`): one module, `bus_replay`,
with session control, status, store queries and decoded items. Every call is one big step
(the columns of every lane of a frame, the items of a span), never one call per record.

## Registration

- `ui/cel/lua_session.c` defines `luaopen_bus_replay`.
- The binary links with `-Wl,--wrap=luaL_openlibs`. `__wrap_luaL_openlibs` calls
  `__real_luaL_openlibs`, then sets `package.preload.bus_replay`, which the router's
  `require` finds. A page's `require` is CEL's own and only finds modules as files on
  `package.path`; it takes `package` and `loadfile` after `luaL_openlibs`, so the wrap
  also answers `package.searchpath` and `loadfile` for this one name, with the module.
  CEL itself is unchanged, and every Lua state it makes (router and page) can `require`
  the module, each getting the same session.
- The script starts with `local br = require("bus_replay")` and stops with a clear error
  if it is missing.
- There is one session per process. `ui_main` creates it before CEL runs, with the
  command line's options, and frees it after. Every Lua state reaches the same one.
- No call blocks on the capture. Commands queue and return at once; queries take the
  store's or the items' lock for the duration of the call only.

## Conventions

- Ticks, ids and counts are Lua integers (64-bit). Times in seconds are floats.
- Channels are the device's channel numbers.
- Levels: `0`, `1`, or `-1` for unknown.
- A query that fails returns `nil, message`. A command returns its id, or `nil, message`
  when it could not be queued; how it went shows in `status()`.
- A query reads the current view. A result carries the view's `gen`: when `gen` changes,
  the capture is a new one and positions, ticks and item ids from before mean nothing.

## Session control

| Call | Does |
|---|---|
| `br.open{backend=, path=, realtime=}` | backend: `"local"`, `"dummy"`, `"file"`, `"remote"`. path: the file, or the link's socket (optional). realtime: the dummy's (default true). Joins a capture a link already runs. |
| `br.hello()` | asks the backend for its hello again (resets the device) |
| `br.start{rate=, c=, channels={0, 1, 3}}` | starts a capture; fields left out keep the device's values |
| `br.stop()` | ends the capture; its data stays to look at |
| `br.record(path)` | the file the next capture is recorded to; `nil` for none |
| `br.close()` | the backend goes |
| `br.decoder_add(name, args, on, hold)` | a protocol decoder as the decode mode takes it (`"i2c", "scl=0,sda=1,name=bus"`), or with `on` a group's label, one stacked on it (`"ina219", "addr=0x43", "bus"`). `hold`: a new group's, in seconds (default 1): it decodes that far behind the capture, so late loss reports still reach it, and its items come that much after their end |
| `br.decoder_remove(label)` | a group, with what is stacked on it |

## Status

`br.status()` returns:

```lua
{
    state = "closed" | "ready" | "running" | "ended",
    backend = "none" | "local" | "dummy" | "file" | "remote",
    busy = false,                -- a command is running or queued
    done = 7,                    -- the last command finished
    ok = true, error = nil,      -- how it went: error is the reason text when not ok
    gen = 3,
    capture = {
        running = true, now = 123456,   -- the newest heartbeat's tick
        records = 0, heartbeats = 0, bytes = 0, crc = 0, gaps = 0, resyncs = 0,
        mismatches = 0, excused = 0, losses = 0, overflow = 0,
    },
    store = {                    -- absent before a capture
        oldest = 0, watermark = 0, newest = 0, records = 0, total = 0, dropped = 0,
        bytes = 0, limit = 268435456,  -- watermark -1 before every data channel's first heartbeat
    },
    events = 12,                 -- events so far
    decoded = 123000,            -- every group has decoded up to here
    catching_up = false,
}
```

`br.events(after)` returns the events after `after` (an event `seq`), oldest first, at
most 64 at a time: `{ {seq=, time=, code=, channel=, text=}, ... }`, time in seconds from
the capture's start, code a capture file event code (0 for the session's own).

`br.device()` returns the view's hello, or `nil` before one:

```lua
{
    model = "TH-1", firmware = "0.3.0", version = "1.0", uid = "a1b2...",
    buffer = 65536, channels = 6, data_channels = 1,
    names = { [0] = "CH1", "CH2", ... },       -- by channel number
    rate = 500000, c = 1000, enabled = {0, 1, 2, 3},   -- the registers as the hello has them
    rates = { 500000, 250000, ... },          -- the sample rates its divider allows
    c_min = 1, c_max = 134217727,
}
```

`br.clock()` returns seconds on a monotonic clock, as a float: the page's sandbox has no
time finer than `os.time`, and the UI times its hold and its own frames with it.

`br.capture()` returns the view's capture info, or `nil` before a capture:
`{ gen=, rate=, c=, channels = {0, 1, 2, 3}, names = { [0] = "CH1", ... } }`.

## Store queries

| Call | Returns |
|---|---|
| `br.columns(channels, edges)` | the lanes of a frame (below) |
| `br.span(channels, from, to, cols)` | the same with `cols` equal columns from `from` to `to` |
| `br.level(channel, tick)` | `0`, `1` or `-1`; `nil, message` for a tick not held (older than the store, or past what has arrived) |
| `br.next(channels, tick)` | the first record after `tick` on any of `channels`, up to the watermark: `tick, channel, level`, or `nil` |
| `br.prev(channels, tick)` | the last record before `tick`: `tick, channel, level`, or `nil` |
| `br.records(channel, from, to, max)` | `{ticks = {...}, levels = {...}}, more` |
| `br.losses(from, to, max)` | `{ {start=, stop=, lost=, channels = { [ch] = lost, ... }}, ... }`, oldest first |

`br.columns` takes a list of channels and a list of `cols + 1` ascending ticks (the
column edges; a view may give idle stretches fewer columns). It returns one table per
channel, in the order given, of parallel lists of `cols` entries each:

```lua
{
    gen = 3, watermark = 123000,
    [1] = { first = {...}, last = {...}, changes = {...}, flags = {...} },
    ...
}
```

- `first[x]`: the level at the end of the tick before the column; `last[x]`: at its end.
- `changes[x]`: the records in the column.
- `flags[x]`: a bit set: 1 loss, 2 unknown somewhere in the column, 4 not held (older
  than the store holds, or past what has arrived).

A frame draws every lane from one `br.columns` call, with edges ending at most at the
watermark from the same frame's `status()`.

## Decoded items

The protocols on offer are listed by `br.protocols()`: the decoders that read lines (not
stacked ones), each `{name=, summary=, params = { {name=, help=, type=, required=,
default=}, ... }}`, `type` being `"line"` (a channel), `"uint"`, `"real"` or `"text"`, and
`default` the value the decoder takes when the argument is left out, if it has one. UIs
build their forms from it, so a new protocol gets one without UI code.

Decoders are listed by `br.decoders()`:
`{ {label=, name=, args=, stacked=, hold=, decoded=, error=}, ... }`: the protocol
decoder's name and arguments, how many are stacked on it, its hold, the tick it has
decoded to, and the reason it stopped (absent if it has not).

| Call | Returns |
|---|---|
| `br.items(label, from, to, max, level)` | the group's top-level items overlapping the span, oldest first (at most `max`, default 1000), with children to detail `level` (default 0); `nil, message` for a label that is no group |
| `br.last(label, n, level)` | its newest `n` items, oldest first |
| `br.item(label, id, level)` | one item by id, or `nil` once trimmed |

An item:

```lua
{
    id = 42, kind = "transaction", decoder = "bus",   -- decoder: the group's label
    start = 1200, stop = 1450,               -- ticks, inclusive
    flags = { "nak_data" },                  -- the kind's flag names, and incomplete, loss
    fields = { addr = 0x44, dir = "W+R", len = 8, status = "ACK" },
    order = { "addr", "dir", "len", "status" },   -- the kind's field order
    header = { "addr", "dir", "len", "status" },  -- the fields the text output heads with
    words = {                                -- in order; lanes by name
        { start =, stop =, value =, bits =, lane = "master", flags = { "break" } }, ...
    },
    text = false,                            -- the kind reads its words as text (UART)
    children = { ... },                      -- items, as above, to the detail asked for
}
```

Field values are integers, floats, strings or booleans as the kind declares them; a hex
field stays an integer (its format is the UI's). Fields not set are absent.

**Trigger packets** are the items of an `activity` group the UI adds itself, with a short
hold so they come soon after the capture
(`br.decoder_add("activity", "gap=300e-6,name=packets", nil, 0.05)`). The newest
complete one is `br.last("packets", 1)[1]`.
