# wayviewer en Arch Linux / CachyOS

Guía para compilar y ejecutar `wayviewer` junto con el relay SRT de Python en
Arch Linux y distribuciones derivadas como CachyOS.

`wayviewer` captura la pantalla de un compositor basado en `wlroots` (sway,
dwl, Hyprland, etc.) y la envía como MPEG-TS de baja latencia. Solo video: no
captura audio ni graba archivos.

## Requisitos

- Sesión Wayland con `wlr-screencopy-v1` y `xdg-output`.
  Funciona con sway, dwl y otros compositores `wlroots`.
  GNOME y KDE **no** son compatibles.
- `WAYLAND_DISPLAY` definido (ejecuta desde dentro de la sesión gráfica).

## 1. Instalar dependencias

```bash
sudo pacman -S --needed base-devel meson ninja pkgconf \
  wayland wayland-protocols mesa libdrm ffmpeg x264 srt python
```

Qué aporta cada paquete:

| Paquete | Uso |
|---|---|
| `base-devel` | `gcc`, `make` y herramientas básicas |
| `meson`, `ninja`, `pkgconf` | sistema de build |
| `wayland` | cabeceras de Wayland y `wayland-scanner` |
| `wayland-protocols` | protocolos `wlr-screencopy` y `xdg-output` |
| `mesa` | `libgbm` (`gbm.pc`) |
| `libdrm` | cabeceras DRM |
| `ffmpeg` | `libavcodec`, `libavformat`, `libavfilter`, `libavutil` |
| `x264` | encoder H.264 |
| `srt` | `libsrt` para el relay SRT |
| `python` | relay `python/wayviewer_relay.py` |

> El paquete de PyPI llamado `srt` **no** es necesario: es para subtítulos
> SubRip. El relay usa `ctypes` contra la `libsrt` del sistema.

Opcional para probar en el mismo equipo:

```bash
sudo pacman -S --needed ffmpeg   # incluye ffplay
```

## 2. Compilar

```bash
chmod +x build-cachyos.sh start-cachyos.sh
./build-cachyos.sh
```

El binario queda en `build/wayviewer`.

Opciones del script:

```bash
./build-cachyos.sh --clean        # borra build/ y recompila
./build-cachyos.sh --debug        # build con símbolos de depuración
./build-cachyos.sh --install      # instala en /usr (requiere sudo)
./build-cachyos.sh --run          # compila y arranca start-cachyos.sh
```

Compilación manual equivalente:

```bash
meson setup build --buildtype=release
ninja -C build
```

Recompilar tras cambiar código:

```bash
ninja -C build
```

## 3. Ejecutar wayviewer + relay Python

```bash
./start-cachyos.sh
```

El script arranca los dos procesos y los detiene juntos con `Ctrl+C`:

1. `wayviewer` en `tcp://127.0.0.1:12401` con un socket Unix en
   `${XDG_RUNTIME_DIR:-/tmp}/wayviewer-12401.sock`.
2. El relay SRT escuchando en `srt://0.0.0.0:9000`.

El socket Unix lleva el puerto TCP en el nombre para que dos instancias en
puertos distintos no se pisen. Puedes cambiarlo con
`WAYVIEWER_UNIX_SOCKET=/ruta/al/socket`.

Al arrancar imprime las IPs locales y el comando exacto para el cliente.

### Cliente SRT (recomendado)

Desde otro equipo:

```bash
ffplay -probesize 65536 -analyzeduration 100000 \
  -fflags nobuffer -flags low_delay -framedrop \
  "srt://IP_DEL_SERVIDOR:9000?mode=caller&latency=120&tlpktdrop=1"
```

Los flags `-probesize` y `-analyzeduration` son importantes: sin ellos el
demuxer MPEG-TS de FFmpeg tarda unos 5 segundos en mostrar el primer frame.

### Fallback TCP

Si prefieres TCP directo, expón también el puerto TCP:

```bash
HOST=0.0.0.0 ./start-cachyos.sh
```

Y en el cliente:

```bash
ffplay -probesize 65536 -analyzeduration 100000 \
  -fflags nobuffer -flags low_delay -framedrop \
  tcp://IP_DEL_SERVIDOR:12401
```

