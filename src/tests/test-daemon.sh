#!/bin/sh
#
# This script is modified from dbus's run-with-temp-session-bus.sh.
#

SCRIPTNAME=$0

die()
{
    if ! test -z "$DBUS_SESSION_BUS_PID" ; then
        echo "killing message bus "$DBUS_SESSION_BUS_PID >&2
        kill -9 $DBUS_SESSION_BUS_PID
    fi
    echo $SCRIPTNAME: $* >&2
    exit 1
}

## convenient to be able to ctrl+C without leaking the message bus process
trap 'die "Received SIGINT"' INT

unset DBUS_SESSION_BUS_ADDRESS
unset DBUS_SESSION_BUS_PID

echo "Running dbus-launch --sh-syntax" >&2

eval `dbus-launch --sh-syntax`

if test -z "$DBUS_SESSION_BUS_PID" ; then
    die "Failed to launch message bus for test script to run"
fi

echo "Started bus pid $DBUS_SESSION_BUS_PID at $DBUS_SESSION_BUS_ADDRESS" >&2

TEMP_PULSE_DIR=`mktemp -d`
export PULSE_RUNTIME_PATH=${TEMP_PULSE_DIR}

# this script would be called inside src/ directory, so we need to use the correct path.
# notice that for tests, we don't load ALSA related modules.
pulseaudio -n \
        --log-target=file:${PWD}/pulse-daemon.log \
        --log-level=debug \
        --load="module-null-sink" \
        --load="module-null-source" \
        --load="module-suspend-on-idle" \
        --load="module-native-protocol-unix" \
        --load="module-cli-protocol-unix" \
        &

# wait a few seconds to let the daemon start!
sleep 5

unset DISPLAY

for ONE_TEST in $@; do
    ${ONE_TEST}
done

# terminate the designated pulseaudio daemon
pacmd exit

wait

kill -TERM $DBUS_SESSION_BUS_PID || die "Message bus vanished! should not have happened" && echo "Killed daemon $DBUS_SESSION_BUS_PID" >&2

sleep 2

## be sure it really died
kill -9 $DBUS_SESSION_BUS_PID > /dev/null 2>&1 || true

exit 0
