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
PROBE="moonlight-keyprobe"
MENU="moonlight-menu"
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
#
# It is a function because it runs twice when the settings screen is used: once
# to show the screen its current values, and again after it has written the
# file, because every setting may have changed while the screen was open.
load_settings() {
if [ -r "$PAK_DIR/moonlight-user.conf" ]; then
    # shellcheck disable=SC1091
    . "$PAK_DIR/moonlight-user.conf"
fi

# Settings the user's file did not set. HOST empty means: let moonlight find
# the first host that answers on the network.
: "${HOST:=}"
: "${APP:=Steam}"
: "${MODE:=stream}"
# The MLP1's panel: 960x720, 4:3. Anything wider is scaled into a shape it is
# not, and the extra pixels are paid for twice -- over the network and again
# in software decode.
: "${WIDTH:=960}"
: "${HEIGHT:=720}"
: "${FPS:=30}"
: "${BITRATE:=5000}"
: "${CODEC:=h264}"
: "${PACKETSIZE:=1024}"
: "${QUIT_COMBO:=}"
: "${PAD_MAP:=}"
# What gets streamed is a PC game, and a PC game draws its prompts for an Xbox
# pad: confirm at the bottom. On this device that is the button printed B, so
# the positions are what have to win by default.
: "${PAD_LAYOUT:=xbox}"
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
[ -n "${ML_PACKETSIZE+x}" ] && PACKETSIZE="$ML_PACKETSIZE"
[ -n "${ML_QUIT_COMBO+x}" ] && QUIT_COMBO="$ML_QUIT_COMBO"
[ -n "${ML_PAD_LAYOUT+x}" ] && PAD_LAYOUT="$ML_PAD_LAYOUT"
[ -n "${ML_PAD_MAP+x}" ] && PAD_MAP="$ML_PAD_MAP"
[ -n "${ML_PAD_WIRING+x}" ] && PAD_WIRING="$ML_PAD_WIRING"

# moonlight reads this to build a quit combination out of buttons this device
# actually has, matching each name against both key names and gamepad buttons.
# A combination guessed wrong is worse than none, because it fires in the
# middle of a game instead of ending the stream, which is why the probe exists.
if [ -n "$QUIT_COMBO" ]; then
    ML_QUIT_COMBO="$QUIT_COMBO"
    export ML_QUIT_COMBO
else
    # Cleared in the settings screen. The export from the first pass is still
    # in this process's environment, and moonlight would go on using a
    # combination the user has just removed.
    unset ML_QUIT_COMBO
fi

# Which of the two arrangements of the face buttons the host should be told
# about. The names in QUIT_COMBO are unaffected by this: they stay the ones the
# key probe printed, because a quit has to be reachable either way.
if [ -n "$PAD_LAYOUT" ]; then
    ML_PAD_LAYOUT="$PAD_LAYOUT"
    export ML_PAD_LAYOUT
else
    unset ML_PAD_LAYOUT
fi

# The table the settings screen writes when the buttons were pointed somewhere
# by hand. It wins over PAD_LAYOUT, which is only the two presets, and is left
# empty whenever one of those presets is what the user picked.
if [ -n "$PAD_MAP" ]; then
    ML_PAD_MAP="$PAD_MAP"
    export ML_PAD_MAP
else
    unset ML_PAD_MAP
fi

# How the four face buttons are wired to what SDL reports. Left empty, both the
# settings screen and the streamer work it out from the device; set it to
# "labels" if a pad is ever numbered the way it is printed after all.
if [ -n "$PAD_WIRING" ]; then
    ML_PAD_WIRING="$PAD_WIRING"
    export ML_PAD_WIRING
else
    unset ML_PAD_WIRING
fi
[ -n "${ML_NOTICE_TIMEOUT+x}" ] && NOTICE_TIMEOUT="$ML_NOTICE_TIMEOUT"

echo "mode=$MODE host='${HOST:-<discover>}' app='$APP' ${WIDTH}x${HEIGHT}@${FPS} bitrate=${BITRATE} codec=$CODEC packetsize=${PACKETSIZE:-default} pad=${PAD_MAP:-${PAD_LAYOUT:-default}} quit='${QUIT_COMBO:-none}'"
}