## Configuración

Variables del script `start-cachyos.sh`:

| Variable | Defecto | Significado |
|---|---:|---|
| `HOST` | `127.0.0.1` | dirección de bind TCP |
| `PORT` | `12401` | puerto TCP (fallback) |
| `SRT_BIND` | `0.0.0.0` | dirección de bind SRT |
| `SRT_PORT` | `9000` | puerto UDP SRT |
| `LATENCY` | `120` | latencia SRT en ms |
| `MAXBW` | `8000` | ancho de banda máximo por cliente, kbit/s |

Variables del encoder (las hereda `wayviewer`):

| Variable | Defecto | Significado |
|---|---:|---|
| `WAYVIEWER_FPS` | `30` | frames por segundo |
| `WAYVIEWER_BITRATE` | `5000` | bitrate máximo, kbit/s |
| `WAYVIEWER_GOP` | `FPS` | intervalo de keyframes |
| `WAYVIEWER_CRF` | `23` | calidad x264/x265 (menor = mejor) |
| `WAYVIEWER_PRESET` | `ultrafast` | preset x264/x265 |
| `WAYVIEWER_THREADS` | `0` | hilos del encoder; `0` = mitad de CPUs lógicas |

Ejemplos:

```bash
# Menos CPU
WAYVIEWER_THREADS=2 ./start-cachyos.sh

# Menos ancho de banda
WAYVIEWER_BITRATE=2500 ./start-cachyos.sh

# Wi-Fi muy pérdido: más margen para retransmisiones SRT
LATENCY=200 ./start-cachyos.sh

# Otro puerto SRT
SRT_PORT=9100 ./start-cachyos.sh
```

Si subes `LATENCY` en el servidor, usa el mismo valor en la URL del cliente.

## Firewall

El relay usa UDP. Si tienes firewall activo, abre el puerto SRT:

```bash
# ufw
sudo ufw allow 9000/udp

# firewalld
sudo firewall-cmd --add-port=9000/udp --permanent
sudo firewall-cmd --reload

# nftables
sudo nft add rule inet filter input udp dport 9000 accept
```

Si usas el fallback TCP, abre también `12401/tcp`.

Comprobar que escucha:

```bash
ss -lunp | grep 9000     # SRT
ss -ltnp | grep 12401    # TCP (solo si HOST=0.0.0.0)
```

## Problemas comunes

**`compositor doesn't support wlr-screencopy-unstable-v1`**
Estás en GNOME, KDE o un compositor sin `wlr-screencopy`. Usa sway, dwl,
Hyprland u otro compositor `wlroots`.

**`Failed to create Unix socket` o no arranca**
Comprueba que ejecutas dentro de la sesión Wayland:

```bash
echo "$WAYLAND_DISPLAY"
```

**El cliente tarda ~5 segundos en empezar**
Faltan `-probesize 65536 -analyzeduration 100000` en `ffplay`.

**`libsrt not found`**
```bash
sudo pacman -S srt
```

**Se ve entrecortado o con artefactos**
Sube la latencia SRT en ambos lados:

```bash
LATENCY=200 ./start-cachyos.sh
# y en el cliente: ...&latency=200&tlpktdrop=1
```

También puedes bajar `WAYVIEWER_BITRATE` si el Wi-Fi está muy congestionado.

**Mucho consumo de CPU**
```bash
WAYVIEWER_THREADS=2 WAYVIEWER_PRESET=ultrafast ./start-cachyos.sh
```

**Puerto ocupado**
Cambia `PORT` y/o `SRT_PORT`, o comprueba qué proceso lo usa:

```bash
ss -lunp | grep 9000
```

## Notas sobre CachyOS

CachyOS es compatible con Arch, así que los mismos paquetes y comandos
funcionan. No hacen falta repositorios adicionales. Si usas un helper como
`paru` o `yay`, puedes instalar igualmente con `pacman`; no se necesita nada
del AUR para compilar `wayviewer`.

## Estructura rápida

```text
build-cachyos.sh    compila wayviewer
start-cachyos.sh    arranca wayviewer + relay SRT
build/wayviewer     binario
python/wayviewer_relay.py  relay SRT (solo stdlib + libsrt)
```
