# Tests

```bash
(cd debug && make check)
```

or directly, against either tree:

```bash
bash test/run_tests.sh debug
bash test/run_tests.sh release
```

## Both MQTT configurations

`MQTT_ENABLED` is a compile-time flag applied to the whole tree, so MQTT on and
off are genuinely different builds and both need testing. `libmodbusmq` never
references mosquitto and has to pass either way; `modbusmq` needs MQTT on for
the publish path to be compiled at all.

```bash
(cd debug && make check-all)     # or: bash test/run_matrix.sh
```

This configures two throwaway build directories, so whatever `debug/` and
`release/` are set to is left alone. It fails on a compiler warning as well as
on a failed check — the tree builds clean under `-Wall -Wextra` and the point is
to keep it that way. If `libmosquitto` is not installed, the MQTT-on half is
skipped rather than failed.

The runner boots a `modbusmq_server` on `test.config` — a fixture whose every
channel carries a `value =` default — and reads those values back through the
real code path. Exit status is 0 only if every check passed.

| file | what it covers | needs a server |
| --- | --- | --- |
| `test_scaling.c` | decoders and encoders and their round trips, connect string parsing, `modbusmq_strerror`, `modbusmq_channel_format_value`, the publish policy, `modbusmq_write_encode` | no |
| `test_read.c` | all four input types end to end: framing, transport, response validation, byte order, scaling | yes |
| `test_loop.c` | `modbusmq_loop_prepare`/`modbusmq_loop_write_read`, subscriptions and callbacks, one-shot posts, the `sleep_time` in/out contract, teardown | yes |
| `run_tests.sh` | the above, plus the command line: `modbusmq_query` round trip, `--version` agreeing with the header, and `-e` override behaviour | — |
| `run_matrix.sh` | builds and runs all of it with `MQTT_ENABLED` off and on, failing on any compiler warning | — |

`test_common.h` is the whole framework: four `CHECK_*` macros and a tally. A
test program prints its own failures and returns non-zero if it had any.

## Adding a test

Add the `.c` file, then its name to the `foreach()` in `CMakeLists.txt` and to
`run_tests.sh`. A program taking a config file gets `test.config` as `argv[1]`.

## Changing the fixture

The expected values in `test_read.c` are hardcoded and correspond to the
`value =` defaults in `test.config`. Change one and the other must follow —
that coupling is deliberate, so a fixture edit cannot quietly weaken a test.

Port 15502 rather than the usual 1502, so a run does not collide with a virtual
server someone left going.