MENU_USED=0
load_settings

# The panel this is drawing on, which is not something a config file should
# have to guess: the MLP1's is 960x720, a 4:3 panel, so a 16:9 stream is both
# scaled and squashed on the way in. Worth one line in the log, because
# "why does it look stretched / why is it slow" both come back to it.
screen_res=$(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null || true)
if [ -z "$screen_res" ]; then
    screen_res=$(fbset -s 2>/dev/null |
        sed -n 's/.*mode "\([0-9]*x[0-9]*\)".*/\1/p' | head -n 1) || true
fi
screen_res=$(printf '%s' "$screen_res" | tr ',' 'x' | tr -d ' ')
[ -n "$screen_res" ] && echo "screen: $screen_res"

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

# Two probes, both used only by MODE=diag, both written to be unable to hang.
# That is not defensive habit: a diagnostic that runs long enough reads exactly
# like the fault it is meant to rule out, and the last launch log that stopped
# mid-diagnosis cost an evening.

run_timeout() {
    # timeout(1) comes from busybox on this device. If a firmware ever ships
    # without it, every probe below becomes an unbounded wait, which is the one
    # failure mode these probes exist to prevent -- so bring our own watchdog
    # rather than trust the binary to be there.
    secs=$1
    shift
    if command -v timeout >/dev/null 2>&1; then
        timeout "$secs" "$@"
        return $?
    fi
    "$@" &
    _p=$!
    ( sleep "$secs"; kill "$_p" 2>/dev/null ) &
    _w=$!
    wait "$_p" 2>/dev/null
    _rc=$?
    kill "$_w" 2>/dev/null || true
    wait "$_w" 2>/dev/null || true   # reap it, or the shell reports the kill
    return "$_rc"
}

run_ping() {
    # Three packets, two seconds each, eight seconds wall -- ICMP is allowed to
    # be silently dropped by plenty of networks, so NO REPLY narrows things
    # without proving anything.
    if run_timeout 8 ping -c 3 -W 2 "$1" >/dev/null 2>&1; then
        echo "OK"
    else
        echo "NO REPLY"
    fi
}

run_tcp() {
    # host port -> OPEN / CLOSED / n/a. This is the probe nc used to do, minus
    # the two ways nc got it wrong: it blocks (a streamer accepts and then
    # waits to be spoken to), and its exit status means different things in
    # busybox and BSD. bash's /dev/tcp does a connect and nothing else, so it
    # cannot block on a peer that is quiet, and its status means one thing.
    if command -v bash >/dev/null 2>&1; then
        if run_timeout 5 bash -c "exec 3<>/dev/tcp/$1/$2" >/dev/null 2>&1; then
            echo "OPEN"
        else
            echo "CLOSED"
        fi
    else
        echo "n/a"
    fi
}

# "Can't connect" is almost always routing or a firewall, and which of the two
# it is depends on a fact nothing else here reports: which network this device
# is actually on. A handheld on a phone hotspot and a PC on the home LAN look
# identical from inside moonlight.
my_addrs=$((ifconfig 2>/dev/null || ip -o -4 addr 2>/dev/null) |
    sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p; s/.*inet \([0-9.]*\) .*/\1/p' |
    grep -v '^127\.' | sort -u | tr '\n' ' ') || true
[ -n "$my_addrs" ] && echo "device addresses: $my_addrs"

# The gateway and the SSID settle the same question from the other end. Two
# devices can both be "on WiFi" and still be on different networks -- a guest
# SSID, a repeater, a phone hotspot -- and knowing which one this is turns
# "moonlight cannot reach the PC" from a mystery into a router setting.
gateway=$(ip route 2>/dev/null |
    sed -n 's/^default.*[ ]via \([0-9.]*\).*/\1/p' | head -n 1) || true
if [ -z "$gateway" ]; then
    gateway=$(route -n 2>/dev/null | awk '$1 == "0.0.0.0" { print $2; exit }') || true
fi
[ -n "$gateway" ] && echo "gateway: $gateway"

