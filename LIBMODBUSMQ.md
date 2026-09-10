# libmodbusmq

The Modbus TCP/RTU library underneath `modbusmq`. Single-threaded, event-driven,
and free of any I/O of its own: it never blocks, never spawns a thread, and never
calls `poll()` behind your back. You own the loop. The library tells you which
descriptor to watch, what to watch it for, and how long to sleep — you call
`poll()` (or `select()`, or whatever your framework already runs) and hand the
result back.

That is the whole design. It exists so a gateway can talk to several devices over
several adapters, on one thread, in a program that is also doing other things.

`modbusmq.h` and `modbusmq_time.h` are the only headers installed, and a
`modbusmq_context_t *` is opaque, so the internals can move without breaking you.

TCP and RTU go through the same state machine. Framing, CRC and header parsing
live behind a vtable chosen when you create the context, so with the exception of
two RTU-only tuning calls, every function below behaves identically on both.

**[MODBUSMQ.md](MODBUSMQ.md)** is the guide to the `modbusmq` program.
**[config/CONFIG.md](config/CONFIG.md)** is the config key reference.

---

## 1. Build and link

```bash
bash setup.sh
(cd release && make && make install)
```

That leaves `libmodbusmq.so.<version>` with the usual `libmodbusmq.so` and
`libmodbusmq.so.<major>` symlinks, plus `modbusmq.h` and `modbusmq_time.h` in the
install tree's `include/`.

```bash
cc myprog.c -lmodbusmq -o myprog
```

`SOVERSION` is the major version, so you link against `libmodbusmq.so.2` and a
minor or build bump does not force a relink.

```c
#include "modbusmq.h"
```

Everything is `extern "C"`-guarded, so C++ callers need nothing extra.

---

## 2. A context, and a connection

A context is one connection to one bus. Build it from a connect string, or from
the RTU line parameters directly:

```c
struct modbusmq_context_t *context = modbusmq_tcp_context("tcp://192.168.1.50:502");

// or
struct modbusmq_context_t *context =
    modbusmq_rtu_context("/dev/ttyUSB0", 9600, 'N', 8, 1);   // device, baud, parity, databits, stopbit
```

If you are taking the connect string from a config file or a command line, let
the library parse it and pick the transport for you:

```c
modbusmq_connect_t connect;

if (modbusmq_parse_connect_string(argv[1], &connect) != 0)
{
    fprintf(stderr, "bad connect string\n");
    return -1;
}

struct modbusmq_context_t *context =
    (connect.connect_type == MODBUSMQ_CONNECT_TCP)
        ? modbusmq_tcp_context(argv[1])
        : modbusmq_rtu_context(connect.device, connect.baudrate, connect.parity,
                               connect.databits, connect.stopbit);
```

Note the RTU field order in the string is `baud:stopbit:databits:parity` — parity
last, and the first three are read numerically, so putting parity earlier fails
the parse. The argument order of `modbusmq_rtu_context()` is *not* the same as
the string's: parity is the third argument. Take the fields off
`modbusmq_connect_t` rather than counting positions by hand.

Then connect, and say which slave you are addressing:

```c
if (modbusmq_connect(context) != 0)
{
    fprintf(stderr, "unable to connect\n");
    return -1;
}

modbusmq_set_slave(context, 39);
```

`modbusmq_set_slave()` applies to frames you build *after* it. On a shared RTU
bus with several slaves, set it again before building each device's request.

`modbusmq_free()` releases the context; `modbusmq_close()` drops the connection
but keeps the context, which is what you want in a reconnect loop.

---

## 3. The smallest useful program

One request, wait for the answer, print it. `modbusmq_send()` is the blocking
convenience path — it runs the same state machine internally until the
transaction completes or your timeout expires.

```c
modbusmq_msg_t msg;
memset(&msg, 0, sizeof(msg));

modbusmq_frame_read_input_registers(context, &msg.frame[0], 0x0000, 4);

int rc = modbusmq_send(context, &msg, 2000);   // wait up to 2000 ms
```

`modbusmq_send()` returns **0** for a complete, validated response, **> 0** when
the wait expired without one, and **< 0** on error. A timeout is not an error
here — it is an ordinary outcome on a bus where a device is slow or absent, so
check for it explicitly rather than testing `rc != 0`.

