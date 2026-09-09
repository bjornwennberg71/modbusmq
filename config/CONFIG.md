# modbusmq Configuration Guide

Reference for every config key. For a walkthrough of setting up
`modbusmq_bridge` — finding the device, building the config, checking it
without hardware — see [SUBSCRIBE.md](../SUBSCRIBE.md).

Config files use a simple `key = value` format. `#` starts a comment — everything after it on the line is ignored. Whitespace around keys and values is stripped automatically.

```
# This is a comment
modbusmq.connect = tcp://192.168.1.50:502   # inline comment also works
```

## Repeating a key

**The last value wins.** Setting a key twice is supported, not a mistake — it lets a file carry a default and override it further down, or keep an alternative visible next to the one in use rather than deleting it:

```
modbusmq.connect = tcp://192.168.1.50:502
modbusmq.connect = tcp://localhost:1502     # testing against the virtual server
```

The second line is the one that takes effect. This works for every key **except the three that allocate**: `input.max`, `write.max` and `input.N.channel.max`. Those are refused with an error, because the arrays are built the moment the key is read and a second one would discard everything already parsed into the first.

---

## Connection

### TCP

```
modbusmq.connect = tcp://192.168.1.50:502
```

### RTU (RS-232 / RS-485)

```
modbusmq.connect = rtu:///dev/ttyUSB0:9600:1:8:N
```

RTU connect string format: `rtu:///device:baudrate:stopbits:databits:parity`

| Field      | Values                        |
|------------|-------------------------------|
| device     | `/dev/ttyUSB0`, `/dev/ttyS0`, etc. |
| baudrate   | 9600, 19200, 38400, 115200, … |
| stopbits   | `1` or `2`                    |
| databits   | `7` or `8`                    |
| parity     | `N` (none), `E` (even), `O` (odd) |

### Timing

```
modbusmq.frame_timeout = 1000    # milliseconds — give up waiting for a response after this
modbusmq.rts_delay     = 10000   # microseconds — pause before transmitting (RTU only)
```

`frame_timeout` defaults to 1000 ms. For slow devices or debugging, increase it.

`rts_delay` defaults to 10000 µs (10 ms). Needed on half-duplex RS-485 to let the bus settle before transmitting. Increase if you see lost frames.

#### Setting `frame_timeout` for RTU

Set it above the device's worst-case turnaround, not its typical one. When a
response arrives *after* its request has timed out, that reply is still on the
bus with nobody waiting for it — and RTU frames carry no transaction id, so
nothing in the frame itself says which request it answers.

The library handles this: on a timeout, a rejected frame, or a failed CRC it
drains the line until it has been silent for a full inter-frame gap (3.5
character times), so the next request starts from a real frame boundary. That
costs up to a few hundred milliseconds at low baud rates, and the affected poll
publishes nothing and logs the reason — deliberately, because the alternative is
publishing another block's registers under this block's topics.

A timeout that is merely tight therefore shows up as gaps and `Waited ... ms for
a response` in the log rather than as wrong values. If you see those regularly,
raise `frame_timeout`.

---

## Inputs

An **input** is one Modbus read request — a slave device, a register type, a starting address, and a count. Each input is polled repeatedly at its configured interval.

### Declaring inputs

`input.max` must appear **before** any `input.N.*` keys. It allocates the input slots.

```
input.max        = 3       # number of inputs (1–100)
input.query_mode = series  # parallell (default) or series
```

| query_mode   | Behaviour |
|--------------|-----------|
| `parallell` (or `0`) | Each input polls independently at its own interval |
| `series` (or `1`)    | Inputs poll one after another, staggered so they do not overlap |

Use `series` when all inputs share the same physical bus and simultaneous requests would cause collisions (typical for RTU multi-drop).

### Per-input keys

Replace `N` with the input number (1-based).

```
input.N.slave          = 39               # Modbus slave / device ID
input.N.type           = input_register   # see the table below
input.N.address        = 0x0FFF           # start register address (hex or decimal)
input.N.address_offset = 0        # registers/coils to skip at the start of the block, see below
input.N.naddress       = 0x27             # number of registers to read
input.N.interval       = 1000             # polling interval in milliseconds
input.N.channel.max    = 13               # number of channels — must come before channel keys

# Publish rate-limiting defaults for every channel on this input that does not
# set its own — see "Publishing: decimals and rate limiting" below.
input.N.on_change      = 0                # publish only when the printed text changes
input.N.min_change     = 0                # publish only when the value moves by at least this much
input.N.min_change_rel = 0                # ...or by at least this fraction of the value, whichever is larger
input.N.min_interval   = 0                # never publish more often than this, in ms
input.N.max_interval   = 0                # heartbeat: publish anyway after this long, in ms
```

