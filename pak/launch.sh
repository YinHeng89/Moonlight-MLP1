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
    while [ "$waited" -lt 20 ]; do
        if [ -s "$PAIR_OUT" ]; then
            pin=$(sed -n 's/.*target PC: *\([0-9][0-9][0-9][0-9]\).*/\1/p' "$PAIR_OUT" | head -n 1)
            [ -n "$pin" ] && break
        fi
        waited=$((waited + 1))
        sleep 1
    done

    if [ -n "$pin" ]; then
        if [ -n "$HOST" ]; then
            notice "PAIRING" "Enter this PIN on your PC:" "$pin" "Host: $HOST"
        else
            notice "PAIRING" "Enter this PIN on your PC:" "$pin"
        fi
    else
        echo "no PIN appeared within 20s" >&2
        notice "PAIRING" "No PIN appeared." "See moonlight.txt"
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
        "./$MOONLIGHT" list -keydir "$DATA/keys" "$HOST" || true
    else
        "./$MOONLIGHT" list -keydir "$DATA/keys" || true
    fi
    ;;

quit)
    if [ -n "$HOST" ]; then
        "./$MOONLIGHT" quit -keydir "$DATA/keys" "$HOST" || true
    else
        "./$MOONLIGHT" quit -keydir "$DATA/keys" || true
    fi
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