```c
if (rc > 0)
{
    fprintf(stderr, "no response within 2000 ms\n");
}
else if (rc < 0)
{
    fprintf(stderr, "send failed: rc=%d\n", rc);
}
else
{
    uint8_t *data   = modbusmq_frame_data(context,   &msg.frame[1]);
    int      nbytes = modbusmq_frame_nbytes(context, &msg.frame[1]);

    for(int i = 0; i + 1 < nbytes; i += 2)
    {
        printf("%d\n", modbusmq_read_int16_ab(data + i));
    }
}
```

**`frame[0]` is the request, `frame[1]` is the response.** Zero the message,
build into `frame[0]`, read out of `frame[1]`. The builder marks the frame as the
writer for you, so you do not set `is_writer` yourself.

`modbusmq_send()` is fine for a one-shot tool. For anything that has to stay
responsive while it waits, use the loop instead — that is section 5.

---

## 4. Building requests

Each builder writes one request into a frame and returns the frame length, or
`< 0` on error. `addr` is the Modbus address; `naddr` counts registers, `nbits`
counts coils.

```c
// reads
modbusmq_frame_read_input_registers(context,   &msg.frame[0], addr, naddr);  // fn 04
modbusmq_frame_read_holding_registers(context, &msg.frame[0], addr, naddr);  // fn 03
modbusmq_frame_read_coil_bits(context,         &msg.frame[0], addr, nbits);  // fn 01
modbusmq_frame_read_input_bits(context,        &msg.frame[0], addr, nbits);  // fn 02

// writes
modbusmq_frame_write_register(context,       &msg.frame[0], addr, value);            // fn 06
modbusmq_frame_write_registers(context,      &msg.frame[0], addr, naddr, values);    // fn 16
modbusmq_frame_write_coil_bit(context,       &msg.frame[0], addr, on);               // fn 05
modbusmq_frame_write_coil_bits(context,      &msg.frame[0], addr, nbits, bits);      // fn 15
modbusmq_frame_write_mask_registers(context, &msg.frame[0], addr, and_mask, or_mask);// fn 22
```

Two details worth knowing before you hit them:

- `modbusmq_frame_write_coil_bit()` takes `value` as a boolean and encodes the
  protocol's `0xFF00` / `0x0000` itself. Pass 1, not 0xFF00.
- `modbusmq_frame_write_coil_bits()` takes **one byte per coil**, non-zero
  meaning on — not packed bits. `nbits` may be 1..1968. The library packs them.

For reads of bit types, the device answers with the coils packed eight to a byte;
`naddr`/`nbits` is a coil count, not a byte count.

---

## 5. The event loop

This is the part that matters, and the reason the library exists.

Two calls drive it. `modbusmq_loop_prepare()` tells you what to wait for:
it returns the file descriptor (or `< 0` if the connection is gone), fills in how
long you may sleep, and sets the poll events the library currently wants.
`modbusmq_loop_write_read()` takes the `revents` you got back and advances the
state machine — writing the pending request, or reading the response, as the
descriptor allows.

```c
while(1)
{
    struct pollfd pollfds[4];
    millitime_t   millisleep = 1000;      // your own default ceiling
    int           nfds = 0;

    memset(pollfds, 0, sizeof(pollfds));

    int fd = modbusmq_loop_prepare(context, &millisleep, &pollfds[nfds].events);
    if (fd < 0)
    {
        // connection is gone — reconnect, see section 7
        break;
    }

    pollfds[nfds].fd      = fd;
    pollfds[nfds].events |= POLLERR | POLLHUP;
    nfds++;

    // ... add your own descriptors here; they are just more entries ...

    if (poll(pollfds, nfds, millisleep) < 0)
    {
        break;
    }

    if (pollfds[0].revents & (POLLIN | POLLOUT | POLLERR | POLLHUP))
    {
        int rc = modbusmq_loop_write_read(context, pollfds[0].revents);
        // see section 7 for what rc means
    }
}
```

`millisleep` is the point of the whole arrangement: it comes back as the time
until the next subscription is due, so an idle poller sleeps exactly as long as
it should and no longer. Lower it afterwards if one of your own timers is due
sooner — never raise it.

It is a pure **out** parameter, despite looking like an in/out one: whatever you
store there before the call is discarded, and the library writes its own answer
over it, capped at its internal 1000 ms ceiling. Initialising it to 1000 first,
as the loop above does, changes nothing — it just happens to match.