| type               | Modbus | Reads                    | `naddress` counts | `channel.offset` is |
|--------------------|--------|--------------------------|-------------------|---------------------|
| `holding_register` | 03     | read/write registers     | registers         | bytes               |
| `input_register`   | 04     | read-only registers      | registers         | bytes               |
| `coil`             | 01     | read/write bits          | coils             | a coil index        |
| `discrete_input`   | 02     | read-only bits           | inputs            | a bit index         |

#### `address_offset`

`address_offset` is added to `input.N.address` to form the address that goes
out on the wire, and does nothing else. Use it when the first register or two
of a documented block must not be read — reserved words some firmware faults
on — without renumbering every channel: `channel.offset` stays measured from
`input.N.address`, the datasheet's base, and the library takes the shift back
out when it decodes. It counts registers for a register block and coils for a
bit block, the same unit as `address`.

```
input.3.address        = 0x2000   # datasheet base
input.3.address_offset = 1        # request actually starts at 0x2001
input.3.naddress       = 2        # ...and covers 0x2001-0x2002
input.3.channel.1.offset = 2      # register 0x2001, still counted from 0x2000
```

A channel that lands inside the skipped registers is outside the response and
is reported as such, the same as any other out-of-range offset.

---

## Bit inputs: coils and discrete inputs

A `coil` or `discrete_input` block polls bits rather than registers. The device answers with them packed eight to a byte, lowest address in the lowest bit.

Because the unit of that address space is a bit, `channel.offset` counts **coils, not bytes** — the same idea as a register block, where it counts bytes. Coil 9 is byte 1, bit 1; you do not work that out yourself.

A bit channel takes **no `format`** — there is nothing to decode. Setting one is a config error, because it almost always means the block was written against the wrong address space.

The four types are four separate address spaces on the device, so a `discrete_input` block at address `0x0000` and an `input_register` block at `0x0000` on the same slave are two different things and may both be present. Responses are matched back to their block by slave, type and address together.

```
# DBS aircon alarms, read as discrete inputs
input.2.slave       = 39
input.2.type        = discrete_input
input.2.address     = 0x0000
input.2.naddress    = 16                  # 16 bits
input.2.interval    = 2000
input.2.channel.max = 2

input.2.channel.1.offset = 0              # the bit at address 0x0000
input.2.channel.1.topic  = alarms/high_temperature

input.2.channel.2.offset = 9              # the bit at address 0x0009
input.2.channel.2.topic  = alarms/door_open
```

A bit publishes as `1.000` or `0.000`. Scaling still applies, which is how an **active-low** signal is turned the right way round without a key of its own:

```
input.2.channel.3.offset = 1
input.2.channel.3.add    = -1             # 0 -> -1, 1 -> 0
input.2.channel.3.mul    = -1             # -1 -> 1,  0 -> 0
input.2.channel.3.topic  = status/ok
```

---

## Channels

A **channel** extracts one value from the response data, applies scaling, and maps it to an MQTT topic.

`input.N.channel.max` must appear **before** any `input.N.channel.M.*` keys for that input.

### Per-channel keys

Replace `N` with the input number and `M` with the channel number (both 1-based).

```
input.N.channel.M.offset = 4        # byte offset into the response data
input.N.channel.M.format = int_ab   # data format (see below)
input.N.channel.M.add    = -400     # added to the raw value before scaling
input.N.channel.M.mod    = -10      # scaling divisor/multiplier (see below)
input.N.channel.M.mul    = -1       # final multiplier
input.N.channel.M.topic  = battery/module/1/voltage   # MQTT topic
input.N.channel.M.value  = 4800     # default value (used by modbusmq_server)
input.N.channel.M.retain = 0        # override mqtt.retain for this channel
input.N.channel.M.qos    = 1        # override mqtt.qos for this channel

# Formatting and publish rate limiting — see the section below. Each of these
# overrides the input-level (and, failing that, the config-wide) default.
input.N.channel.M.decimals       = 2     # decimal places when printing/publishing
input.N.channel.M.on_change      = 0     # publish only when the printed text changes
input.N.channel.M.min_change     = 0.5   # publish only when the value moves by at least this much
input.N.channel.M.min_change_rel = 0.001 # ...or by at least this fraction of the value (0.001 = 0.1%)
input.N.channel.M.min_interval   = 5000  # never publish more often than this, in ms
input.N.channel.M.max_interval   = 60000 # heartbeat: publish anyway after this long, in ms
```

