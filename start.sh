#!/usr/bin/env bash
#
# Start wayviewer, a video-only TCP streaming server.
#
# Usage:
#   ./start.sh
#   PORT=9000 ./start.sh
#   HOST=192.168.1.10 PORT=9000 ./start.sh
#   ./start.sh --port 9000 --host 192.168.1.10
#
# Environment:
#   PORT       TCP port to listen on   (default: 1234)
#   HOST       Address to bind         (default: 0.0.0.0)
#   BUILD_DIR  Build directory         (default: build)
#   NO_BUILD   Set to 1 to never build (default: 0)
#   WAYVIEWER_FPS           Frame rate              (default: 30)
#   WAYVIEWER_BITRATE       Bitrate cap in kbit/s   (default: 5000)
#   WAYVIEWER_GOP           Keyframe interval       (default: FPS)
#   WAYVIEWER_CRF           x264/x265 quality       (default: 23)
#   WAYVIEWER_PRESET        x264/x265 preset        (default: ultrafast)
#   WAYVIEWER_THREADS       Encoder threads, 0=auto (default: 0)
#   WAYVIEWER_UNIX_SOCKET   Extra Unix socket path  (default: unset)

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

PORT="${PORT:-1234}"
HOST="${HOST:-0.0.0.0}"
BUILD_DIR="${BUILD_DIR:-build}"
NO_BUILD="${NO_BUILD:-0}"

BIN="$SCRIPT_DIR/$BUILD_DIR/wayviewer"

usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Start wayviewer as a video-only TCP streaming server.

Options:
  --port <port>   TCP port to listen on   (default: 1234)
  --host <addr>   Address to bind         (default: 0.0.0.0)
  --no-build      Do not auto-build if the binary is missing
  -h, --help      Show this help

Environment:
  PORT       TCP port to listen on   (default: 1234)
  HOST       Address to bind         (default: 0.0.0.0)
  BUILD_DIR  Build directory         (default: build)
  NO_BUILD   Set to 1 to never build (default: 0)
  WAYVIEWER_FPS           Frame rate              (default: 30)
  WAYVIEWER_BITRATE       Bitrate cap in kbit/s   (default: 5000)
  WAYVIEWER_GOP           Keyframe interval       (default: FPS)
  WAYVIEWER_CRF           x264/x265 quality       (default: 23)
  WAYVIEWER_PRESET        x264/x265 preset        (default: ultrafast)
  WAYVIEWER_THREADS       Encoder threads, 0=auto (default: 0)
  WAYVIEWER_UNIX_SOCKET   Extra Unix socket path  (default: unset)

Examples:
  ./start.sh
  PORT=9000 ./start.sh
  HOST=192.168.1.10 PORT=9000 ./start.sh
  WAYVIEWER_UNIX_SOCKET=/run/user/1000/wayviewer.sock ./start.sh
EOF
}

EXTRA_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --no-build)
            NO_BUILD=1
            shift
            ;;
        --port)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --port" >&2
                exit 1
            fi
            PORT="$2"
            shift 2
            ;;
        --host|--bind)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for $1" >&2
                exit 1
            fi
            HOST="$2"
            shift 2
            ;;
        *)
            EXTRA_ARGS+=("$1")
            shift
            ;;
    esac
done

if [[ ! "$PORT" =~ ^[0-9]+$ ]] || (( PORT < 1 || PORT > 65535 )); then
    echo "Invalid port: $PORT (expected 1-65535)" >&2
    exit 1
fi

if [[ ! -x "$BIN" ]]; then
    if [[ "$NO_BUILD" == "1" ]]; then
        echo "Binary not found: $BIN" >&2
        echo "Run ./build.sh first." >&2
        exit 1
    fi

    echo "wayviewer binary not found, building..."
    if [[ -x "$SCRIPT_DIR/build.sh" ]]; then
        BUILD_DIR="$BUILD_DIR" "$SCRIPT_DIR/build.sh"
    else
        meson setup "$BUILD_DIR" --buildtype=release
        ninja -C "$BUILD_DIR"
    fi
fi

if [[ ! -x "$BIN" ]]; then
    echo "Binary not found after build: $BIN" >&2
    exit 1
fi

echo "Starting wayviewer on ${HOST}:${PORT}"
if [[ -n "${WAYVIEWER_UNIX_SOCKET:-}" ]]; then
    echo "Unix socket: ${WAYVIEWER_UNIX_SOCKET}"
fi
echo
echo "Local IP addresses:"
if command -v ip >/dev/null 2>&1; then
    ip -o -4 addr show scope global 2>/dev/null \
        | awk '{print $4}' | cut -d/ -f1 | sed 's/^/  /' || true
elif command -v hostname >/dev/null 2>&1; then
    hostname -I 2>/dev/null | tr ' ' '\n' | sed '/^$/d; s/^/  /' || true
fi
echo
echo "On the other device, connect with:"
echo "  ffplay -probesize 65536 -analyzeduration 100000 \\"
echo "    -fflags nobuffer -flags low_delay -framedrop tcp://<IP>:${PORT}"
echo
echo "Multiple clients can connect to the same port."
echo
echo "If the connection is blocked, allow TCP port ${PORT} in your firewall:"
if command -v ufw >/dev/null 2>&1; then
    echo "  sudo ufw allow ${PORT}/tcp"
elif command -v firewall-cmd >/dev/null 2>&1; then
    echo "  sudo firewall-cmd --add-port=${PORT}/tcp"
else
    echo "  nft:      sudo nft add rule inet filter input tcp dport ${PORT} accept"
    echo "  iptables: sudo iptables -A INPUT -p tcp --dport ${PORT} -j ACCEPT"
fi
echo
echo "Press Ctrl+C to stop the server."
echo

exec "$BIN" --port "$PORT" --host "$HOST" "${EXTRA_ARGS[@]}"
