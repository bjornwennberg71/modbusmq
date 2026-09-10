# Tests

```bash
(cd debug && make check)
```

or directly, against either tree:

```bash
bash test/run_tests.sh debug
bash test/run_tests.sh release
```

The runner boots a `modbusmq_server` on `test.config` — a fixture whose every
channel carries a `value =` default — and reads those values back through the
real code path. Exit status is 0 only if every check passed.

| file | what it covers | needs a server |
| --- | --- | --- |
| `test_scaling.c` | decoders and encoders and their round trips, connect string parsing, `modbusmq_strerror`, `modbusmq_channel_format_value`, the publish policy, `modbusmq_write_encode` | no |
| `test_read.c` | all four input types end to end: framing, transport, response validation, byte order, scaling | yes |
| `test_loop.c` | `modbusmq_loop_prepare`/`modbusmq_loop_write_read`, subscriptions and callbacks, one-shot posts, the `sleep_time` in/out contract, teardown | yes |
| `run_tests.sh` | the above, plus the command line: `modbusmq_query` round trip, `--version` agreeing with the header, and `-e` override behaviour | — |

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