Only `offset`, `format`, and `topic` are required. `add`, `mod`, `mul`, and `value` are optional.

### Data formats

The name says the type, the suffix says the byte order: `ab` high byte first, `ba` low byte first, `abcd` and `badc` the 32-bit equivalents. `int_*` is signed, `uint_*` is unsigned.

| Format        | Size   | Description                          |
|---------------|--------|--------------------------------------|
| `uint_a`      | 1 byte | unsigned 8-bit                       |
| `int_a`       | 1 byte | signed 8-bit                         |
| `uint_ab`     | 2 bytes | unsigned 16-bit, big-endian (most common) |
| `uint_ba`     | 2 bytes | unsigned 16-bit, little-endian      |
| `int_ab`      | 2 bytes | signed 16-bit, big-endian           |
| `int_ba`      | 2 bytes | signed 16-bit, little-endian        |
| `uint_abcd`   | 4 bytes | unsigned 32-bit, big-endian         |
| `uint_badc`   | 4 bytes | unsigned 32-bit, mixed-endian       |
| `int_abcd`    | 4 bytes | signed 32-bit, big-endian           |
| `int_badc`    | 4 bytes | signed 32-bit, mixed-endian         |
| `float_abcd`  | 4 bytes | IEEE 754 float, big-endian          |
| `float_badc`  | 4 bytes | IEEE 754 float, mixed-endian (BADC) |
| `float_dcba`  | 4 bytes | IEEE 754 float, little-endian       |
| `float_cdab`  | 4 bytes | IEEE 754 float, low word first      |

The type also decides how the value is printed and published. An integer format
publishes as a whole number, or with exactly as many decimals as its `mod`
divisor implies: `uint_ab` with no `mod` gives `3`, `int_ab` with `mod = -10`
gives `21.5`. A float format always publishes with three decimals, since the
device gave no hint of its real precision. A bit publishes as `1` or `0`.
`decimals` overrides any of this per channel; see "Publishing: decimals and
rate limiting" below.

Pick `uint_ab` for anything that cannot go negative — a speed, a voltage, a percentage, a status word — and `int_ab` for anything that can. Getting it wrong on a temperature is the classic failure: read unsigned, an ambient of −5.0 °C arrives as `0xFFCE`, decodes as 65486 and scales to 6548.6 °C.

A **status or flag word must be unsigned**. Read signed, a word with the top bit set publishes as a negative number instead of the bit pattern you wanted.

#### `int_ab` changed meaning in config.version 2.0

`int_ab` and `int_ba` originally meant *unsigned*, and there was no signed 16-bit format at all. That is now fixed, but a config file already deployed somewhere must not change meaning just because the library was upgraded — so the new meaning is opt-in:

```
config.version = 2.0     # int_* is signed, uint_* is unsigned
```

Below 2.0, or with no `config.version` at all, `int_a`/`int_ab`/`int_ba` keep the old unsigned meaning and the program logs one line at startup naming the file and how many channels are affected. Nothing breaks and nothing changes underneath you.

**To migrate a config**: set `config.version = 2.0`, then go through each `int_ab` channel and decide. Anything that genuinely cannot go negative becomes `uint_ab`; anything that can stays `int_ab` and now decodes correctly. It is worth doing channel by channel rather than with a search and replace — the channels where the answer is "signed" are exactly the ones that were quietly broken before.

The 32-bit formats did not change behaviour. `int_abcd` was already signed, though only by an implementation-defined conversion rather than on purpose; it is now signed deliberately, and `uint_abcd` exists for the other case.

### Scaling

Values are scaled in this order:

```
result = (raw + add)
if mod < 0:  result = result / abs(mod)
if mod > 0:  result = result * mod
if mul != 0: result = result * mul
```

**Examples:**

| Raw value | add    | mod  | mul | Result           | Use case |
|-----------|--------|------|-----|------------------|----------|
| 4800      | 0      | -100 | 0   | 48.00            | voltage in hundredths of a volt |
| -50       | 0      | -10  | 0   | -5.00            | sub-zero temperature, `int_ab` |
| 10500     | -10000 | -10  | -1  | 50.00            | signed current with offset |
| 434       | -400   | -10  | 0   | 3.4              | temperature with -40 offset |

