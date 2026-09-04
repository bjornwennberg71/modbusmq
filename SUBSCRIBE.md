# Setting up modbusmq_subscribe

`modbusmq_subscribe` polls one or more Modbus devices on a schedule and publishes
what it reads to MQTT. With a `write.N` section it also goes the other way:
subscribe to a topic, write the value to a register or coil.

This is the walkthrough. [config/CONFIG.md](config/CONFIG.md) is the reference for
every key.

---

## 1. Build with MQTT

MQTT is off by default, and without it `modbusmq_subscribe` polls but publishes
nothing:

```bash
bash setup.sh --enable-mqtt
(cd release && make)
```

The flag is cached in `CMakeCache.txt`, so a plain `cmake ..` afterwards keeps it.

---

## 2. Find the device

Before writing any config, confirm the device answers. `modbusmq_query` needs no
config file:

```bash
# slave 39, 4 input registers from 0x0000, 2-byte values
./modbusmq_query rtu:///dev/ttyUSB0:9600:1:8:N 39 input_register 0x0000 4 -2
```

If you do not know the slave id, walk it — most devices ship as 1. If nothing
answers at all, the problem is the connect string, the wiring, or the baud rate,
not the config.

For RTU the fields are `device:baud:stopbit:databits:parity`, **parity last**.

---

## 3. The smallest working config

```
config.name    = my device
config.version = 2.0

modbusmq.connect = rtu:///dev/ttyUSB0:9600:1:8:N

input.max = 1

input.1.slave       = 39
input.1.type        = input_register
input.1.address     = 0x0000
input.1.naddress    = 4
input.1.interval    = 2000
input.1.channel.max = 1

input.1.channel.1.offset = 0
input.1.channel.1.format = uint_ab
input.1.channel.1.topic  = voltage

mqtt.name    = my_device
mqtt.connect = mqtt://localhost:1883
```

Run it:

```bash
./modbusmq_subscribe -c my_device.config -v
```

**Set `config.version = 2.0` on anything new.** Below that, `int_ab` keeps an old
meaning where it was unsigned — see step 5.

---

## 4. One input block per request

An input is one Modbus read, repeated every `interval` milliseconds. Registers
that are far apart belong in separate blocks; a single block reading a huge span
to reach two useful registers wastes the bus.

```
input.N.slave       = 39               # device id
input.N.type        = input_register   # input_register | holding_register | coil | discrete_input
input.N.address     = 0x0000           # first register
input.N.naddress    = 0x1F             # how many
input.N.interval    = 2000             # ms
input.N.channel.max = 9                # must come before the channel keys
```

`input.max` and `input.N.channel.max` allocate, so they must appear **before** the
keys they cover. They are also the only keys that refuse to be repeated.

On a shared RS-485 bus set `input.query_mode = series` so blocks do not transmit
over each other.

---

## 5. Channels: getting the number right

A channel picks one value out of the response, scales it, and gives it a topic.

```
input.1.channel.3.offset = 8         # BYTE offset into the response, not a register number
input.1.channel.3.format = int_ab
input.1.channel.3.mod    = -10
input.1.channel.3.topic  = sensors/inlet_temperature
```

Three things to get right, in this order:

**Offset is in bytes.** Register 4 of a 2-byte-per-register response is at offset
8. This is the most common mistake.

**Signed or unsigned.** `int_*` is signed, `uint_*` is unsigned; the `ab`/`ba`
suffix is byte order. Anything that can go negative — temperatures, currents —
needs `int_ab`. Read unsigned, -5.0 °C arrives as `0xFFCE` and publishes as
6548.6. Status and flag words must stay `uint_ab`, or a top bit set turns into a
negative.

**Scaling**, applied in this order:

```
result = (raw + add)
if mod < 0: result = result / abs(mod)
if mod > 0: result = result * mod
if mul != 0: result = result * mul
```

`add` lands **before** the scaling. A device reporting tenths of a degree with a
-40 offset is `add = -400`, `mod = -10`.

---

## 6. Check it without the device

`modbusmq_server` serves a config's `value =` defaults as a virtual device, so a
config can be verified before going near hardware:

```bash
# point a copy of the config at tcp://localhost:1502, give channels a value =
./modbusmq_server -c my_device.config &
./modbusmq_subscribe -c my_device.config -v
```

