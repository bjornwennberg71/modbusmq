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
echo "--------------------------------"
echo "passed: $PASS   failed: $FAIL"

if [ "$FAIL" -ne 0 ]; then
    exit 1
fi

exit 0
