<p align="center">
  <img src="images/modbusmq_small.png" alt="modbusmq logo" width="600">
</p>

# modbusmq
A config-driven MQTT bridge for Modbus devices, and the C library underneath it.

`modbusmq` puts an MQTT front end on any Modbus device. Registers and coils become
topics you can subscribe to; values published to a topic become writes back to the
device. The register map, the scaling and the topic names all live in a plain
`.config` file, so a new meter, battery or inverter is a file to write, not code.

It provides:

- **modbusmq** — a bidirectional MQTT ↔ Modbus bridge: polls registers and coils onto topics, and writes values arriving on topics back to the device
- **libmodbusmq** — the single-threaded, event-driven Modbus TCP/RTU library it is built on, useful on its own
- **Standalone tools** for one-shot queries and for emulating a Modbus device
- **Device configuration examples** for Accuvim II, Polarium, Murata and Shoto systems

The project is designed to be simple to integrate, portable, and suitable for both embedded and server-side applications.

## Background

The origin of **modbusmq** came from a practical requirement: I needed a reliable way to query a wide range of batteries, meters, and industrial devices—such as Polarium battery strings, Shoto racks, and Accuvim II power meters to name a few from the top of my head. Since most of these devices expose Modbus registers, the excellent open-source `libmodbus` library from Stephane Raimbault (https://github.com/stephane/libmodbus) was the natural design reference for what a solid Modbus transport layer should look like. `modbusmq` is an independent implementation written from scratch — it does not share or derive any code from `libmodbus`.

However, several challenges quickly became apparent:

1. **Multiple adapters, one process**  
   Deployments often require communication with several devices simultaneously. Managing one thread per device is overkill.

2. **Straightforward MQTT integration**  
   Device data needed to feed directly into monitoring dashboards and energy management systems. A clean translation layer from Modbus registers to MQTT topics was essential.

3. **Configuration-driven behavior**  
   New devices should be supported without recompiling code. Human-readable `.config` files defining registers, scaling, types, and defaults were a natural solution.

These requirements led to the design of a **single-threaded, event-driven state machine** for Modbus operations. Instead of blocking per request or relying on worker threads, all reads and writes are scheduled cooperatively through a shared FSM that behaves the same for both TCP and RTU. This approach eliminates race conditions, simplifies concurrency, and delivers predictable timing.

As the design evolved, it became clear that the scheduler and Modbus abstraction formed a reusable component—now the core **libmodbusmq** library. On top of this library, a set of standalone tools was built:

- `modbusmq_query` — simple direct Modbus operations  
- `modbusmq` — the MQTT bridge, reading and writing from `.config` files  
- `modbusmq_server` — a virtual Modbus device serving configurable default values  
- I have several other programs I might add to the system in the near future time permitting.
The result is a lightweight, extensible system that can communicate with multiple devices, over multiple adapters, without multithreading, while remaining easy to deploy, configure, and maintain.

What was not planned is where `modbusmq` ended up. It started as a poller
that happened to publish what it read. Once `write.N` entries arrived it could go the
other way too, and at that point it stopped being a polling tool and became the whole
translation layer: a generic MQTT front end for any Modbus device, driven entirely by
configuration. That is now the main way the project gets used.

---

## Features

### Library (`libmodbusmq`)
- Unified Modbus TCP/RTU API  
- Event-driven async read/write  
- Synchronous support for backward compatibility
- Flexible subscription mechanism  
- Clean C API with full documentation  
- Suitable for embedded gateways and constrained environments  
- Connect string for TCP and RTU

### Tools
- **modbusmq** — the MQTT ↔ Modbus bridge: scheduled reads out to topics, topic writes back to registers and coils  
- **modbusmq_query** — one-shot tool for direct Modbus reads and writes  
- **modbusmq_server** — virtual Modbus device emulator returning `.config` default values  

### Configuration
**[MODBUSMQ.md](MODBUSMQ.md) — how to set up `modbusmq` and write its config file.**
Start there. [config/CONFIG.md](config/CONFIG.md) is the reference for every key.

`.config` files describe:
- Device connection (TCP or RTU)  
- Registers/coils to read  
- Scaling, formatting, and units  
- Publish rate limiting: on-change, deadband, min/max interval  
- Default values  
- Optional MQTT topic mapping  

Example configs included:
- `accuvim_ii.config`  
- `polarium.config`  
- `shoto.config`
- `example1.config` → `example3.config`, graded from minimal to fully fledged

---

## Repository Structure
```
modbusmq/
├── lib/ # Core C library
├── programs/modbusmq.c # MQTT <-> Modbus bridge service
├── programs/modbusmq_query.c # One-shot query tool
├── programs/modbusmq_server.c # Virtual Modbus device emulator
├── config/*.config # Device configuration examples
├── MODBUSMQ.md  # Setting up modbusmq
├── config/CONFIG.md # Config file key reference

```

---

## Building

Set up the `debug/` and `release/` build directories:
```bash
(bash ./setup.sh)
```

Build (debug):
```bash
(cd debug && make)
```

Build (release):
```bash
(cd release && make)
```

Build with MQTT support (links against `libmosquitto`):
```bash
(cd debug && cmake -DMQTT_ENABLED=ON .. && make)
```

Install (into `debug/modbusmq/` or `release/modbusmq/`):
```bash
(cd debug && make install)
```

Build output includes:
```
lib/libmodbusmq.so
programs/modbusmq
programs/modbusmq_query
programs/modbusmq_server
config/*.config
```


# Programs
## modbusmq

The service. It sits between one Modbus bus and one MQTT broker and translates in
both directions, driven entirely by a `.config` file.

```
                       ┌────────────────────┐
   published topics ◄───┤                    ├───► reads:  functions 01/02/03/04
                       │      modbusmq      │
   command topics ────►┤                    ├───► writes: functions 05/06/16
                       └────────────────────┘
      MQTT broker         my_device.config        Modbus TCP host, or an
                                                  RS-485 line and its slaves
```

**Outbound.** Each `input.N` block is one Modbus read repeated on its own interval —
input registers, holding registers, coils or discrete inputs. Each channel under it
picks a value out of the response, applies byte order and scaling to get engineering
units, and publishes it to its topic. Publishing can be throttled per channel:
on-change only, a deadband, a minimum interval, a heartbeat.

**Inbound.** Each `write.N` block subscribes to a topic. A value arriving there is
scaled back into raw register form with the same encoder the read path uses in
reverse, then written to the device — single coil (05), single register (06) or a
register pair (16). Writes carry their own slave and address, because the register
you set a value in is rarely the one you read it back from. They are fire and
forget: failures are logged and reported through the error callback, nothing retries.

**Scope of one process.** One config, one bus, one broker, and as many slaves on
that bus as it has. Several buses means several processes with a config each. There
are no threads — reads, writes and timers all run through one cooperative state
machine, so timing is predictable and there is nothing to race.

It reconnects on both sides on its own, and because a broker forgets subscriptions
across a reconnect, it re-subscribes every write topic when the MQTT link comes back.

Supporting a new meter, battery or inverter is a `.config` file. No C.

**Setting one up: [MODBUSMQ.md](MODBUSMQ.md).**

```
./modbusmq -c accuvim_ii.config
./modbusmq -c polarium.config
```

### Overriding config keys with -e

`-e key=value` replaces one config key for a single run, using the same key names
the file uses. Repeatable. The substitution happens as the file is read, so the
override lands at the point the key appears rather than after the parse.

```
# run a tcp config over rtu, without editing or copying the file
./modbusmq -c accuvim_ii.config -e modbusmq.connect=rtu:///dev/ttyUSB0:9600:1:8:N

# read every input the file defines, not just the first input.max of them
./modbusmq -c shoto.config -e input.max=9
```

An override edits a line the config already has; it does not add one the config
is missing. An override that matched nothing is reported and the run refused, so
a mistyped key stops instead of quietly running against the config's own device.

## modbusmq_query

Simple one-shot Modbus request tool.
I used it all the time to find the slave-id of devices. 

Examples:
```
tcp = tcp://192.168.1.50:502
rtu = rtu:///dev/ttyUSB-RS485_4:9600:1:8:N
Slave_id=1
read input registers
addr=0x00
number of values = 4
2 byte output
verbose
./modbusmq_query tcp://192.168.1.50:502 1 input_register 0x00 4 -2 -v
```

It also writes. `--write` takes the value in engineering units and `--mod`/`--mul`/`--add`
undo the scaling exactly as a config channel would, using the same encoder
`modbusmq` uses — so what you check here is what production will send.

```
# 25.5 degC into a signed register holding tenths of a degree
./modbusmq_query tcp://192.168.1.50:502 39 holding_register 0x0028 --write 25.5 --format int_ab --mod -10

# switch a coil on (function 05)
./modbusmq_query tcp://192.168.1.50:502 39 coil 0x000C --write 1
```

`--dry-run` prints the frame and sends nothing. It needs no device at all, so the
scaling in a config can be checked from a desk before anyone stands next to the
equipment:

```
$ ./modbusmq_query rtu:///dev/ttyUSB0:9600:1:8:N 39 holding_register 0x0028 \
      --write -5.0 --format int_ab --mod -10 --dry-run
write: slave 39 reg 0x0028 = -5 (raw 0xFFCE, function 06)
dry run, nothing sent. Frame body (no transaction id or CRC yet):
  27 06 00 28 FF CE
```

Ideal for connectivity tests and low-level debugging.

## modbusmq_server

Virtual Modbus device emulator.

Reads a *.config file and exposes a Modbus TCP/RTU endpoint that always returns the configured default values.

Future idea: have modbusmq_server subscribe to an MQTT feed (or watch a file) and serve back live/updating values instead of static config defaults — useful for testing modbusmq against a scenario that changes over time instead of a fixed snapshot.

Example:
```
./modbusmq_server -c shoto.config &
# Use -v if you want to know some details about the request/response
# -e points the config at the virtual server without editing or copying it
./modbusmq -c ../config/shoto.config -e modbusmq.connect=tcp://localhost:1502 -v
```

Sample `-v` output (one battery module shown, trimmed):
```
info: writer: xmit=12 length=12 [00][01][00][00][00][06][27][04][0F][FF][00][27]
info: reader: xmit=87 length=87 <00><01>...<00>
info: battery/module/1/voltage=48.00
info: battery/module/1/current=-50.0
info: battery/module/1/remaining_capacity=80.0
info: battery/module/1/soc=8.00
info: battery/module/1/soh=1.00
...and so on for each configured channel, repeated per battery module.
```

You can point any Modbus client or modbusmq at this virtual server to test data flows.

# Contributing

Contributions are welcome.

I am sure there are bugs that need fixing! I have really only tested input registers and holding registers with live batteries and meters.

# License

Licensed under the Apache License 2.0 — see [LICENSE](LICENSE).
