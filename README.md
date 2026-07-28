<p align="center">
  <img src="images/modbusmq_small.png" alt="modbusmq logo" width="600">
</p>

# modbusmq
High-performance Modbus client library and MQTT integration toolkit.

`modbusmq` is a lightweight, event-driven Modbus TCP/RTU client library accompanied by a set of practical tools for industrial energy systems, monitoring solutions, and embedded gateways.

It provides:

- **libmodbusmq** — a modular and efficient C library for Modbus communications  
- **A subscription engine** for periodic polling defined using `.config` files  
- **Standalone programs** for querying devices, running subscription pipelines, or hosting a virtual Modbus server  
- **Device configuration examples** for Accuvim II, Polarium, and Shoto battery systems

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
- `modbusmq_subscribe` — subscription-based reading using `.config` files  
- `modbusmq_server` — a virtual Modbus device serving configurable default values  
- I have several other programs I might add to the system in the near future time permitting.
The result is a lightweight, extensible system that can communicate with multiple devices, over multiple adapters, without multithreading, while remaining easy to deploy, configure, and maintain.

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
- **modbusmq_subscribe** — reads `.config` files, polls devices, optionally publishes to MQTT  
- **modbusmq_query** — one-shot tool for direct Modbus queries  
- **modbusmq_server** — virtual Modbus device emulator returning `.config` default values  

### Configuration
`.config` files describe:
- Device connection (TCP or RTU)  
- Registers/coils to read  
- Scaling, formatting, and units  
- Default values  
- Optional MQTT topic mapping  

Example configs included:
- `accuvim_ii.config`  
- `polarium.config`  
- `shoto.config`

---

## Repository Structure
```
modbusmq/
├── lib/ # Core C library
├── programs/modbusmq_subscribe.c # Subscription-driven polling tool
├── programs/modbusmq_query.c # One-shot query tool
├── programs/modbusmq_server.c # Virtual Modbus device emulator
├── config/*.config # Device configuration examples

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
programs/modbusmq_subscribe
programs/modbusmq_query
programs/modbusmq_server
config/*.config
```


# Programs
## modbusmq_subscribe

Continuous Modbus polling with .config files.

Examples:
```
./modbusmq_subscribe -c accuvim_ii.config
./modbusmq_subscribe -c polarium.config
```

Used for production data acquisition and optional MQTT output.

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

Ideal for connectivity tests and low-level debugging.

## modbusmq_server

Virtual Modbus device emulator.

Reads a *.config file and exposes a Modbus TCP/RTU endpoint that always returns the configured default values.

Future idea: have modbusmq_server subscribe to an MQTT feed (or watch a file) and serve back live/updating values instead of static config defaults — useful for testing modbusmq_subscribe against a scenario that changes over time instead of a fixed snapshot.

Example:
```
./modbusmq_server -c shoto.config &
# Use -v if you want to know some details about the request/response
./modbusmq_subscribe -c ../config/shoto.config -v
```

Sample `-v` output (one battery module shown, trimmed):
```
info: writer: xmit=12 length=12 [00][01][00][00][00][06][27][04][0F][FF][00][27]
info: reader: xmit=87 length=87 <00><01>...<00>
info: battery/module/1/voltage=48.000
info: battery/module/1/current=-50.000
info: battery/module/1/remaining_capacity=80.000
info: battery/module/1/soc=8.000
info: battery/module/1/soh=1.000
...and so on for each configured channel, repeated per battery module.
```

You can point any Modbus client or modbusmq_subscribe at this virtual server to test data flows.

# Contributing

Contributions are welcome.

I am sure there are bugs that need fixing! I have really only tested input registers and holding registers with live batteries and meters.

# License

Licensed under the Apache License 2.0 — see [LICENSE](LICENSE).
