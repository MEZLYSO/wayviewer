#!/usr/bin/env bash
#
# Build wayviewer on Arch Linux / CachyOS.
#
# Usage:
#   ./build-cachyos.sh
#   ./build-cachyos.sh --clean
#   ./build-cachyos.sh --debug
#   ./build-cachyos.sh --install
#   ./build-cachyos.sh --run
#
# Environment:
#   BUILD_DIR    Build directory            (default: build)
#   BUILD_TYPE   Meson build type           (default: release)
#   PREFIX       Install prefix             (default: /usr)
#   MESON_ARGS   Extra arguments for meson setup
#   NINJA_ARGS   Extra arguments for ninja

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-release}"
PREFIX="${PREFIX:-/usr}"
MESON_ARGS="${MESON_ARGS:-}"
NINJA_ARGS="${NINJA_ARGS:-}"

CLEAN=0
INSTALL=0
RECONFIGURE=0
RUN=0

usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Build wayviewer on Arch Linux / CachyOS.

Options:
  --clean        Remove the build directory before building
  --debug        Build with debug symbols (buildtype=debug)
  --release      Build optimized (buildtype=release, default)
  --install      Run ninja install after a successful build
  --reconfigure  Reconfigure an existing build directory
  --run          Start ./start-cachyos.sh after a successful build
  -h, --help     Show this help

Environment:
  BUILD_DIR      Build directory (default: build)
  BUILD_TYPE     Meson build type (default: release)
  PREFIX         Install prefix (default: /usr)
  MESON_ARGS     Extra arguments passed to meson setup
  NINJA_ARGS     Extra arguments passed to ninja

Examples:
  ./build-cachyos.sh
  ./build-cachyos.sh --clean --debug
  PREFIX=/usr/local ./build-cachyos.sh --install
  ./build-cachyos.sh --run
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean)
            CLEAN=1
            shift
            ;;
        --debug)
            BUILD_TYPE=debug
            shift
            ;;
        --release)
            BUILD_TYPE=release
            shift
            ;;
        --install)
            INSTALL=1
            shift
            ;;
        --reconfigure)
            RECONFIGURE=1
            shift
            ;;
        --run)
            RUN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

install_hint() {
    cat <<'EOF'
  sudo pacman -S --needed base-devel meson ninja pkgconf \
    wayland wayland-protocols mesa libdrm ffmpeg x264 srt python
EOF
}

check_tools() {
    local missing=()

    for cmd in meson ninja pkg-config c++ wayland-scanner; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            missing+=("$cmd")
        fi
    done

    if (( ${#missing[@]} > 0 )); then
        echo "Missing build tools: ${missing[*]}" >&2
        echo "Install them with:" >&2
        install_hint >&2
        exit 1
    fi
}

check_dependencies() {
    local missing=()
    local packages=(
        wayland-client
        wayland-protocols
        libavutil
        libavcodec
        libavformat
        libavfilter
        gbm
        libdrm
    )

    for pkg in "${packages[@]}"; do
        if ! pkg-config --exists "$pkg"; then
            missing+=("$pkg")
        fi
    done

    if (( ${#missing[@]} > 0 )); then
        echo "Missing build dependencies: ${missing[*]}" >&2
        echo "Install them with:" >&2
        install_hint >&2
        exit 1
    fi
}

check_tools
check_dependencies

if [[ "$CLEAN" == "1" ]]; then
    echo "Cleaning $BUILD_DIR..."
    rm -rf "$BUILD_DIR"
fi

# Remove a stale/incomplete build directory before configuring.
if [[ ! -f "$BUILD_DIR/build.ninja" && -d "$BUILD_DIR" ]]; then
    echo "Removing incomplete build directory..."
    rm -rf "$BUILD_DIR"
fi

if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
    echo "Configuring $BUILD_DIR (buildtype=$BUILD_TYPE, prefix=$PREFIX)..."
    # shellcheck disable=SC2086
    meson setup "$BUILD_DIR" \
        --buildtype="$BUILD_TYPE" \
        --prefix="$PREFIX" \
        $MESON_ARGS
elif [[ "$RECONFIGURE" == "1" ]]; then
    echo "Reconfiguring $BUILD_DIR (buildtype=$BUILD_TYPE, prefix=$PREFIX)..."
    # shellcheck disable=SC2086
    meson setup --reconfigure "$BUILD_DIR" \
        --buildtype="$BUILD_TYPE" \
        --prefix="$PREFIX" \
        $MESON_ARGS
fi

echo "Building..."
# shellcheck disable=SC2086
ninja -C "$BUILD_DIR" $NINJA_ARGS

BIN="$SCRIPT_DIR/$BUILD_DIR/wayviewer"
if [[ ! -x "$BIN" ]]; then
    echo "Build finished but $BIN was not produced." >&2
    exit 1
fi

echo
echo "Build complete: $BIN"
echo "Run it with: ./$BUILD_DIR/wayviewer --help"
echo "Start streaming with: ./start-cachyos.sh"

if [[ "$INSTALL" == "1" ]]; then
    echo
    echo "Installing to $PREFIX (may require sudo)..."
    ninja -C "$BUILD_DIR" install
fi

if [[ "$RUN" == "1" ]]; then
    echo
    exec "$SCRIPT_DIR/start-cachyos.sh"
fi
