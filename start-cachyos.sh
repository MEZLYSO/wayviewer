#!/usr/bin/env bash
#
# Start wayviewer and the Python SRT relay together on Arch Linux / CachyOS.
#
# wayviewer captures and encodes the screen.  The relay reads the encoded
# MPEG-TS stream from the Unix socket and serves it to any number of SRT
# clients.  Both processes stop together on Ctrl+C.
#
# Usage:
#   ./start-cachyos.sh
#   HOST=0.0.0.0 ./start-cachyos.sh
#   SRT_PORT=9000 LATENCY=120 ./start-cachyos.sh
#
# Environment:
#   HOST        TCP bind address               (default: 127.0.0.1)
#   PORT        TCP port                       (default: 12401)
#   SRT_BIND    SRT bind address               (default: 0.0.0.0)
#   SRT_PORT    SRT UDP port                   (default: 9000)
#   LATENCY     SRT latency in ms              (default: 120)
#   MAXBW       SRT max bandwidth in kbit/s    (default: 8000)
#   WAYVIEWER_* encoder/capture options        (see wayviewer --help)

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BIN="$SCRIPT_DIR/build/wayviewer"
RELAY="$SCRIPT_DIR/python/wayviewer_relay.py"

HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-12401}"
SRT_BIND="${SRT_BIND:-0.0.0.0}"
SRT_PORT="${SRT_PORT:-9000}"
LATENCY="${LATENCY:-120}"
MAXBW="${MAXBW:-8000}"

SOCKET="${WAYVIEWER_UNIX_SOCKET:-${XDG_RUNTIME_DIR:-/tmp}/wayviewer-${PORT}.sock}"
export WAYVIEWER_UNIX_SOCKET="$SOCKET"

usage() {
    cat <<EOF
Usage: $0 [--help]

Start wayviewer plus the Python SRT relay.

Environment:
  HOST        TCP bind address               (default: 127.0.0.1)
  PORT        TCP port                       (default: 12401)
  SRT_BIND    SRT bind address               (default: 0.0.0.0)
  SRT_PORT    SRT UDP port                   (default: 9000)
  LATENCY     SRT latency in ms              (default: 120)
  MAXBW       SRT max bandwidth in kbit/s    (default: 8000)
  WAYVIEWER_FPS, WAYVIEWER_BITRATE, WAYVIEWER_GOP,
  WAYVIEWER_CRF, WAYVIEWER_PRESET, WAYVIEWER_THREADS
              Encoder/capture options (see ./build/wayviewer --help)

Examples:
  ./start-cachyos.sh
  HOST=0.0.0.0 ./start-cachyos.sh
  LATENCY=200 ./start-cachyos.sh
  WAYVIEWER_BITRATE=3000 WAYVIEWER_THREADS=2 ./start-cachyos.sh
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

if [[ ! -x "$BIN" ]]; then
    echo "Binary not found: $BIN" >&2
    echo "Build it first with: ./build-cachyos.sh" >&2
    exit 1
fi

if [[ ! -f "$RELAY" ]]; then
    echo "Relay not found: $RELAY" >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 not found. Install it with: sudo pacman -S python" >&2
    exit 1
fi

if ! ldconfig -p 2>/dev/null | grep -q 'libsrt\.so' \
    && ! ls /usr/lib/libsrt.so* /usr/lib64/libsrt.so* >/dev/null 2>&1; then
    echo "warning: libsrt not found; install it with: sudo pacman -S srt" >&2
fi

port_in_use() {
    local proto="$1"
    local port="$2"
    command -v ss >/dev/null 2>&1 || return 1
    # The filter prints a header even when there are no matches, so drop it.
    ss -l"$proto"n "sport = :$port" 2>/dev/null | tail -n +2 | grep -q .
}

if port_in_use t "$PORT"; then
    echo "TCP port $PORT is already in use." >&2
    echo "Stop the other instance or run with PORT=<other port>." >&2
    exit 1
fi

if port_in_use u "$SRT_PORT"; then
    echo "UDP port $SRT_PORT is already in use." >&2
    echo "Stop the other relay or run with SRT_PORT=<other port>." >&2
    exit 1
fi

WV_PID=""
RELAY_PID=""

cleanup() {
    trap - EXIT INT TERM

    if [[ -n "$RELAY_PID" ]] && kill -0 "$RELAY_PID" 2>/dev/null; then
        kill "$RELAY_PID" 2>/dev/null || true
    fi
    if [[ -n "$WV_PID" ]] && kill -0 "$WV_PID" 2>/dev/null; then
        kill "$WV_PID" 2>/dev/null || true
    fi

    wait 2>/dev/null || true
}

trap cleanup EXIT INT TERM

echo "Starting wayviewer on tcp://$HOST:$PORT"
echo "Unix socket: $SOCKET"
"$BIN" --host "$HOST" --port "$PORT" &
WV_PID=$!

# Wait for wayviewer to create the Unix socket.  Check the process first so a
# stale socket from another instance cannot make us start the relay against it.
for _ in $(seq 1 100); do
    if ! kill -0 "$WV_PID" 2>/dev/null; then
        wait "$WV_PID" 2>/dev/null || true
        echo "wayviewer exited during startup" >&2
        exit 1
    fi
    [[ -S "$SOCKET" ]] && break
    sleep 0.1
done

if [[ ! -S "$SOCKET" ]]; then
    echo "Timed out waiting for $SOCKET" >&2
    exit 1
fi

echo "Starting SRT relay on srt://$SRT_BIND:$SRT_PORT (latency ${LATENCY} ms)"
python3 -u "$RELAY" \
    --unix-socket "$SOCKET" \
    --srt-bind "$SRT_BIND" \
    --srt-port "$SRT_PORT" \
    --latency "$LATENCY" \
    --maxbw "$MAXBW" &
RELAY_PID=$!

echo
echo "Local IP addresses:"
if command -v ip >/dev/null 2>&1; then
    ip -o -4 addr show scope global 2>/dev/null \
        | awk '{print $4}' | cut -d/ -f1 | sed 's/^/  /' || true
else
    hostname -I 2>/dev/null | tr ' ' '\n' | sed '/^$/d; s/^/  /' || true
fi

echo
echo "On the other device:"
echo "  ffplay -probesize 65536 -analyzeduration 100000 \\"
echo "    -fflags nobuffer -flags low_delay -framedrop \\"
echo "    \"srt://<IP>:$SRT_PORT?mode=caller&latency=$LATENCY&tlpktdrop=1\""
echo
if [[ "$HOST" == "0.0.0.0" || "$HOST" == "::" ]]; then
    echo "TCP fallback:"
    echo "  ffplay -probesize 65536 -analyzeduration 100000 \\"
    echo "    -fflags nobuffer -flags low_delay -framedrop \\"
    echo "    tcp://<IP>:$PORT"
    echo
fi
echo "Press Ctrl+C to stop both processes."

# If either process exits, stop the other one through the EXIT trap and
# propagate the failure.
status=0
wait -n "$WV_PID" "$RELAY_PID" || status=$?
exit "$status"
