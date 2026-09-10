#!/bin/bash
#
# Runs the test suite against both build configurations.
#
# MQTT_ENABLED is a compile-time flag applied to the whole tree, so the two
# configurations are genuinely different builds. libmodbusmq itself never
# references mosquitto and must pass either way; modbusmq needs MQTT on to do
# its real job, so that half is where the publish path is actually compiled.
#
# The builds go in throwaway directories so debug/ and release/ keep whatever
# the user configured them with.
#
# usage: bash test/run_matrix.sh
#
# bjornw: 202509
#

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

FAILED=0

#
# configure, build and test one configuration
#
run_config()
{
    local mqtt=$1
    local dir="$WORK/mqtt_$mqtt"

    echo "================================================"
    echo "MQTT_ENABLED=$mqtt"
    echo "================================================"

    mkdir -p "$dir"

    if ! (cd "$dir" && cmake -DCMAKE_BUILD_TYPE=Debug -DMQTT_ENABLED="$mqtt" "$ROOT" > "$dir/cmake.log" 2>&1); then
        if [ "$mqtt" = "ON" ] && grep -qi "mosquitto" "$dir/cmake.log"; then
            echo "  SKIP: libmosquitto not installed"
            echo
            return
        fi
        echo "  FAIL: cmake failed"
        tail -15 "$dir/cmake.log" | sed 's/^/    /'
        FAILED=$((FAILED+1))
        return
    fi

    if ! (cd "$dir" && make > "$dir/build.log" 2>&1); then
        echo "  FAIL: build failed"
        grep -E "error" "$dir/build.log" | head -10 | sed 's/^/    /'
        FAILED=$((FAILED+1))
        return
    fi

    local warnings
    warnings=$(grep -c "warning:" "$dir/build.log")

    if [ "$warnings" -ne 0 ]; then
        echo "  FAIL: $warnings compiler warnings"
        grep "warning:" "$dir/build.log" | sed "s|$ROOT/||" | sort -u | head -20 | sed 's/^/    /'
        FAILED=$((FAILED+1))
    else
        echo "  ok   builds with no warnings"
    fi

    #
    # A machine with libmosquitto installed links it happily even when MQTT is
    # off, so the missing-dependency bug is invisible here and only shows up on
    # a clean one. Assert on the actual link instead.
    #
    if [ "$mqtt" = "OFF" ]; then
        if ldd "$dir/programs/modbusmq" 2>/dev/null | grep -qi mosquitto; then
            echo "  FAIL: modbusmq links libmosquitto in an MQTT_ENABLED=OFF build"
            FAILED=$((FAILED+1))
        else
            echo "  ok   does not link libmosquitto"
        fi
    fi

    if bash "$HERE/run_tests.sh" "$dir"; then
        echo "  ok   suite passed with MQTT_ENABLED=$mqtt"
    else
        echo "  FAIL: suite failed with MQTT_ENABLED=$mqtt"
        FAILED=$((FAILED+1))
    fi

    echo
}

run_config OFF
run_config ON

echo "================================================"
if [ "$FAILED" -ne 0 ]; then
    echo "$FAILED configuration(s) failed"
    exit 1
fi

echo "all configurations passed"
exit 0