### Publishing: decimals and rate limiting

Every channel value is printed the same way whether it goes to the log or to
MQTT, and every poll is a candidate to publish — by default, every one of them
does, which is what always happened before these settings existed.

#### Decimal places

Without `decimals` set, the number of decimal places follows from the type:

- a coil or discrete input channel has nothing to round: **0 decimals**.
- a float format (`float_abcd` and friends) always prints to **3 decimals**,
  regardless of scaling.
- an integer format follows its own `mod`: `mod = -100` means the raw value is
  hundredths, so it prints to 2 decimals; `mod = -10` to 1; `mod` that is
  positive, zero, or unset prints as a whole number. A negative `mod` that is
  not a clean power of ten (`-25`, say) still needs some precision, so that
  case falls back to 3 rather than rounding the value away.

Set `input.N.channel.M.decimals` to override this for one channel.

This is a formatting change only — it does not touch how a value is scaled,
only how many digits of the already-scaled result get printed. A status word
with no `mod` used to publish as `3.000`; it now publishes as `3`.

#### Publish rate limiting

Five keys decide whether a freshly polled value is worth sending at all, each
settable per channel (`input.N.channel.M.<key>`), inherited from its input
(`input.N.<key>`) when the channel does not set its own, and inherited from a
config-wide default (`publish.<key>`) when neither does. All default to off,
which is today's behaviour: publish every poll.

- **`on_change`** (`0`/`1`, default `0`): when set, an unchanged value is
  suppressed even if `min_change` is `0`.
- **`min_change`** (a value in the channel's own engineering units, default
  `0`): publish only when the value has moved by at least this much since the
  last publish.
- **`min_change_rel`** (a fraction, default `0`, e.g. `0.001` = 0.1%):
  publish only when the value has moved by at least this fraction of itself.
  The effective threshold is the *larger* of `min_change` and
  `min_change_rel × value` — set both and whichever is more demanding at the
  moment wins. A reading near zero is floored at a reference of `1.0` before
  the fraction is applied, so a relative threshold never shrinks to nothing
  just because the value passed through zero.
- **`min_interval`** (milliseconds, default `0`): never publish this channel
  more often than this. A change that arrives inside the window is **dropped,
  not queued** — the next poll still compares against the last value actually
  published, so a real change is still caught once the window has passed, just
  not the instant it happened.
- **`max_interval`** (milliseconds, default `0` = off): a heartbeat. Publish
  regardless of the above once this long has passed since the last publish, so
  a subscriber can tell the channel is still alive even when nothing changed.
  Since this is only checked when a poll lands, it effectively **rounds up to
  the next poll interval** — `max_interval = 3000` on a channel polled every
  2000 ms fires around 4000 ms, not 3000.

"Unchanged" for `on_change`, and for deciding whether the value moved at all,
is judged on the **printed text** — at the channel's own decimal count — not
the raw float. A reading wobbling in the noise below the last printed digit
does not count as a change.

Precedence, in order: never published yet always publishes; then the
heartbeat; then `min_interval` can suppress a value that would otherwise go
out; only then do `on_change`/`min_change`/`min_change_rel` get a say.

**Recommended combinations:**

```
# Alarm / status bits: only publish when something actually changes, but say
# you're still alive once an hour even if nothing did.
input.2.channel.1.on_change    = 1
input.2.channel.1.max_interval = 3600000

# A temperature: ignore poll-to-poll jitter under half a degree, never more
# than once every 5s, but heartbeat once a minute.
input.1.channel.4.min_interval   = 5000
input.1.channel.4.min_change     = 0.5
input.1.channel.4.max_interval   = 60000

# A power reading that swings across a wide range: a fixed min_change is
# either too tight at high power or too loose at low power, so use a
# percentage instead.
input.1.channel.7.min_change_rel = 0.001
```

**Suppressed values are still visible** with `-v`, logged at debug level with
the reason — nothing is silently dropped from the log, only from the broker.

**Interaction with `retain`:** with `min_change` (or `on_change`/
`min_change_rel`) set and `retain = 0`, a subscriber connecting between
publishes sees nothing for this channel until it next changes or heartbeats.
Pair a suppressive setting with either `retain = 1` or a `max_interval`, or a
fresh subscriber can be left looking at a channel that appears dead.

---

## Writes (MQTT to Modbus)

