# wayviewer

Stream the screen of a `wlroots`-based compositor over TCP as MPEG-TS, with an optional Unix-socket output for a local Python process. Video only: no audio, no file recording.

The compositor must support `wlr-screencopy-v1` and `xdg-output`.

# Build

## Packages

```bash
meson ninja gcc pkg-config wayland-devel \
wayland-protocols libgbm-devel libdrm-devel ffmpeg6-devel x264-devel

./build.sh
```

Other distributions: install the equivalent `wayland`, `libav*`, `gbm`, `libdrm` and `x264` development packages, then run `./build.sh`.

Arch Linux / CachyOS: see [README-Archlinux.md](README-Archlinux.md) and use `./build-cachyos.sh` / `./start-cachyos.sh`.

Manual build:

```bash
meson setup build --buildtype=release
ninja -C build
```

# Usage

```bash
./build/wayviewer
```

The server listens on `0.0.0.0:1234` by default. The only options are the port and the bind host:

```bash
./build/wayviewer --port 9000
./build/wayviewer --host 192.168.1.10 --port 9000
```

Or use the script:

```bash
./start.sh
PORT=9000 ./start.sh
HOST=192.168.1.10 PORT=9000 ./start.sh
```

On another device:

```bash
ffplay -probesize 65536 -analyzeduration 100000 \
  -fflags nobuffer -flags low_delay -framedrop \
  tcp://IP_OF_SERVER:1234
```

`-probesize` / `-analyzeduration` are important: FFmpeg's MPEG-TS demuxer
sets `AVFMTCTX_NOHEADER`, so `avformat_find_stream_info()` normally waits the
full 5-second analyze window before the first frame is shown. With these
flags playback starts in about half a second.

Multiple clients can connect to the same port at the same time. Each client receives the same stream independently; a slow or disconnected client does not stop the others.

Press Ctrl+C to stop the server.

# Defaults

The stream is tuned for low latency:

- MPEG-TS over TCP with `tcp_nodelay`.
- x264 `tune=zerolatency`, `preset=ultrafast`, `crf=23`, sliced threads.
- No B-frames, 30 fps, GOP of 30.
- Rate capped at 5000 kbit/s with a 2500 kbit VBV buffer.
- Immediate packet flushing and zero mux delay.
- PAT/PMT repeated before every keyframe, so a client that joins mid-stream
  can start on the next IDR without a separate PSI prefix.
- Double-buffered capture with condition variables (no busy polling).
- Small per-client queues and send buffers; slow clients are disconnected.
- A new client triggers an IDR frame immediately, so it can start decoding
  without waiting for the next scheduled keyframe.
- Encoder threads default to half the logical CPUs (capped at 8), which keeps
  per-frame latency low without oversubscribing the CPU.
- Captures the whole first output continuously.

# Tuning

The defaults are conservative for a congested Wi-Fi network. You can override
them with environment variables without changing the CLI:

```bash
WAYVIEWER_FPS=60 ./build/wayviewer
WAYVIEWER_BITRATE=8000 ./build/wayviewer
WAYVIEWER_GOP=60 ./build/wayviewer
```

| Variable | Default | Meaning |
|---|---:|---|
| `WAYVIEWER_FPS` | `30` | Capture and encode frame rate |
| `WAYVIEWER_BITRATE` | `5000` | Encoder bitrate cap in kbit/s |
| `WAYVIEWER_GOP` | `FPS` | Keyframe interval in frames (1 second) |
| `WAYVIEWER_CRF` | `23` | x264/x265 quality, lower is better |
| `WAYVIEWER_PRESET` | `ultrafast` | x264/x265 preset |
| `WAYVIEWER_THREADS` | `0` | Encoder threads; `0` = half the logical CPUs (max 8) |
| `WAYVIEWER_UNIX_SOCKET` | unset | Also serve the stream on a Unix socket |

`WAYVIEWER_THREADS` controls x264 sliced threads. Lower values use less CPU;
higher values shorten per-frame encode time. On a 4-core/8-thread CPU the
automatic value is 4. `WAYVIEWER_PRESET=superfast` or `veryfast` improves
quality at the cost of CPU.

For two clients on a congested network, remember that TCP broadcast sends one
copy per client: 5000 kbit/s per client means up to ~10 Mbit/s total.

# Unix socket / Python integration

`wayviewer` can keep serving TCP and, at the same time, expose the same
MPEG-TS byte stream on a Unix stream socket. This is intended for a local
Python process that handles client transport (for example SRT) or a GUI:

```bash
WAYVIEWER_UNIX_SOCKET=/run/user/1000/wayviewer.sock ./build/wayviewer
```

Then a Python client can connect with `socket.AF_UNIX` and read MPEG-TS bytes:

```python
import socket

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect("/run/user/1000/wayviewer.sock")
while True:
    chunk = sock.recv(65536)
    if not chunk:
        break
    # Forward opaque MPEG-TS bytes; do not parse them per frame.
```

The Unix socket uses the same broadcast path as TCP: all connected readers get
the same bytes, and a slow reader is disconnected instead of blocking the
encoder. The socket file is removed when the server exits.

TCP remains available when the Unix socket is enabled. If you do not need it,
bind it to the loopback interface with `--host 127.0.0.1`.

The stream is not encrypted or authenticated. Use it on a LAN or through a
VPN/SSH tunnel.

# SRT relay (recommended for Wi-Fi)

For more than one client, or for a congested Wi-Fi network, use the Python
relay. It reads the Unix socket once and sends the already-encoded MPEG-TS
stream to each SRT client independently. A slow client is resynchronized at
the next keyframe instead of stalling the others.

```bash
WAYVIEWER_UNIX_SOCKET=/run/user/1000/wayviewer.sock ./build/wayviewer --host 127.0.0.1
python3 python/wayviewer_relay.py \
  --unix-socket /run/user/1000/wayviewer.sock \
  --srt-port 9000 --latency 120 --maxbw 8000
```

On another device:

```bash
ffplay -probesize 65536 -analyzeduration 100000 \
  -fflags nobuffer -flags low_delay -framedrop \
  "srt://IP_OF_SERVER:9000?mode=caller&latency=120&tlpktdrop=1"
```

Notes:

- `latency=120` keeps the delay low. On a very lossy Wi-Fi link, raise it to
  `200`–`300` on both the relay (`--latency`) and the client; SRT will recover
  more lost packets before dropping them.
- `maxbw` is a per-client cap. The stream itself is usually far below it.
- New clients wait for an IDR frame, and the relay asks `wayviewer` for one
  immediately, so joining does not require replaying a cached GOP.
- The relay reconnects to the Unix socket automatically if `wayviewer`
  restarts.