ssid=$(iwgetid -r 2>/dev/null || true)
if [ -z "$ssid" ]; then
    ssid=$(iw dev 2>/dev/null | sed -n 's/^[[:space:]]*ssid \(.*\)$/\1/p' | head -n 1) || true
fi
[ -n "$ssid" ] && echo "wifi: $ssid"

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

# The settings screen, when the file asks for it. Editing moonlight-user.conf
# means editing a shell file on an SD card, which on a handheld with no
# keyboard is not editing at all -- so the pak opens here instead, and whatever
# the user picks comes back through one file: the settings are written into
# moonlight-user.conf, the action into menu-action.
if [ "$MODE" = menu ]; then
    MENU_OUT="$DATA/menu-action"
    : >"$MENU_OUT"
    if [ -x "$PAK_DIR/$MENU" ]; then
        echo "running: $MENU"
        menu_rc=0
        "./$MENU" --conf "$PAK_DIR/moonlight-user.conf" --out "$MENU_OUT" \
            --timeout "${MENU_TIMEOUT:-600}" || menu_rc=$?
        menu_action=$(cat "$MENU_OUT" 2>/dev/null || true)
        echo "menu action: ${menu_action:-none} (exit $menu_rc)"
        if [ -z "$menu_action" ] && [ "$menu_rc" != 0 ]; then
            # It never opened a window, so nothing it might have printed was
            # visible. Say it here instead of leaving a black screen with no
            # reason, which is how this whole class of bug began.
            notice "SETTINGS DID NOT START" \
                "$MENU exited $menu_rc before choosing anything." \
                "See moonlight.txt for its output." \
                "Set MODE=stream in moonlight-user.conf" \
                "to skip this screen."
            exit 1
        fi
        if [ -z "$menu_action" ] || [ "$menu_action" = none ]; then
            echo "no action chosen, stopping"
            exit 0
        fi
        # Every setting may have changed while the screen was open, so read the
        # file again. MODE in it is "menu" -- the screen is how the pak opens --
        # so the action the user picked is what decides this run.
        [ "$menu_action" = menu ] && menu_action=stream   # never back to here
        load_settings
        MODE="$menu_action"
        MENU_USED=1
    else
        notice "SETTINGS UNAVAILABLE" "$MENU is missing from the pak." \
            "Falling back to MODE=stream."
        MODE=stream
    fi
fi

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

    # The default 30s is not long enough for this one: the PIN has to be read
    # off a handheld, then walked over to another machine and typed into a web
    # page. Losing the PIN to a timeout is the worst possible failure here,
    # because the pairing has to be restarted on the host to get another.
    NOTICE_TIMEOUT="${PIN_NOTICE_TIMEOUT:-180}"
    if [ -n "$HOST" ]; then
        notice "PAIRING" "Enter this PIN on your PC:" "$pin" \
            "Host: $HOST" \
            "Sunshine: open https://$HOST:47990" \
            "and use its PIN page."
    else
        notice "PAIRING" "Enter this PIN on your PC:" "$pin" \
            "Sunshine's web UI, PIN page."
    fi

    wait "$pair_pid" || true
    if grep -qi "ucces.*pair" "$PAIR_OUT"; then
        # Pairing is a one-off, so put the card back the way the user wants it
        # rather than making them edit the file again before anything works.
        # Best effort: the card can be mounted read-only, and a failed pairing
        # must never be the thing that leaves MODE stuck on pair.
        if [ "$MENU_USED" = 1 ]; then
            # The settings screen is how the pak opens, so there is nothing to
            # switch back: the user picks Action=stream on the next launch.
            paired_note="Open the pak again and press START with Action set to stream."
        else
        paired_note="Set MODE=stream in moonlight-user.conf and open again."
        if [ -w "$PAK_DIR/moonlight-user.conf" ] &&
            sed 's/^MODE=.*/MODE="stream"/' "$PAK_DIR/moonlight-user.conf" \
                >"$PAK_DIR/.moonlight-user.conf.new" 2>/dev/null &&
            cat "$PAK_DIR/.moonlight-user.conf.new" >"$PAK_DIR/moonlight-user.conf"; then
            rm -f "$PAK_DIR/.moonlight-user.conf.new"
            echo "moonlight-user.conf: MODE is now stream"
            paired_note="moonlight-user.conf is back on MODE=stream. Open the pak again to stream."
        fi
        fi
        notice "PAIRED" "Pairing succeeded." "$paired_note"
    else
        notice "PAIR FAILED" "$(tail -n 3 "$PAIR_OUT" | tr '\n' ' ' | cut -c1-200)" \
            "Sunshine needs the PIN typed into" \
            "https://${HOST:-the host}:47990 while" \
            "this PIN is on screen."
    fi
    ;;

