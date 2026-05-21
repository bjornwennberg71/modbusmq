# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

The project uses pre-configured `debug/` and `release/` build directories. To set them up from scratch:

```bash
bash setup.sh
```

To build (debug):
```bash
cd debug && make
```

To build (release):
```bash
cd release && make
```

To build with MQTT support (links against `libmosquitto`):
```bash
cd debug && cmake -DMQTT_ENABLED=ON .. && make
```

To install (into `debug/modbus2MQTT/` or `release/modbus2MQTT/`):
```bash
cd debug && make install
```

Built artifacts: `lib/libmodbusmq.so`, `programs/modbusmq_query`, `programs/modbusmq_subscribe`, `programs/modbusmq_server`.

## Architecture

`modbusmq` is a single-threaded, event-driven Modbus TCP/RTU library. The core design avoids blocking and threading by using a cooperative FSM that handles both TCP and RTU transports identically.

### Library (`lib/`)

- **`modbusmq_context_t`** (`modbusmq_internal.h`) — central state object holding the socket/fd, queued messages, subscription timers, transport callbacks (`modbusmq_cb_t`), and registered user callbacks.
- **`modbusmq_cb_t`** — vtable of function pointers populated by either `modbusmq_tcp_context()` or `modbusmq_rtu_context()`. All transport-specific behavior (framing, CRC, header parsing) lives in `modbusmq_tcp.c` / `modbusmq_rtu.c` and is accessed through this table.
- **`modbusmq_msg_t`** — a request/response pair. `frame[0]` is the writer (request), `frame[1]` is the reader (response). A frame holds a raw byte buffer plus transmit state.
- **Message queue** (`msg_wrapper_head`) — one-shot messages posted via `modbusmq_post()` or `modbusmq_send()`.
- **Timer queue** (`timer_head`) — repeating subscriptions registered via `modbusmq_subscribe()`. `modbusmq_loop_prepare()` returns the next sleep time; `modbusmq_loop_write_read()` advances the FSM on I/O events.
- **`modbusmq_config_t`** (`modbusmq_config.h`) — parsed representation of a `.config` file. Contains connection string, list of `modbusmq_input_t` subscriptions, each with a list of `modbusmq_channel_t` channels describing register offsets, data format, scaling (`mod`, `mul`, `add`), and MQTT topic.

### Programs (`programs/`)

- **`modbusmq_query`** — one-shot CLI query; parses connect string, builds a single request, calls `modbusmq_send()` (blocking), prints raw register values.
- **`modbusmq_subscribe`** — continuous polling from a `.config` file; calls `modbusmq_subscribe()` for each input, then drives the event loop. Optionally publishes to MQTT via `libmosquitto` when `MQTT_ENABLED=1`.
- **`modbusmq_server`** — virtual Modbus TCP/RTU device; serves `.config` default values to any connecting client.

### Config files (`config/`)

Plain key=value format. Key sections:
- `modbus.connect` — TCP (`tcp://host:port`) or RTU (`rtu:///dev/ttyUSBx:baud:parity:data:stop`) connect string.
- `input.max` / `input.query_mode` — number of inputs and query ordering (`0`=parallel, `1`=series).
- `input.N.*` — per-device slave ID, register type (`input_register` / `holding_register`), base address, count, poll interval (ms).
- `input.N.channel.M.*` — per-channel register offset, data format (`int_ab`, `int_ba`, `int_abcd`, `float_abcd`, etc.), scaling (`mod` = divide by abs(mod), `mul`, `add`), MQTT topic, and optional default `value`.
- `mqtt.*` — MQTT broker address and topic prefix.

### Data format / scaling convention

`mod` is overloaded: a negative value means divide (e.g. `mod=-100` → `value/100`), a positive value means multiply. `mul=-1` inverts sign. `add` is applied after scaling. See `modbusmq_read_channel()` in `modbusmq.c` for the exact computation.

## Connect String Format

```
tcp://192.168.1.50:502
rtu:///dev/ttyUSB1:9600:N:8:1
```

Parsed by `modbusmq_parse_connect_string()` into `modbusmq_connect_t`.

## Testing with the Virtual Server

```bash
# Start virtual server on localhost:1502 using a config's default values
./debug/programs/modbusmq_server -c config/shoto.config &

# Subscribe against it
./debug/programs/modbusmq_subscribe -c config/shoto.config -v
```
