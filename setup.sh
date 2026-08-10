#!/bin/bash
#
# configure the debug/ and release/ build trees
#
# usage: setup.sh [--enable-mqtt]
#
#   --enable-mqtt   build modbusmq_subscribe against libmosquitto, so it
#                   publishes to the broker instead of only polling
#
MQTT=OFF

usage()
{
    echo "usage: $0 [--enable-mqtt]"
}

for arg in "$@"; do
    case "$arg" in
        --enable-mqtt) MQTT=ON ;;
        -h|--help)     usage; exit 0 ;;
        *)
            echo "$0: unknown option: $arg" >&2
            usage >&2
            exit 1
            ;;
    esac
done

echo "configuring debug/ and release/ with MQTT_ENABLED=$MQTT"

mkdir -p debug release
(cd debug && cmake -DCMAKE_BUILD_TYPE=Debug -DMQTT_ENABLED=$MQTT -DCMAKE_INSTALL_PREFIX=`pwd`/modbusmq .. )
(cd release && cmake -DCMAKE_BUILD_TYPE=Release -DMQTT_ENABLED=$MQTT -DCMAKE_INSTALL_PREFIX=`pwd`/modbusmq .. )
