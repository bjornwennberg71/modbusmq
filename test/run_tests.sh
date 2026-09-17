#!/bin/bash
#
# modbusmq test suite
#
# Boots a modbusmq_server on the fixture config, runs the compiled test
# programs against it, then exercises the command line tools the same way.
#
# usage: bash test/run_tests.sh [build-dir]     (build-dir defaults to debug)
#
# bjornw: 202509
#

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
# accept either an absolute build dir (how the cmake target calls us) or one
# relative to the project root (how a person does)
case "${1:-debug}" in
    /*) BUILD="$1" ;;
    *)  BUILD="$ROOT/${1:-debug}" ;;
esac

if [ ! -d "$BUILD" ]; then
    echo "no build directory $BUILD -- run setup.sh first"
    exit 2
fi

CONFIG="$HERE/test.config"
PORT=15502

SERVER="$BUILD/programs/modbusmq_server"
BRIDGE="$BUILD/programs/modbusmq"
QUERY="$BUILD/programs/modbusmq_query"
LIBDIR="$BUILD/lib"

export LD_LIBRARY_PATH="$LIBDIR:${LD_LIBRARY_PATH:-}"

for f in "$SERVER" "$BRIDGE" "$QUERY"; do
    if [ ! -x "$f" ]; then
        echo "missing $f -- build it first"
        exit 2
    fi
done

TMP=$(mktemp -d)
SERVER_PID=""

cleanup()
{
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

PASS=0
FAIL=0

#
# record the outcome of one test
#
ok()   { PASS=$((PASS+1)); echo "  ok   $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL $1"; }

#
# run a compiled test program; it prints its own detail and returns non-zero
# when any of its checks failed
#
run_program()
{
    local name=$1
    local bin="$BUILD/test/$name"

    if [ ! -x "$bin" ]; then
        bad "$name (not built)"
        return
    fi

    # timeout, not just for tidiness: a regression in the send-timeout clamping
    # would hang rather than fail, and a hung test wedges CI instead of failing it
    timeout 60 "$bin" "$CONFIG" > "$TMP/$name.log" 2>&1
    local rc=$?

    if [ $rc -eq 0 ]; then
        sed 's/^/    /' "$TMP/$name.log" | grep -E "ok:|==" || true
        ok "$name"
        return
    fi

    sed 's/^/    /' "$TMP/$name.log"

    if [ $rc -eq 124 ]; then
        bad "$name (timed out after 60s)"
    else
        bad "$name"
    fi
}

#
# assert a command's output contains a pattern
#
expect_output()
{
    local what=$1 pattern=$2
    shift 2

    if "$@" 2>&1 | grep -qE "$pattern"; then
        ok "$what"
    else
        bad "$what (expected /$pattern/)"
        "$@" 2>&1 | sed 's/^/    /' | head -5
    fi
}

#
# assert a command exits with a given code
#
expect_exit()
{
    local what=$1 want=$2
    shift 2

    "$@" >/dev/null 2>&1
    local got=$?

    if [ "$got" -eq "$want" ]; then
        ok "$what"
    else
        bad "$what (exit $got, wanted $want)"
    fi
}

echo "modbusmq test suite: $(basename "$BUILD")"
echo

#
# 1. pure tests, no server needed
#
echo "unit tests"
run_program test_scaling

#
# 2. start the virtual device
#
"$SERVER" -c "$CONFIG" > "$TMP/server.log" 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 50); do
    if (exec 3<>/dev/tcp/localhost/$PORT) 2>/dev/null; then
        break
    fi
    sleep 0.1
done

if ! (exec 3<>/dev/tcp/localhost/$PORT) 2>/dev/null; then
    echo "  FAIL modbusmq_server never came up on port $PORT"
    sed 's/^/    /' "$TMP/server.log" | head -10
    exit 1
fi
echo "  ok   modbusmq_server listening on $PORT"
PASS=$((PASS+1))
echo

#
# 3. tests that talk to it
#
echo "integration tests"
run_program test_read
run_program test_loop
echo

#
# 4. the command line tools
#
echo "command line"

# modbusmq_query reads the raw register the fixture serves: 4800 at offset 0
expect_output "modbusmq_query reads a register" "4800" \
    "$QUERY" "tcp://localhost:$PORT" 1 input_register 0x00 2 -2

# every program reports the version the header declares
MAJOR=$(grep 'define MODBUSMQ_VERSION_MAJOR' "$ROOT/lib/modbusmq.h" | grep -oE '[0-9]+')
MINOR=$(grep 'define MODBUSMQ_VERSION_MINOR' "$ROOT/lib/modbusmq.h" | grep -oE '[0-9]+$')
BUILDV=$(grep 'define MODBUSMQ_VERSION_BUILD' "$ROOT/lib/modbusmq.h" | grep -oE '[0-9]+$')
WANT="$MAJOR.$MINOR.$BUILDV"

expect_output "modbusmq --version matches the header"        "$WANT" "$BRIDGE" --version
expect_output "modbusmq_query --version matches the header"  "$WANT" "$QUERY"  --version
expect_output "modbusmq_server --version matches the header" "$WANT" "$SERVER" --version

# -e substitutes at the line the key appears on
expect_output "-e overrides a config key" "overridden by -e" \
    timeout 2 "$BRIDGE" -c "$CONFIG" -v -e modbusmq.frame_timeout=1234

# -e naming a key the config lacks warns, and does not stop the run
expect_output "-e with an unknown key warns" "WARNING.*never appears" \
    timeout 2 "$BRIDGE" -c "$CONFIG" -e bug=bug2

# exit 124 means timeout had to kill it, i.e. it was still running
timeout 2 "$BRIDGE" -c "$CONFIG" -e bug=bug2 >/dev/null 2>&1
if [ $? -eq 124 ]; then
    ok "-e with an unknown key keeps running (warning, not error)"
else
    bad "-e with an unknown key stopped the run"
fi

# a malformed -e is an error
expect_exit "malformed -e is rejected" 255 "$BRIDGE" -c "$CONFIG" -e nonsense

# modbusmq_server takes -e as well, which is how an rtu config is tested on tcp
expect_output "modbusmq_server accepts -e" "key=value" "$SERVER" --help

echo
echo "config validation"

#
# Build a one-channel config with $body appended, and assert the bridge refuses
# it with a message that says why.
#
# Whole configs rather than -e overrides: -e only replaces a key the file
# already has, and every case below is a key a working config never contains.
#
bad_config()
{
    local what=$1 pattern=$2 body=$3
    local f="$TMP/bad.config"

    cat > "$f" <<EOF
config.name      = bad
config.version   = 2.0
modbusmq.connect = tcp://localhost:$PORT

input.max = 1
input.1.slave       = 1
input.1.type        = holding_register
input.1.address     = 0x0200
input.1.naddress    = 8
input.1.interval    = 1000
input.1.channel.max = 1
input.1.channel.1.offset = 0
input.1.channel.1.topic  = t/bad
input.1.channel.1.format = uint_ab
$body
EOF

    if timeout 5 "$BRIDGE" -c "$f" 2>&1 | grep -qE "$pattern"; then
        ok "$what"
    else
        bad "$what (expected /$pattern/)"
        timeout 5 "$BRIDGE" -c "$f" 2>&1 | sed 's/^/    /' | head -5
    fi
}

#
# The counterpart: assert a config is ACCEPTED, so the refusals above cannot be
# satisfied by a validator that refuses everything put in front of it.
#
# "Accepted" means it got past validation, which shows up as the bridge trying
# to reach a device rather than complaining about the file. Nothing is
# listening on $PORT+1, and that is the point -- the connect attempt is the
# evidence.
#
good_config()
{
    local what=$1 body=$2
    local f="$TMP/good.config"

    cat > "$f" <<EOF
config.name      = good
config.version   = 2.0
modbusmq.connect = tcp://localhost:$((PORT + 1))

input.max = 1
input.1.slave       = 1
input.1.type        = holding_register
input.1.address     = 0x0200
input.1.naddress    = 8
input.1.interval    = 1000
input.1.channel.max = 1
input.1.channel.1.offset = 0
input.1.channel.1.topic  = t/good
input.1.channel.1.format = uint_ab
$body
EOF

    local out
    out=$(timeout 5 "$BRIDGE" -c "$f" 2>&1)

    if echo "$out" | grep -qE "Unable to parse config-file"; then
        bad "$what (config was refused)"
        echo "$out" | sed 's/^/    /' | head -5
    else
        ok "$what"
    fi
}

# a variable-width text format has no size of its own
bad_config "ascii without a length is rejected" "needs length" \
    "input.1.channel.1.format = ascii_ab"

# scaling has nothing to work on
bad_config "mod on a text channel is rejected" "mod/mul does not apply" \
    "input.1.channel.1.format = ascii_ab
input.1.channel.1.nregisters = 4
input.1.channel.1.mod = -10"

# ...and the reverse: a length on a channel whose format already has one
bad_config "length on a numeric channel is rejected" "applies to a text format only" \
    "input.1.channel.1.nregisters = 4"

# timefmt describes a timestamp, not a serial number
bad_config "timefmt on a string channel is rejected" "time format only" \
    "input.1.channel.1.format = ascii_ab
input.1.channel.1.nregisters = 4
input.1.channel.1.timefmt = %Y"

# a field-per-register clock carries no timezone to convert from
bad_config "timezone on a field datetime is rejected" "epoch format only" \
    "input.1.channel.1.format = datetime_regs
input.1.channel.1.timezone = local"

# min_change is a magnitude gate and a string has no magnitude
bad_config "min_change on a text channel is rejected" "min_change" \
    "input.1.channel.1.format = ascii_ab
input.1.channel.1.nregisters = 4
input.1.channel.1.min_change = 0.5"

# text formats are read-only
bad_config "a text format in a write entry is rejected" "read but not written" \
    "write.max = 1
write.1.slave  = 1
write.1.type   = holding_register
write.1.address = 0x0300
write.1.format = ascii_ab
write.1.topic  = t/set"

#
# Block writes. A block goes out as one function 16 frame covering every
# register it spans, so the config has to account for every one of them --
# these are the ways of failing to.
#
# The header stops at channel.max on purpose: it allocates the channel array,
# so the channel.M lines below it only parse once it has been seen.
#
BLOCK_HEAD="write.max = 1
write.1.name    = blk
write.1.slave   = 1
write.1.type    = holding_register
write.1.address = 0x1000
write.1.topic   = t/blk
write.1.channel.max = 2"

# register 1 is spanned by the block but claimed by nobody, and would go out
# as a zero nobody asked for
bad_config "a gap in a block write is rejected" "belongs to no channel" \
    "$BLOCK_HEAD
write.1.naddress = 3
write.1.channel.1.offset = 0
write.1.channel.1.format = uint_ab
write.1.channel.2.offset = 4
write.1.channel.2.format = uint_ab"

# the 32-bit channel already covers register 1
bad_config "two channels on one register are rejected" "already claimed by another channel" \
    "$BLOCK_HEAD
write.1.channel.1.offset = 0
write.1.channel.1.format = uint_abcd
write.1.channel.2.offset = 2
write.1.channel.2.format = uint_ab"

# offset counts bytes, so a register boundary is an even one
bad_config "an odd channel offset is rejected" "not on a register boundary" \
    "$BLOCK_HEAD
write.1.channel.1.offset = 0
write.1.channel.1.format = uint_ab
write.1.channel.2.offset = 3
write.1.channel.2.format = uint_ab"

# the registers of a block are different things, so the entry cannot carry one
# format and one scaling for all of them
bad_config "format/mod on a block write entry is rejected" "belong on write.1.channel.M" \
    "$BLOCK_HEAD
write.1.format = uint_ab
write.1.mod    = -10
write.1.channel.1.offset = 0
write.1.channel.1.format = uint_ab
write.1.channel.2.offset = 2
write.1.channel.2.format = uint_ab"

# ...and the reverse: a block length with nothing laying the block out
bad_config "naddress without channels is rejected" "naddress describes a block" \
    "write.max = 1
write.1.name    = blk
write.1.slave   = 1
write.1.type    = holding_register
write.1.address = 0x1000
write.1.topic   = t/blk
write.1.format  = uint_ab
write.1.naddress = 2"

# function 16 writes registers; there is no block form for coils
bad_config "a coil block write is rejected" "coils are written one at a time" \
    "write.max = 1
write.1.name    = blk
write.1.slave   = 1
write.1.type    = coil
write.1.address = 0x1000
write.1.topic   = t/blk
write.1.channel.max = 2
write.1.channel.1.offset = 0
write.1.channel.1.format = uint_ab
write.1.channel.2.offset = 2
write.1.channel.2.format = uint_ab"

# a block channel scales a number, and a serial number is not one
bad_config "a text format in a block write channel is rejected" "write.1.channel.1.*read but not written" \
    "$BLOCK_HEAD
write.1.channel.1.offset = 0
write.1.channel.1.format = ascii_ab
write.1.channel.2.offset = 2
write.1.channel.2.format = uint_ab"

# ...and a block that is laid out correctly is accepted. Three channels, the
# last a 4-byte format spanning two registers, every register claimed exactly
# once, naddress left for the channels to settle.
good_config "a correctly laid out block write is accepted" \
    "$BLOCK_HEAD
write.1.channel.1.offset = 0
write.1.channel.1.format = uint_ab
write.1.channel.1.mod    = -10
write.1.channel.1.value  = 63
write.1.channel.2.offset = 2
write.1.channel.2.format = uint_abcd
write.1.channel.2.value  = 100000"

# an ack topic is derived rather than required
good_config "a block write with acks is accepted" \
    "mqtt.ack = 1
$BLOCK_HEAD
write.1.channel.1.offset = 0
write.1.channel.1.format = uint_ab
write.1.channel.2.offset = 2
write.1.channel.2.format = uint_ab"

echo
echo "--------------------------------"
echo "passed: $PASS   failed: $FAIL"

if [ "$FAIL" -ne 0 ]; then
    exit 1
fi

exit 0