probe)
    # What are this device's buttons? Asked because the answer decides the only
    # thing that cannot be guessed: the keys to hold to end a stream. The probe
    # prints every key and button it sees to the log, which is readable on a
    # computer, and stops on its own -- it cannot ask for a button to stop.
    PROBE_OUT="$LOG_ROOT/moonlight-probe.txt"
    : >"$PROBE_OUT"
    if [ -x "$PAK_DIR/$PROBE" ]; then
        echo "running: $PROBE for ${PROBE_SECONDS:-60}s"
        # A little longer than the probe's own timer, so a hung probe is
        # reported rather than waited out.
        run_timeout "$(( ${PROBE_SECONDS:-60} + 15 ))" \
            "./$PROBE" --seconds "${PROBE_SECONDS:-60}" >"$PROBE_OUT" 2>&1 || true
        cat "$PROBE_OUT"
        notice "KEY PROBE DONE" \
            "Every button name went to" \
            "moonlight-probe.txt on the card." \
            "Names like back,start or x,b" \
            "set QUIT_COMBO in" \
            "moonlight-user.conf."
    else
        notice "KEY PROBE" "$PROBE is missing from the pak"
    fi
    ;;

diag)
    # Answer one question -- can this device reach the host at all? -- from the
    # bottom of the stack upwards, then put the answer on the screen instead of
    # in a log nobody can read on a handheld. It also doubles as a check of the
    # notice program itself: if the panel appears, the screen works and every
    # other "nothing happened" report is about the network.
    echo "--- network diagnosis ---"

    gw_res="-"
    [ -n "$gateway" ] && gw_res=$(run_ping "$gateway")
    echo "ping gateway ${gateway:-<none>}: $gw_res"

    host_ping="-"
    p_http="-"
    p_https="-"
    summary="no host set"
    if [ -n "$HOST" ]; then
        host_ping=$(run_ping "$HOST")
        echo "ping host $HOST: $host_ping"
        p_http=$(run_tcp "$HOST" 47989)
        p_https=$(run_tcp "$HOST" 47984)
        echo "tcp $HOST 47989 (serverinfo, http): $p_http"
        echo "tcp $HOST 47984 (applist, https): $p_https"

        DIAG_OUT="$LOG_ROOT/moonlight-diag.txt"
        : >"$DIAG_OUT"
        # The real probe: moonlight asking the host for its app list does the
        # same HTTPS round trip a stream does, and unlike a port scan it says
        # which layer said no -- refused, dropped, unpaired, wrong version.
        echo "running: $MOONLIGHT list -keydir $DATA/keys $HOST"
        run_timeout 25 "./$MOONLIGHT" list -keydir "$DATA/keys" "$HOST" \
            >"$DIAG_OUT" 2>&1 || echo "moonlight list exited nonzero"
        # The tail, not the head: moonlight talks while it works and only says
        # what actually happened at the end, so the first 90 characters are
        # usually "Connecting to ...".
        summary=$(tail -n 2 "$DIAG_OUT" | tr '\n' ' ' | cut -c1-90)
        [ -n "$summary" ] || summary="no output at all"
        echo "moonlight list: $summary"
    fi

    # Longer than the default: this is the one panel worth reading properly,
    # and A dismisses it anyway.
    NOTICE_TIMEOUT="${DIAG_NOTICE_TIMEOUT:-90}"
    set -- "NETWORK DIAGNOSIS" \
        "This device: ${my_addrs:-no IP}" \
        "WiFi: ${ssid:-unknown}  Gateway: ${gateway:-unknown}" \
        "Ping gateway: $gw_res" \
        "Ping PC ${HOST:-<not set>}: $host_ping" \
        "PC ports 47989 / 47984: $p_http / $p_https" \
        "moonlight says: $summary"
    # Worth saying out loud, because it is how this usually ends: the network
    # was never the problem, the pairing just has not happened yet. "Ping PC:
    # NO REPLY" alongside two OPEN ports is the same story -- plenty of hosts
    # ignore ICMP and it means nothing about GameStream.
    if printf '%s\n' "$summary" | grep -qi "must pair"; then
        set -- "$@" "PC is reachable. Set MODE=pair and open again."
    else
        set -- "$@" "Full results in moonlight.txt"
    fi
    notice "$@"
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
        -codec "$CODEC"
    # Kept under the 1500-byte ethernet MTU on purpose. Video packets larger
    # than that get fragmented, and on a lossy 2.4GHz link losing one fragment
    # of a frame costs the whole frame -- which is what "it stutters" looks
    # like from the couch. Empty disables it and takes moonlight's default.
    if [ -n "$PACKETSIZE" ]; then
        set -- "$@" -packetsize "$PACKETSIZE"
    fi
    set -- "$@" $EXTRA
    if [ -n "$HOST" ]; then
        set -- "$@" "$HOST"
    fi

    echo "running: $MOONLIGHT $*"

    # Deliberately not exec. exec is the obvious way to hand the device over to
    # moonlight, and it is also why a failed stream was a black screen: every
    # way this can fail -- never paired, host unreachable, app name not on the
    # host -- moonlight reports in the first second or two and then exits, and
    # with exec its exit is the script's exit. Nothing was left to print the
    # reason, so the launch ended with an empty screen and a log nobody can
    # read on this device. Start it in the background instead and watch it for
    # a few seconds: if it is still alive by then it is streaming, and this
    # script does nothing further until it ends, exactly as exec would have.
    STREAM_OUT="$LOG_ROOT/moonlight-stream.txt"
    : >"$STREAM_OUT"
    "./$MOONLIGHT" "$@" >"$STREAM_OUT" 2>&1 &
    stream_pid=$!

    waited=0
    while [ "$waited" -lt "${STREAM_START_TIMEOUT:-5}" ]; do
        kill -0 "$stream_pid" 2>/dev/null || break
        sleep 1
        waited=$((waited + 1))
    done

    if kill -0 "$stream_pid" 2>/dev/null; then
        # It got past the handshake; whatever happens now is the stream.
        wait "$stream_pid" || true
        cat "$STREAM_OUT"
        exit 0
    fi

    wait "$stream_pid" || true
    cat "$STREAM_OUT"

    reason=$(tr '\n' ' ' <"$STREAM_OUT" | cut -c1-200)
    echo "stream failed: $reason"
    if grep -qiE "can.?t connect" "$STREAM_OUT"; then
        notice "CANNOT REACH HOST" \
            "${HOST:-the host} did not answer." \
            "Check Sunshine is running and that" \
            "this device is on the same network." \
            "Run MODE=diag to see what is blocked."
    elif grep -qi "autodiscovery failed" "$STREAM_OUT"; then
        notice "NO HOST FOUND" \
            "Nothing answered discovery." \
            "Set HOST to the PC's IP in" \
            "moonlight-user.conf."
    elif grep -qi "pair" "$STREAM_OUT"; then
        notice "NOT PAIRED" \
            "This device has not been paired with" \
            "${HOST:-the host} yet." \
            "Set MODE=pair in moonlight-user.conf" \
            "and open the pak again for a PIN."
    elif grep -qiE "not found|no application|does not exist" "$STREAM_OUT"; then
        notice "APP NOT FOUND" \
            "'$APP' is not listed on the host." \
            "Run MODE=list to see the real names."
    else
        notice "STREAM FAILED" "$reason" "See moonlight-stream.txt"
    fi
    exit 1
    ;;

*)
    notice "MOONLIGHT" "Unknown MODE '$MODE' in moonlight-user.conf"
    exit 1
    ;;
esac

exit 0