Nothing here is exclusive. Your MQTT socket, your control socket, your timer
descriptor all go into the same `pollfds` array. The library asks only that it
gets its descriptor watched and its `revents` handed back.

---

## 6. Subscriptions and callbacks

A subscription is a request repeated on an interval. Register the message once
and the library re-issues it forever, calling you back with each response.

Subscriptions need a config on the context first — `modbusmq_subscribe()` refuses
without one, because the subscription callback hands you back the
`modbusmq_input_t` that produced the response:

```c
modbusmq_set_config(context, modbusmq_config_get());
```

Then register the callbacks and the subscriptions:

```c
void on_subscription(struct modbusmq_context_t *context,
                     modbusmq_msg_t *msg,
                     struct modbusmq_input_t *input)
{
    uint8_t *data   = modbusmq_frame_data(context,   &msg->frame[1]);
    int      nbytes = modbusmq_frame_nbytes(context, &msg->frame[1]);
    // decode and do something with it
}

modbusmq_set_subscription_callback(context, on_subscription);

modbusmq_msg_t msg;
memset(&msg, 0, sizeof(msg));

modbusmq_set_slave(context, 39);
modbusmq_frame_read_input_registers(context, &msg.frame[0], 0x000F, 7);

modbusmq_subscribe(context, &msg, 2000);   // every 2000 ms
```

**The message is copied.** Both `modbusmq_subscribe()` and `modbusmq_post()`
`memcpy` it into their own queue entry, so a stack `modbusmq_msg_t` reused in a
loop is fine — build, register, `memset`, build the next one.

Three callbacks are available:

| callback | fires on |
| --- | --- |
| `modbusmq_set_message_callback()` | a one-shot posted with `modbusmq_post()` completing |
| `modbusmq_set_subscription_callback()` | a subscription's response arriving |
| `modbusmq_set_error_callback()` | a request being given up on |

`modbusmq_post()` queues a one-shot alongside the subscriptions — that is how a
command arriving from elsewhere becomes a write on the bus without disturbing the
polling. It is fire-and-forget: it returns once the message is queued, not once
it has been sent.

Every message carries a `req_id`, stamped by the library and never reused, so a
subscription polling the same slave every two seconds gets a fresh one per poll.
On RTU there is no transaction id on the wire, so this is a local correlation id
— it is what lets you tie an error on a shared bus back to the device that caused
it. `modbusmq_msg_tag()` formats the `[req 4711 slave 3 addr 0x01F4]` prefix that
every error line in the library carries. It returns a pointer to a static buffer:
fine in a single-threaded library, but never use it twice in one `printf`.

---

## 7. Errors, and which ones are fatal

`modbusmq_loop_write_read()` returns 0, or one of three codes. **Telling them
apart is the single most important thing to get right in a long-running
program**, because the obvious reflex — tear down the connection on any error —
is wrong for two of the three.

```c
int rc = modbusmq_loop_write_read(context, pollfds[0].revents);

if (rc == MODBUSMQ_ERR_PROTOCOL)
{
    // A frame was rejected: bad slave id, function, byte count or CRC. The
    // library has already discarded it and resynced the stream. The connection
    // is FINE. Log it and keep going — the next queued request starts from a
    // clean stream. Reconnecting here would throw away every other
    // subscription's progress over one bad frame.
    log_it(rc);
}
else if (rc < 0)
{
    // MODBUSMQ_ERR_TRANSPORT: the connection itself is gone.
    modbusmq_close(context);
    modbusmq_reset_queue(context);
    reconnect_later();
}
```

`MODBUSMQ_ERR_TIMEOUT` never comes back from the loop functions at all. A request
that went out and got nothing back within the frame timeout is simply dropped and
the queue moves on; you hear about it through the error callback only.

`modbusmq_reset_queue()` on a lost connection matters: it clears requests that
were mid-flight so the reconnected stream does not start by trying to finish a
transaction the device never heard.

The error callback gives you the failed pair and the code:

```c
void on_error(struct modbusmq_context_t *context, modbusmq_msg_t *msg, int error)
{
    fprintf(stderr, "%s error=%d\n", modbusmq_msg_tag(context, msg), error);
}
```

