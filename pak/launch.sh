#!/bin/sh
# Moonlight pak entry point for Leaf on the MLP1.
#
# Leaf runs this from inside the .pak directory with PLATFORM/DEVICE/HOME and
# the *_PATH variables already set. moonlight itself is a command-line program,
# so this wrapper owns everything a handheld cannot do by itself: read the
# settings file, find the host, show the pairing PIN on screen, and put the
# result back in front of the user instead of on a terminal nobody has.
#
# Everything it prints lands in the shared log, moonlight.txt.
set -eu

PAK_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$PAK_DIR"

MOONLIGHT="moonlight"
NOTICE="moonlight-notice"
LOG_ROOT="${LOGS_PATH:-${SHARED_USERDATA_PATH:-/tmp/.userdata/shared}/logs}"
mkdir -p "$LOG_ROOT"
LOG="$LOG_ROOT/$MOONLIGHT.txt"
exec >>"$LOG" 2>&1

echo "=== Launching $MOONLIGHT at $(date) ==="
echo "pak: $PAK_DIR"

# Keys and any per-host config have to survive a pak update, so they live in
# the shared userdata tree and not beside the binary.
DATA_ROOT="${SHARED_USERDATA_PATH:-$PAK_DIR/.userdata}"
DATA="$DATA_ROOT/moonlight"
mkdir -p "$DATA/keys"
echo "data: $DATA"

HOME="${HOME:-/tmp}"
export HOME

# Priority, lowest to highest: the built-in fallbacks just below, then
# moonlight-user.conf, then ML_* variables in the environment. The ML_ prefix
# is how a launcher or a test drives this pak without editing the card.
if [ -r "$PAK_DIR/moonlight-user.conf" ]; then
    # shellcheck disable=SC1091
    . "$PAK_DIR/moonlight-user.conf"
fi

# Settings the user's file did not set. HOST empty means: let moonlight find
# the first host that answers on the network.
: "${HOST:=}"
: "${APP:=Steam}"
: "${MODE:=stream}"
: "${WIDTH:=1280}"
: "${HEIGHT:=720}"
: "${FPS:=60}"
: "${BITRATE:=10000}"
: "${CODEC:=h264}"
: "${EXTRA:=}"
: "${NOTICE_TIMEOUT:=30}"

[ -n "${ML_HOST+x}" ] && HOST="$ML_HOST"
[ -n "${ML_APP+x}" ] && APP="$ML_APP"
[ -n "${ML_MODE+x}" ] && MODE="$ML_MODE"
[ -n "${ML_WIDTH+x}" ] && WIDTH="$ML_WIDTH"
[ -n "${ML_HEIGHT+x}" ] && HEIGHT="$ML_HEIGHT"
[ -n "${ML_FPS+x}" ] && FPS="$ML_FPS"
[ -n "${ML_BITRATE+x}" ] && BITRATE="$ML_BITRATE"
[ -n "${ML_CODEC+x}" ] && CODEC="$ML_CODEC"
[ -n "${ML_EXTRA+x}" ] && EXTRA="$ML_EXTRA"
[ -n "${ML_NOTICE_TIMEOUT+x}" ] && NOTICE_TIMEOUT="$ML_NOTICE_TIMEOUT"

echo "mode=$MODE host='${HOST:-<discover>}' app='$APP' ${WIDTH}x${HEIGHT}@${FPS} bitrate=${BITRATE} codec=$CODEC"

MAPPING="$PAK_DIR/res/gamecontrollerdb.txt"

notice() {
    # TITLE [BODY...] on screen; never fatal if the notice program is missing,
    # the log still has everything.
    if [ -x "$PAK_DIR/$NOTICE" ]; then
        "$PAK_DIR/$NOTICE" --timeout "$NOTICE_TIMEOUT" "$@" || true
    fi
    for line in "$@"; do
        echo "notice: $line"
    done
}

# "Can't connect" is almost always routing or a firewall, and which of the two
# it is depends on a fact nothing else here reports: which network this device
# is actually on. A handheld on a phone hotspot and a PC on the home LAN look
# identical from inside moonlight.
my_addrs=$((ifconfig 2>/dev/null || ip -o -4 addr 2>/dev/null) |
    sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p; s/.*inet \([0-9.]*\) .*/\1/p' |
    grep -v '^127\.' | sort -u | tr '\n' ' ') || true
[ -n "$my_addrs" ] && echo "device addresses: $my_addrs"

if [ -n "$HOST" ]; then
    host_net=$(printf '%s\n' "$HOST" | cut -d. -f1-3)
    my_nets=$(printf '%s\n' "$my_addrs" | tr ' ' '\n' | cut -d. -f1-3 | sort -u) || true
    if [ -n "$my_nets" ] && ! printf '%s\n' "$my_nets" | grep -qx "$host_net"; then
        echo "NOTE: host $HOST is on $host_net.x but this device is on:" \
            "$(printf '%s\n' "$my_nets" | tr '\n' ' ')"
    fi
fi

