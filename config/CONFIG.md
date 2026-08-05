# modbusmq Configuration Guide

Config files use a simple `key = value` format. `#` starts a comment — everything after it on the line is ignored. Whitespace around keys and values is stripped automatically.

```
# This is a comment
modbusmq.connect = tcp://192.168.1.50:502   # inline comment also works
```

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
input.N.type           = input_register   # input_register or holding_register
input.N.address        = 0x0FFF           # start register address (hex or decimal)
input.N.naddress       = 0x27             # number of registers to read
input.N.interval       = 1000             # polling interval in milliseconds
input.N.channel.max    = 13               # number of channels — must come before channel keys
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
```

Only `offset`, `format`, and `topic` are required. `add`, `mod`, `mul`, and `value` are optional.

### Data formats

| Format        | Size   | Description                          |
|---------------|--------|--------------------------------------|
| `int_a`       | 1 byte | unsigned 8-bit                       |
| `int_ab`      | 2 bytes | 16-bit, big-endian (most common)    |
| `int_ba`      | 2 bytes | 16-bit, little-endian               |
| `int_abcd`    | 4 bytes | 32-bit, big-endian                  |
| `int_badc`    | 4 bytes | 32-bit, mixed-endian (BADC)         |
| `float_abcd`  | 4 bytes | IEEE 754 float, big-endian          |
| `float_badc`  | 4 bytes | IEEE 754 float, mixed-endian (BADC) |
| `float_dcba`  | 4 bytes | IEEE 754 float, little-endian       |

When in doubt, start with `int_ab` — it is the most common encoding for Modbus devices.

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
| 10500     | -10000 | -10  | -1  | 50.00            | signed current with offset |
| 434       | -400   | -10  | 0   | 3.4              | temperature with -40 offset |

---

## MQTT

```
mqtt.name         = my_device          # MQTT client identifier
mqtt.connect      = mqtt://localhost:1883
mqtt.topic_prefix = factory/line1/     # prepended to every channel topic
```

If `mqtt.topic_prefix` is set, the final published topic is `prefix + channel.topic`. For example, with prefix `factory/line1/` and channel topic `battery/voltage`, the message is published to `factory/line1/battery/voltage`.

MQTT publishing requires building with `-DMQTT_ENABLED=ON`.

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

- **`input.max` must appear before any `input.N.*` key.** Same for `input.N.channel.max` before channel keys. The parser allocates memory when it sees these declarations; later keys that reference out-of-range indices are silently skipped.
- **`input.query_mode` must appear before `input.N.*` keys** for the mode to take effect when building the timer list.
- **Hex addresses** (`0x0FFF`) work in address fields.
- **Byte offset vs register offset**: `channel.offset` is always in bytes. Register 3 of a 2-byte-per-register response is at byte offset 6.