A **write** is the mirror image of an input: instead of polling a register and publishing the result, `modbusmq_bridge` subscribes to an MQTT topic and writes whatever arrives to a register or coil.

`write.max` must appear **before** any `write.N.*` keys, the same way `input.max` does.

```
write.max = 2
```

Writes are **fire and forget**. The request is queued alongside the polls, so it shares the bus in turn and needs no separate connection; nothing waits for the echo. A write that fails is reported through the normal error path, tagged with its slave and address.

### Per-write keys

```
write.N.name      = compressor_setpoint   # used in log lines; optional
write.N.slave     = 39                    # required, never defaulted
write.N.type      = holding_register      # holding_register or coil
write.N.function  = write_register        # write_register, write_registers or write_coil
write.N.address   = 0x0028                # absolute register or coil address, required
write.N.format    = int16                 # required for register writes
write.N.add       = 0                     # scaling, applied in reverse (see below)
write.N.mod       = -10
write.N.mul       = 1
write.N.on_value  = 1                     # optional discrete command mapping
write.N.off_value = 0
write.N.topic     = settings/compressor   # subscribed, never published
```

| Function           | Modbus | Writes                                  |
|--------------------|--------|-----------------------------------------|
| `write_register`   | 06     | one register — 2-byte formats           |
| `write_registers`  | 16     | two registers — 4-byte formats           |
| `write_coil`       | 05     | one coil (bit)                          |

`write.N.type` accepts `coil` or `holding_register` only. A `discrete_input` is read-only by definition and an `input_register` has no write function at all, so both are rejected.

`type` and `function` are two ways of saying the same thing. Give either one and the other is derived; give both and they must agree. A 4-byte format requires `write_registers`, because function 06 writes a single register.

`slave` is never defaulted — writing to a guessed device is not a recoverable mistake.

### Scaling a write

Scaling is the exact inverse of a channel's, undone in reverse order:

```
raw = value
if mul != 0: raw = raw / mul
if mod > 0:  raw = raw / mod
if mod < 0:  raw = raw * abs(mod)
raw = raw - add
raw = round(raw)
```

So a channel reading `mod = -10` publishes `raw / 10`, and a write with `mod = -10` sends `round(value * 10)`. A value that does not fit the format is **rejected and logged, never truncated** — a setpoint that silently wraps to a small number is worse than one that never arrives, because the device accepts it without complaint.

### Commands: on_value / off_value

With `on_value` and `off_value` set, the payload is treated as a command rather than a measurement. `1`/`0` work, and so do `on`/`off`/`true`/`false` in any case, since brokers differ on what they publish. Anything else is rejected and logged.

Set both or neither. They work for register writes too, not just coils — some models take power on/off as a register value rather than a coil.

### Example

```
write.max = 2

# Compressor setpoint, degC x10, in a signed holding register
write.1.name     = compressor_on_temperature_setpoint
write.1.slave    = 39
write.1.type     = holding_register
write.1.function = write_register
write.1.address  = 0x0028
write.1.format   = int16
write.1.mod      = -10
write.1.topic    = settings/compressor_on_temperature

# Unit power, a coil (function 05)
write.2.name      = power_on_off
write.2.slave     = 39
write.2.type      = coil
write.2.function  = write_coil
write.2.address   = 0x000C
write.2.on_value  = 1
write.2.off_value = 0
write.2.topic     = commands/power
```

Publishing `25.5` to `settings/compressor_on_temperature` writes `0x00FF` (255) to register `0x0028` on slave 39. Publishing `ON` to `commands/power` sets coil `0x000C`.

### Notes

- **Do not retain command topics.** A retained payload is replayed by the broker every time we subscribe, including after every reconnect, so a retained setpoint gets rewritten to the device on each one. This is the publisher's setting, not something the config can override.
- **Writes need MQTT.** Without `mqtt.connect`, or in a build without `--enable-mqtt`, write entries are parsed and warned about but can never fire.
- **Several entries may share a topic**, and one publish then fires all of them. Deliberate, but easy to do by accident with placeholder topics.
- `mqtt.topic_prefix` applies to write topics as well as published ones.

---

## MQTT

```
mqtt.name         = my_device          # MQTT client identifier
mqtt.connect      = mqtt://localhost:1883
mqtt.topic_prefix = factory/line1/     # prepended to every channel topic
mqtt.retain       = 1                  # default retain flag, 1/0 true/false yes/no
mqtt.qos          = 0                  # default QoS, 0, 1 or 2
```