This confirms offsets, formats and scaling. It cannot confirm the register
addresses are the ones the real device uses.

---

## 7. Bits: coils and discrete inputs

```
input.2.type        = discrete_input   # function 02, read-only bits
input.2.address     = 0x0000
input.2.naddress    = 16               # counts bits
input.2.channel.max = 2

input.2.channel.1.offset = 0           # a COIL INDEX here, not a byte offset
input.2.channel.1.topic  = alarms/high_temperature
input.2.channel.2.offset = 9
input.2.channel.2.topic  = alarms/door_open
```

Bit channels take **no `format`** — there is nothing to decode, and setting one is
an error. They publish `1.000` or `0.000`.

Active low is handled with the scaling rather than a key of its own: `add = -1`
with `mul = -1` turns a raw 0 into a published 1.

---

## 8. Writing from MQTT

A `write.N` entry subscribes a topic and writes what arrives. It carries its own
slave and address, because the register you set a value in is usually not the one
you read it back from.

```
write.max = 2

write.1.name     = compressor_setpoint
write.1.slave    = 39
write.1.type     = holding_register
write.1.function = write_register        # 06; write_registers is 16, for 4-byte formats
write.1.address  = 0x0028
write.1.format   = int_ab
write.1.mod      = -10
write.1.topic    = settings/compressor_on_temperature

write.2.name      = power
write.2.slave     = 39
write.2.type      = coil
write.2.function  = write_coil           # 05
write.2.address   = 0x000C
write.2.on_value  = 1
write.2.off_value = 0
write.2.topic     = commands/power
```

Publishing `25.5` to the first writes `0x00FF`. Publishing `on`, `true` or `1` to
the second sets the coil.

Check the scaling before sending anything, with no device attached:

```bash
./modbusmq_query rtu:///dev/ttyUSB0:9600:1:8:N 39 holding_register 0x0028 \
    --write 25.5 --format int_ab --mod -10 --dry-run
```

Writes are fire and forget. A failure is logged and reported through the error
callback; nothing waits for the echo and nothing retries.

**Do not retain command topics.** A retained payload is replayed by the broker on
every subscribe, including after each reconnect, so a retained setpoint gets
rewritten to the device every time. That is the publisher's setting, not something
the config can override.

---

## 9. MQTT

```
mqtt.name         = my_device        # client id, must be unique on the broker
mqtt.connect      = mqtt://localhost:1883
mqtt.topic_prefix = factory/line1/   # prepended to every topic, read and write
mqtt.retain       = 1                # default; 1/0, true/false, yes/no
mqtt.qos          = 0                # default; 0, 1 or 2
```

Published values are strings with three decimals, and the prefix applies to write
topics as well as published ones.

`retain` suits slow-moving state — a subscriber that connects midway gets a value
straight away rather than waiting a poll interval. It suits anything event-like
much less, because a stale retained reading looks live forever. Override it per
channel where the default is wrong:

```
input.1.channel.4.retain = 0
input.1.channel.4.qos    = 1
```

The defaults match what modbusmq has always done, so an existing config behaves
exactly as before.

---

## 10. Reading the log

`-v` turns on the frame detail. A healthy run repeats one line per channel per
interval:

```
info: factory/line1/sensors/inlet_temperature=21.500
```

| What you see | Usually means |
|---|---|
| `no response within N ms` | wrong slave id, wrong baud, or the device is not there |
| `byte count mismatch` | `naddress` disagrees with what the device returned |
| `offset N is outside the M bytes received` | the channel offset is past the end of the block |
| a value ~6550 where you expected a small negative | `uint_ab` on a signed register — use `int_ab` |
| a value 10x or 100x off | `mod` sign or magnitude |
| `config.version is X, so N channel(s) ... keep the old UNSIGNED meaning` | migrate to `config.version = 2.0` |
| nothing published, no errors | built without `--enable-mqtt`, or no `mqtt.connect` |
| frames lost on RTU | raise `modbusmq.rts_delay`, then `modbusmq.frame_timeout` |

---

## See also

- [config/CONFIG.md](config/CONFIG.md) — every key, in reference form
- [README.md](README.md) — the other two programs