`msg` is owned by the library and is freed as soon as the callback returns. Copy
anything that must outlive it.

Do not reach for `modbusmq_strerror()` here. Despite the name it is a thin
wrapper over `strerror(3)` and knows nothing about the `MODBUSMQ_ERR_*` codes —
handing it one produces `"Unknown error -1"`. It is for an `errno` you picked up
alongside a failure, not for the library's own return codes. Map those yourself,
or just print the number.

---

## 8. Decoding values

The raw readers take a pointer into the response payload:

```c
int   modbusmq_read_int16_ab(const uint8_t *data);        // unsigned, big endian
int   modbusmq_read_int16_ba(const uint8_t *data);        // unsigned, byte swapped
int   modbusmq_read_int16_ab_signed(const uint8_t *data); // two's complement
int   modbusmq_read_int16_ba_signed(const uint8_t *data);
int   modbusmq_read_int32_abcd(const uint8_t *data);
int   modbusmq_read_int32_badc(const uint8_t *data);
float modbusmq_read_float_abcd(const uint8_t *data);
float modbusmq_read_float_badc(const uint8_t *data);
float modbusmq_read_float_dcba(const uint8_t *data);
```

The plain `_ab` / `_ba` pair is **unsigned** — a negative reading comes back as a
large positive number. Use the `_signed` variants for anything that can go below
zero, such as a current or a temperature.

If you are driving the library from a config, `modbusmq_read_channel()` does the
whole job for a channel — byte order, then `add`, then `mod`, then `mul` — and
`modbusmq_channel_in_range()` tells you first whether the channel actually lies
inside the data that came back. `modbusmq_channel_format_value()` turns the
result into the text to print or publish, and `modbusmq_channel_publish_decide()`
applies the channel's publish policy. Those four are what the `modbusmq` program
is built out of; a program with its own idea of what to do with a value can
ignore them and read the bytes directly.

On the way out, `modbusmq_write_encode()` is the exact inverse of
`modbusmq_read_channel()`: it undoes a write entry's `add`/`mod`/`mul` and
encodes the result into up to two registers in wire order, rejecting values that
do not fit the format rather than truncating them.

---

## 9. RTU tuning

Two knobs, both RTU-only and both harmless to call on TCP:

```c
modbusmq_rtu_rts_delay(context, 10000);    // microseconds to wait before writing
modbusmq_frame_timeout(context, 1000);     // ms to wait for a frame before giving up
```

`modbusmq_rtu_rts_delay()` is the one to reach for when an RS-485 adapter drops
or mangles frames: it delays the write long enough for the line to settle after
the driver is enabled. The defaults are 10 ms and 1000 ms.

---

## 10. Rules and limits

- **Single-threaded.** One context belongs to one thread, and the library assumes
  it. There is no locking anywhere. Several contexts on several buses in one
  thread is the intended shape; one context shared between threads is not.
- **No I/O of its own.** It never calls `poll()` or sleeps on your behalf — with
  the deliberate exception of `modbusmq_send()`, which is documented as blocking.
- **Messages are copied on queue.** `modbusmq_post()` and `modbusmq_subscribe()`
  take their own copy; a stack `modbusmq_msg_t` is fine.
- **Callback messages are not.** A `modbusmq_msg_t *` handed to a callback dies
  when the callback returns.
- **`modbusmq_msg_tag()` returns a static buffer.** Never twice in one `printf`.
- **Frames cap at `MODBUSMQ_FRAME_MAX` (260 bytes)**, the Modbus maximum.
- **`modbusmq_set_slave()` affects frames built after it**, not the context
  globally in any retroactive sense.
- **Only `modbusmq.h` and `modbusmq_time.h` are installed.** `modbusmq_internal.h`,
  `modbusmq_rtu.h` and `modbusmq_tcp.h` are not part of the API, and the context
  struct is opaque on purpose.

---

## See also

- **[MODBUSMQ.md](MODBUSMQ.md)** — the `modbusmq` program, if a config file will
  do the job instead of C.
- **[config/CONFIG.md](config/CONFIG.md)** — every config key.
- `programs/modbusmq_query.c` — the smallest real consumer, one blocking request.
- `programs/modbusmq.c` — the full event loop, subscriptions, reconnect handling
  and a second (MQTT) descriptor sharing the same `poll()`.