# There used to be a TCP reachability probe here. It is gone, because it
# turned out to be worse than nothing twice over:
#
#   - nc blocks. busybox nc waits for the peer to close even after stdin hits
#     EOF, and a streamer accepts the connection and then says nothing until
#     spoken to, so the probe hung and never reached moonlight at all. That
#     is what a launch log that stops right after "device addresses" is.
#   - nc's exit status means opposite things across implementations. BSD nc
#     returns 0 after its own timeout; busybox nc gets killed by timeout(1)
#     and returns 124 for a host that is genuinely up. Both take three
#     seconds, so neither timing nor status can tell them apart.
#
# A diagnosis that blocks the thing it is diagnosing, and whose verdict is
# not portable, is not a diagnosis. moonlight reports this better itself.

# The host argument goes last, and only when there is one: with no argument
# moonlight runs its own discovery and reports what it found.

case "$MODE" in
pair)
    PAIR_OUT="$LOG_ROOT/moonlight-pair.txt"
    : >"$PAIR_OUT"

    if [ -n "$HOST" ]; then
        set -- pair -keydir "$DATA/keys" -mapping "$MAPPING" "$HOST"
    else
        set -- pair -keydir "$DATA/keys" -mapping "$MAPPING"
    fi

    echo "running: $MOONLIGHT $*"
    # moonlight prints the PIN and then blocks waiting for it to be typed into
    # the host, so it has to stay running while the PIN is on screen.
    "./$MOONLIGHT" "$@" >"$PAIR_OUT" 2>&1 &
    pair_pid=$!

    pin=""
    waited=0
    while [ "$waited" -lt "${PIN_TIMEOUT:-30}" ]; do
        if [ -s "$PAIR_OUT" ]; then
            pin=$(sed -n 's/.*target PC: *\([0-9][0-9][0-9][0-9]\).*/\1/p' "$PAIR_OUT" | head -n 1)
            [ -n "$pin" ] && break
        fi
        # A PIN can only come from a process that is still alive. moonlight
        # exits within a second or two when it cannot reach the host, so stop
        # waiting the moment it dies rather than sitting out the full timeout
        # and reporting "no PIN appeared" -- which reads like a display fault
        # and sends everyone looking in the wrong place.
        if ! kill -0 "$pair_pid" 2>/dev/null; then
            echo "moonlight exited after ${waited}s without printing a PIN"
            break
        fi
        waited=$((waited + 1))
        sleep 1
    done

    if [ -z "$pin" ]; then
        wait "$pair_pid" || true
        # Say what went wrong, not just what did not happen. "No PIN appeared"
        # is true for every failure from here to the PC's firewall.
        reason=$(tr '\n' ' ' <"$PAIR_OUT" | cut -c1-160)
        if grep -qiE "can.?t connect|cannot connect" "$PAIR_OUT"; then
            notice "CANNOT REACH HOST" \
                "${HOST:-the host} did not answer on port 47989." \
                "On the PC: is Sunshine/GFE running?" \
                "Does the firewall allow moonlight?" \
                "Same network as this device?"
        elif grep -qi "autodiscovery failed" "$PAIR_OUT"; then
            notice "NO HOST FOUND" \
                "Nothing answered mDNS discovery." \
                "Set HOST to the PC's IP in" \
                "moonlight-user.conf and retry."
        elif grep -qiE "pin|Enter the following" "$PAIR_OUT"; then
            notice "PAIRING" "PIN was printed but not recognised." "$reason"
        else
            notice "PAIR FAILED" "$reason" "See moonlight-pair.txt"
        fi
        echo "pair output: $reason"
        exit 1
    fi

    if [ -n "$HOST" ]; then
        notice "PAIRING" "Enter this PIN on your PC:" "$pin" "Host: $HOST"
    else
        notice "PAIRING" "Enter this PIN on your PC:" "$pin"
    fi

    wait "$pair_pid" || true
    if grep -qi "ucces.*pair" "$PAIR_OUT"; then
        notice "PAIRED" "Pairing succeeded." "Set MODE=stream and reopen."
    else
        notice "PAIR FAILED" "$(tail -n 3 "$PAIR_OUT" | tr '\n' ' ' | cut -c1-200)"
    fi
    ;;

list)
    if [ -n "$HOST" ]; then
        set -- list -keydir "$DATA/keys" "$HOST"
    else
        set -- list -keydir "$DATA/keys"
    fi
    echo "running: $MOONLIGHT $*"
    "./$MOONLIGHT" "$@" || true
    ;;

quit)
    if [ -n "$HOST" ]; then
        set -- quit -keydir "$DATA/keys" "$HOST"
    else
        set -- quit -keydir "$DATA/keys"
    fi
    echo "running: $MOONLIGHT $*"
    "./$MOONLIGHT" "$@" || true
    ;;

stream)
    set -- stream -platform sdl \
        -keydir "$DATA/keys" \
        -mapping "$MAPPING" \
        -app "$APP" \
        -width "$WIDTH" -height "$HEIGHT" \
        -fps "$FPS" \
        -bitrate "$BITRATE" \
        -codec "$CODEC" \
        $EXTRA
    if [ -n "$HOST" ]; then
        set -- "$@" "$HOST"
    fi

    echo "running: $MOONLIGHT $*"
    # exec: the stream is the whole point of the launch, and Leaf expects the
    # pak's process to be the one that is playing.
    exec "./$MOONLIGHT" "$@"
    ;;

*)
    notice "MOONLIGHT" "Unknown MODE '$MODE' in moonlight-user.conf"
    exit 1
    ;;
esac

exit 0
