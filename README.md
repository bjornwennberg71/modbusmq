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

The origin of **modbusmq** came from a practical requirement: I needed a reliable way to query a wide range of batteries, meters, and industrial devices—such as Polarium battery strings, Shoto racks, and Accuvim II power meters to new a few from the top of my head. Since most of these devices expose Modbus registers, the natural starting point was the excellent open-source `libmodbus` library from Stephane Raimbault (https://github.com/stephane/libmodbus).

However, several challenges quickly became apparent:

1. **Multiple adapters, one process**  
   Deployments often require communication with several devices simultaneously. Managing one thread per device is overkill.

2. **Straightforward MQTT integration**  
   Device data needed to feed directly into monitoring dashboards and energy management systems. A clean translation layer from Modbus registers to MQTT topics was essential.

3. **Configuration-driven behavior**  
   New devices should be supported without recompiling code. Human-readable `.config` files defining registers, scaling, types, and defaults were a natural solution.

These requirements led to the design of a **single-threaded, event-driven state machine** for Modbus operations. Instead of blocking per request or relying on worker threads, all reads and writes are scheduled cooperatively through a shared FSM that behaves the same for both TCP and RTU. This approach eliminates race conditions, simplifies concurrency, and delivers predictable timing.

As the design evolved, it became clear that the scheduler and Modbus abstraction formed a reusable component—now the core **libmodbusmq** library. On top of this library, a set of standalone tools was built:

- `modbus_query` — simple direct Modbus operations  
- `modbus_subscribe` — subscription-based reading using `.config` files  
- `modbus_server` — a virtual Modbus device serving configurable default values  
- I have several other programs I might add to the system in the near future time permitting.
The result is a lightweight, extensible system that can communicate with multiple devices, over multiple adapters, without multithreading, while remaining easy to deploy, configure, and maintain.

---

## Features

### Library (`libmodbusmq`)
- Unified Modbus TCP/RTU API  
- Event-driven async read/write  
- Synchronous support for backward compability
- Flexible subscription mechanism  
- Clean C API with full documentation  
- Suitable for embedded gateways and constrained environments  
- Connect string for TCP and RTU

### Tools
- **modbus_subscribe** — reads `.config` files, polls devices, optionally publishes to MQTT  
- **modbus_query** — one-shot tool for direct Modbus queries  
- **modbus_server** — virtual Modbus device emulator returning `.config` default values  

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
├── programs/modbus_subscribe.c # Subscription-driven polling tool
├── programs/modbus_query.c # One-shot query tool
├── programs/modbus_server.cpp # Virtual Modbus device emulator
├── config/*.config # Device configuration examples

```

---

## Building

### Build everything with mqtt support
```bash
mkdir -p build && (cd build && cmake .. )
(cd build && make install)
```

### Build everything without mqtt support
```bash
mkdir -p build && (cd build && cmake .. -DMQTT_ENABLED=OFF ..)
(cd build && make install)
```

Build output includes:
```
lib/libmodbusmq.a
bin/modbus_subscribe
bin/modbus_query
bin/modbus_server
config/*.config
```


# Programs
## modbus_subscribe

Continuous Modbus polling with .config files.

Examples:
```
./modbus_subscribe -c accuvim_ii.config
./modbus_subscribe -c polarium.config
```

Used for production data acquisition and optional MQTT output.

## modbus_query

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
./modbus_query tcp://192.168.1.50:502 1 input_register 0x00 4 -2 -v
```

Ideal for connectivity tests and low-level debugging.

## modbus_server

Virtual Modbus device emulator.

Reads a *.config file and exposes a Modbus TCP/RTU endpoint that always returns the configured default values.

Easy to modify in such a way that the modbus_server can read from a mqtt-server the values to feed to the client.

Example:
```
./modbus_server -c shoto.config &
# Use -v if you want to know some details about the request/response
./modbus_subscribe -c ../config/shoto.config -v
mqtt device = localhost
mqtt port   = 6660
20251214 21:40:26:944:info: writer: xmit=12 length=12 [00][01][00][00][00][06][27][04][0F][FF][00][27]
20251214 21:40:26:945:info: reader: xmit=87 length=87 <00><01><00><00><00><51><27><04><4E><12><C0><29><04><03><20><01><B2><FF><E8><00><00><00><00><00><00><03><20><00><64><03><20><00><78><00><0D><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00>
20251214 21:40:26:945:info: battery/module/1/voltage=48.000 
20251214 21:40:26:945:info: battery/module/1/current=-50.000 
20251214 21:40:26:945:info: battery/module/1/remaining_capacity=80.000 
20251214 21:40:26:945:info: battery/module/1/avg_cell_temp=3.400 
20251214 21:40:26:945:info: battery/module/1/env_temp=6511.200 
20251214 21:40:26:945:info: battery/module/1/warning_flag=0.000 
20251214 21:40:26:945:info: battery/module/1/protection_flag=0.000 
20251214 21:40:26:945:info: battery/module/1/fault_status=0.000 
20251214 21:40:26:945:info: battery/module/1/soc=8.000 
20251214 21:40:26:945:info: battery/module/1/soh=1.000 
20251214 21:40:26:945:info: battery/module/1/full_capacity=80.000 
20251214 21:40:26:945:info: battery/module/1/cycle_count=120.000 
20251214 21:40:26:945:info: battery/module/1/num_cells=13.000 

and so on for the other batteries: (omitting debug print of query here)
(The rest of the config for these batteries does not have default values)

20251214 09:10:02:268:info: battery/module/2/voltage=0.000 
20251214 09:10:02:268:info: battery/module/2/current=1000.000 
20251214 09:10:02:268:info: battery/module/2/remaining_capacity=0.000 
20251214 09:10:02:268:info: battery/module/2/avg_cell_temp=-40.000 
20251214 09:10:02:268:info: battery/module/2/env_temp=-40.000 
20251214 09:10:02:268:info: battery/module/2/warning_flag=0.000 
20251214 09:10:02:268:info: battery/module/2/protection_flag=0.000 
20251214 09:10:02:268:info: battery/module/2/fault_status=0.000 
20251214 09:10:02:268:info: battery/module/2/soc=0.000 
20251214 09:10:02:268:info: battery/module/2/soh=0.000 
20251214 09:10:02:268:info: battery/module/2/full_capacity=0.000 
20251214 09:10:02:268:info: battery/module/2/cycle_count=0.000 
20251214 09:10:02:268:info: battery/module/2/num_cells=0.000 
20251214 09:10:03:268:info: battery/module/3/voltage=0.000 
20251214 09:10:03:268:info: battery/module/3/current=1000.000 
20251214 09:10:03:268:info: battery/module/3/remaining_capacity=0.000 
20251214 09:10:03:268:info: battery/module/3/avg_cell_temp=-40.000 
20251214 09:10:03:268:info: battery/module/3/env_temp=-40.000 
20251214 09:10:03:268:info: battery/module/3/warning_flag=0.000 
20251214 09:10:03:268:info: battery/module/3/protection_flag=0.000 
20251214 09:10:03:268:info: battery/module/3/fault_status=0.000 
20251214 09:10:03:268:info: battery/module/3/soc=0.000 
20251214 09:10:03:268:info: battery/module/3/soh=0.000 
20251214 09:10:03:268:info: battery/module/3/full_capacity=0.000 
20251214 09:10:03:268:info: battery/module/3/cycle_count=0.000 
20251214 09:10:03:268:info: battery/module/3/num_cells=0.000 
```

You can point any Modbus client or modbus_subscribe at this virtual server to test data flows.

# Contributing

Contributions are welcome.

I am sure there are bugs that needs fixing! I have really only tested input registers and holding registers with live batteries and meters.