### retain and qos

`mqtt.retain` defaults to `1` and `mqtt.qos` to `0`, which is what publishing has
always done, so leaving them out changes nothing.

Retain is right for slow-moving state — a voltage, a state of charge — because a
subscriber connecting midway gets a value immediately instead of waiting a poll
interval. It is wrong for anything event-like, where a stale retained reading
looks live forever and there is no way to tell how old it is.

Either can be overridden per channel:

```
input.1.channel.4.retain = 0     # this one is not worth keeping
input.1.channel.4.qos    = 1
```

A channel with no `retain`/`qos` of its own takes the `mqtt.*` value.

Note that this governs only what modbusmq **publishes**. Write topics are
subscribed at QoS 1, and whether an incoming command arrives retained is the
publisher's setting, not something this config can override — see the note under
Writes.

If `mqtt.topic_prefix` is set, the final published topic is `prefix + channel.topic`. For example, with prefix `factory/line1/` and channel topic `battery/voltage`, the message is published to `factory/line1/battery/voltage`.

MQTT publishing requires building with `-DMQTT_ENABLED=ON`.

### `publish.*`: config-wide rate-limiting defaults

The five keys from "Publishing: decimals and rate limiting" above also exist
at the top level, as the bottom rung under `input.N.*` and
`input.N.channel.M.*`:

```
publish.on_change      = 0
publish.min_change     = 0
publish.min_change_rel = 0
publish.min_interval   = 0
publish.max_interval   = 0
```

A channel takes its own value if it sets one, else its input's, else this one.
Each key resolves independently, so `publish.min_interval = 1000` does not
drag `on_change` along with it for a channel that never asked for it. Useful
for a whole config that should default to, say, a one-minute heartbeat without
repeating `max_interval = 60000` on every input.

---

## Complete example

```
# Shoto V2 battery — single module via RTU
modbusmq.connect      = rtu:///dev/ttyUSB0:9600:1:8:N
modbusmq.rts_delay    = 10000   # 10 ms
modbusmq.frame_timeout = 1000

input.max        = 1
input.query_mode = series

input.1.slave       = 39
input.1.type        = input_register
input.1.address     = 0x0FFF
input.1.naddress    = 0x27
input.1.interval    = 1000
input.1.channel.max = 4

# Voltage: raw is in hundredths of a volt
input.1.channel.1.offset = 0
input.1.channel.1.format = int_ab
input.1.channel.1.mod    = -100
input.1.channel.1.topic  = battery/1/voltage

# Current: raw is signed with 10000 offset, in tenths of an amp
input.1.channel.2.offset = 2
input.1.channel.2.format = int_ab
input.1.channel.2.add    = -10000
input.1.channel.2.mod    = -10
input.1.channel.2.mul    = -1
input.1.channel.2.topic  = battery/1/current

# State of Charge: raw is in hundredths of a percent
input.1.channel.3.offset = 16
input.1.channel.3.format = int_ab
input.1.channel.3.mod    = -100
input.1.channel.3.topic  = battery/1/soc

# Average cell temperature: raw has -40 offset, in tenths of a degree
input.1.channel.4.offset = 6
input.1.channel.4.format = int_ab
input.1.channel.4.add    = -400
input.1.channel.4.mod    = -10
input.1.channel.4.topic  = battery/1/avg_cell_temp

mqtt.name    = battery_monitor
mqtt.connect = mqtt://localhost:1883
```

---

## Common pitfalls

- **A repeated key takes its last value**, except `input.max`, `write.max` and `input.N.channel.max`, which are an error.
- **`write.max` must appear before any `write.N.*` key**, and a `write.N` beyond `write.max` is a hard error rather than a silent skip.
- **`input.max` must appear before any `input.N.*` key.** Same for `input.N.channel.max` before channel keys. The parser allocates memory when it sees these declarations; later keys that reference out-of-range indices are silently skipped.
- **`input.query_mode` must appear before `input.N.*` keys** for the mode to take effect when building the timer list.
- **Hex addresses** (`0x0FFF`) work in address fields.
- **Byte offset vs register offset**: for a register block `channel.offset` is in bytes — register 3 of a 2-byte-per-register response is at byte offset 6. For a `coil` or `discrete_input` block it is a coil index instead, because a bit has no byte offset of its own.
- **A dropped write is reported, not silent.** If the Modbus link goes down with requests still queued, each one is logged and passed to the error callback rather than discarded quietly.
