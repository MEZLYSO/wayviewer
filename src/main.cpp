#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 199309L
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>

#include <list>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <getopt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>
#include <gbm.h>
#include <fcntl.h>
#include <xf86drm.h>

#include "frame-writer.hpp"
#include "buffer-pool.hpp"
#include "tcp-server.hpp"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

#include "config.h"

#define MAX_FRAME_FAILURES 16

static const int GRACEFUL_TERMINATION_SIGNALS[] = { SIGTERM, SIGINT, SIGHUP };

std::mutex frame_writer_mutex, frame_writer_pending_mutex;
std::unique_ptr<FrameWriter> frame_writer;

static int drm_fd = -1;
static struct gbm_device *gbm_device = NULL;
static std::string drm_device_name;

static struct wl_shm *shm = NULL;
static struct zxdg_output_manager_v1 *xdg_output_manager = NULL;
static struct zwlr_screencopy_manager_v1 *screencopy_manager = NULL;
static struct zwp_linux_dmabuf_v1 *dmabuf = NULL;
void request_next_frame();

struct wf_recorder_output
{
    wl_output *output;
    zxdg_output_v1 *zxdg_output;
    std::string name, description;
    int32_t x, y, width, height;
    int32_t transform;
};

std::list<wf_recorder_output> available_outputs;

static void
display_handle_geometry(void *data,
                        wl_output *,
                        int32_t, int32_t,
                        int32_t,
                        int32_t,
                        int32_t,
                        const char *,
                        const char *,
                        int32_t transform)
{
    wf_recorder_output *wo = (wf_recorder_output*) data;

    wo->transform = transform;
}

static void
display_handle_mode(void *,
                    struct wl_output *,
                    uint32_t,
                    int32_t,
                    int32_t,
                    int32_t)
{
}

static void
display_handle_done(void *, wl_output *)
{
}

static void
display_handle_scale(void *,
                     wl_output *,
                     int32_t)
{
}

static void
display_handle_name(void *,
                    wl_output *,
                    const char *)
{
}

static void
display_handle_description(void *,
                           wl_output *,
                           const char *)
{
}

static const struct wl_output_listener output_listener = {
	display_handle_geometry,
	display_handle_mode,
	display_handle_done,
	display_handle_scale,
	display_handle_name,
	display_handle_description
};

static void handle_xdg_output_logical_position(void*,
    zxdg_output_v1* zxdg_output, int32_t x, int32_t y)
{
    for (auto& wo : available_outputs)
    {
        if (wo.zxdg_output == zxdg_output)
        {
            wo.x = x;
            wo.y = y;
        }
    }
}

static void handle_xdg_output_logical_size(void*,
    zxdg_output_v1* zxdg_output, int32_t w, int32_t h)
{
    for (auto& wo : available_outputs)
    {
        if (wo.zxdg_output == zxdg_output)
        {
            wo.width = w;
            wo.height = h;
        }
    }
}

static void handle_xdg_output_done(void*, zxdg_output_v1*) { }

static void handle_xdg_output_name(void*, zxdg_output_v1 *zxdg_output_v1,
    const char *name)
{
    for (auto& wo : available_outputs)
    {
        if (wo.zxdg_output == zxdg_output_v1)
            wo.name = name;
    }
}

static void handle_xdg_output_description(void*, zxdg_output_v1 *zxdg_output_v1,
    const char *description)
{
    for (auto& wo : available_outputs)
    {
        if (wo.zxdg_output == zxdg_output_v1)
            wo.description = description;
    }
}


const zxdg_output_v1_listener xdg_output_implementation = {
    .logical_position = handle_xdg_output_logical_position,
    .logical_size = handle_xdg_output_logical_size,
    .done = handle_xdg_output_done,
    .name = handle_xdg_output_name,
    .description = handle_xdg_output_description
};

struct wf_buffer : public buffer_pool_buf
{
    struct gbm_bo *bo = nullptr;
    zwp_linux_buffer_params_v1 *params = nullptr;
    struct wl_buffer *wl_buffer = nullptr;
    void *data = nullptr;
    size_t size = 0;
    enum wl_shm_format format;
    int drm_format;
    int width, height, stride;
    bool y_invert;

    timespec presented;
    uint64_t base_usec;
};

std::atomic<bool> exit_main_loop{false};

buffer_pool<wf_buffer, 2> buffers;

bool buffer_copy_done = false;

static int backingfile(off_t size)
{
    char name[] = "/tmp/wayviewer-shared-XXXXXX";
    int fd = mkstemp(name);
    if (fd < 0) {
        return -1;
    }

    int ret;
    while ((ret = ftruncate(fd, size)) == EINTR) {
        // No-op
    }
    if (ret < 0) {
        close(fd);
        return -1;
    }

    unlink(name);
    return fd;
}

static struct wl_buffer *create_shm_buffer(uint32_t fmt,
    int width, int height, int stride, void **data_out)
{
    int size = stride * height;

    int fd = backingfile(size);
    if (fd < 0) {
        fprintf(stderr, "creating a buffer file for %d B failed: %m\n", size);
        return NULL;
    }

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %m\n");
        close(fd);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    close(fd);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
        stride, fmt);
    wl_shm_pool_destroy(pool);

    *data_out = data;
    return buffer;
}

void free_shm_buffer(wf_buffer& buffer)
{
    if (buffer.wl_buffer == NULL)
    {
        return;
    }

    munmap(buffer.data, buffer.size);
    wl_buffer_destroy(buffer.wl_buffer);
    buffer.wl_buffer = NULL;
}

static bool use_dmabuf = false;
static bool use_hwupload = false;

static uint32_t wl_shm_to_drm_format(uint32_t format)
{
    if (format == WL_SHM_FORMAT_ARGB8888) {
        return GBM_FORMAT_ARGB8888;
    } else if (format == WL_SHM_FORMAT_XRGB8888) {
        return GBM_FORMAT_XRGB8888;
    } else {
        return format;
    }
}

static void frame_handle_buffer(void *, struct zwlr_screencopy_frame_v1 *frame, uint32_t format,
    uint32_t width, uint32_t height, uint32_t stride)
{
    if (use_dmabuf) {
        return;
    }

    auto& buffer = buffers.capture();
    auto old_format = buffer.format;
    buffer.format = (wl_shm_format)format;
    buffer.drm_format = wl_shm_to_drm_format(format);
    buffer.width = width;
    buffer.height = height;
    buffer.stride = stride;

    /* ffmpeg requires even width and height */
    if (buffer.width % 2)
        buffer.width -= 1;
    if (buffer.height % 2)
        buffer.height -= 1;

    if (!buffer.wl_buffer || old_format != format) {
        free_shm_buffer(buffer);
        buffer.wl_buffer =
            create_shm_buffer(format, width, height, stride, &buffer.data);
    }

    if (buffer.wl_buffer == NULL) {
        fprintf(stderr, "failed to create buffer\n");
        exit(EXIT_FAILURE);
    }

    zwlr_screencopy_frame_v1_copy(frame, buffer.wl_buffer);
}

static void frame_handle_flags(void*, struct zwlr_screencopy_frame_v1 *, uint32_t flags) {
    buffers.capture().y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

int32_t frame_failed_cnt = 0;

static void frame_handle_ready(void *, struct zwlr_screencopy_frame_v1 *,
    uint32_t tv_sec_hi, uint32_t tv_sec_low, uint32_t tv_nsec) {

    auto& buffer = buffers.capture();
    buffer_copy_done = true;
    buffer.presented.tv_sec = ((1ll * tv_sec_hi) << 32ll) | tv_sec_low;
    buffer.presented.tv_nsec = tv_nsec;
    frame_failed_cnt = 0;
}

static void frame_handle_failed(void *, struct zwlr_screencopy_frame_v1 *) {
    std::cerr << "Failed to copy frame, retrying..." << std::endl;
    ++frame_failed_cnt;
    request_next_frame();
    if (frame_failed_cnt > MAX_FRAME_FAILURES)
    {
        std::cerr << "Failed to copy frame too many times, exiting!" << std::endl;
        exit_main_loop = true;
    }
}

static void frame_handle_damage(void *, struct zwlr_screencopy_frame_v1 *,
    uint32_t, uint32_t, uint32_t, uint32_t)
{
}

static void dmabuf_created(void *data, struct zwp_linux_buffer_params_v1 *,
    struct wl_buffer *wl_buffer) {

    auto& buffer = buffers.capture();
    buffer.wl_buffer = wl_buffer;

    zwlr_screencopy_frame_v1 *frame = (zwlr_screencopy_frame_v1*) data;

    zwlr_screencopy_frame_v1_copy(frame, buffer.wl_buffer);
}

static void dmabuf_failed(void *, struct zwp_linux_buffer_params_v1 *) {
    std::cerr << "Failed to create dmabuf" << std::endl;
    exit_main_loop = true;
}

static const struct zwp_linux_buffer_params_v1_listener params_listener = {
    .created = dmabuf_created,
    .failed = dmabuf_failed,
};

static wl_shm_format drm_to_wl_shm_format(uint32_t format)
{
    if (format == GBM_FORMAT_ARGB8888) {
        return WL_SHM_FORMAT_ARGB8888;
    } else if (format == GBM_FORMAT_XRGB8888) {
        return WL_SHM_FORMAT_XRGB8888;
    } else {
        return (wl_shm_format)format;
    }
}

static void frame_handle_linux_dmabuf(void *, struct zwlr_screencopy_frame_v1 *frame,
    uint32_t format, uint32_t width, uint32_t height)
{
    if (!use_dmabuf) {
        return;
    }

    auto& buffer = buffers.capture();

    auto old_format = buffer.format;
    buffer.format = drm_to_wl_shm_format(format);
    buffer.drm_format = format;
    buffer.width = width;
    buffer.height = height;

    if (!buffer.wl_buffer || (old_format != buffer.format)) {
        if (buffer.bo) {
            if (buffer.wl_buffer) {
                wl_buffer_destroy(buffer.wl_buffer);
            }

            zwp_linux_buffer_params_v1_destroy(buffer.params);
            gbm_bo_destroy(buffer.bo);
        }

        const uint64_t modifier = 0; // DRM_FORMAT_MOD_LINEAR
        buffer.bo = gbm_bo_create_with_modifiers(gbm_device, buffer.width,
            buffer.height, format, &modifier, 1);
        if (buffer.bo == NULL)
        {
            buffer.bo = gbm_bo_create(gbm_device, buffer.width,
                buffer.height, format, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
        }
        if (buffer.bo == NULL)
        {
            std::cerr << "Failed to create gbm bo" << std::endl;
            exit_main_loop = true;
            return;
        }

        buffer.stride = gbm_bo_get_stride(buffer.bo);

        buffer.params = zwp_linux_dmabuf_v1_create_params(dmabuf);

        uint64_t mod = gbm_bo_get_modifier(buffer.bo);
        zwp_linux_buffer_params_v1_add(buffer.params,
            gbm_bo_get_fd(buffer.bo), 0,
            gbm_bo_get_offset(buffer.bo, 0),
            gbm_bo_get_stride(buffer.bo),
            mod >> 32, mod & 0xffffffff);

        zwp_linux_buffer_params_v1_add_listener(buffer.params, &params_listener, frame);
        zwp_linux_buffer_params_v1_create(buffer.params, buffer.width,
            buffer.height, format, 0);
    } else {
        zwlr_screencopy_frame_v1_copy(frame, buffer.wl_buffer);
    }
}

static void frame_handle_buffer_done(void *, struct zwlr_screencopy_frame_v1 *) {
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_handle_buffer,
    .flags = frame_handle_flags,
    .ready = frame_handle_ready,
    .failed = frame_handle_failed,
    .damage = frame_handle_damage,
    .linux_dmabuf = frame_handle_linux_dmabuf,
    .buffer_done = frame_handle_buffer_done,
};

static void dmabuf_feedback_done(void *, struct zwp_linux_dmabuf_feedback_v1 *feedback)
{
    zwp_linux_dmabuf_feedback_v1_destroy(feedback);
}

static void dmabuf_feedback_format_table(void *, struct zwp_linux_dmabuf_feedback_v1 *,
    int32_t fd, uint32_t)
{
    close(fd);
}

static void dmabuf_feedback_main_device(void *, struct zwp_linux_dmabuf_feedback_v1 *,
    struct wl_array *device)
{
    dev_t dev_id;
    memcpy(&dev_id, device->data, device->size);

    drmDevice *dev = NULL;
    if (drmGetDeviceFromDevId(dev_id, 0, &dev) != 0) {
        std::cerr << "Failed to get DRM device from dev id " << strerror(errno) << std::endl;
        return;
    }

    if (dev->available_nodes & (1 << DRM_NODE_RENDER)) {
        drm_device_name = dev->nodes[DRM_NODE_RENDER];
    } else if (dev->available_nodes & (1 << DRM_NODE_PRIMARY)) {
        drm_device_name = dev->nodes[DRM_NODE_PRIMARY];
    }

    drmFreeDevice(&dev);
}

static void dmabuf_feedback_tranche_done(void *, struct zwp_linux_dmabuf_feedback_v1 *)
{
}

static void dmabuf_feedback_tranche_target_device(void *, struct zwp_linux_dmabuf_feedback_v1 *,
    struct wl_array *)
{
}

static void dmabuf_feedback_tranche_formats(void *, struct zwp_linux_dmabuf_feedback_v1 *,
    struct wl_array *)
{
}

static void dmabuf_feedback_tranche_flags(void *, struct zwp_linux_dmabuf_feedback_v1 *,
    uint32_t)
{
}

static const struct zwp_linux_dmabuf_feedback_v1_listener dmabuf_feedback_listener = {
    .done = dmabuf_feedback_done,
    .format_table = dmabuf_feedback_format_table,
    .main_device = dmabuf_feedback_main_device,
    .tranche_done = dmabuf_feedback_tranche_done,
    .tranche_target_device = dmabuf_feedback_tranche_target_device,
    .tranche_formats = dmabuf_feedback_tranche_formats,
    .tranche_flags = dmabuf_feedback_tranche_flags,
};

static void handle_global(void*, struct wl_registry *registry,
    uint32_t name, const char *interface, uint32_t) {

    if (strcmp(interface, wl_output_interface.name) == 0)
    {
        auto output = (wl_output*)wl_registry_bind(registry, name, &wl_output_interface, 4);
        wf_recorder_output wro;
        wro.output = output;
        available_outputs.push_back(wro);
        wl_output_add_listener(output, &output_listener, &available_outputs.back());
    }
    else if (strcmp(interface, wl_shm_interface.name) == 0)
    {
        shm = (wl_shm*) wl_registry_bind(registry, name, &wl_shm_interface, 1);
    }
    else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0)
    {
        screencopy_manager = (zwlr_screencopy_manager_v1*) wl_registry_bind(registry, name,
            &zwlr_screencopy_manager_v1_interface, 3);
    }
    else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0)
    {
        xdg_output_manager = (zxdg_output_manager_v1*) wl_registry_bind(registry, name,
            &zxdg_output_manager_v1_interface, 2); // version 2 for name & description, if available
    }
    else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0)
    {
        dmabuf = (zwp_linux_dmabuf_v1*) wl_registry_bind(registry, name,
            &zwp_linux_dmabuf_v1_interface, 4);
        if (dmabuf) {
            struct zwp_linux_dmabuf_feedback_v1 *feedback =
                zwp_linux_dmabuf_v1_get_default_feedback(dmabuf);
            zwp_linux_dmabuf_feedback_v1_add_listener(feedback, &dmabuf_feedback_listener, NULL);
        }
    }
}

static void handle_global_remove(void*, struct wl_registry *, uint32_t) {
    // Who cares?
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static uint64_t timespec_to_usec (const timespec& ts)
{
    return ts.tv_sec * 1000000ll + 1ll * ts.tv_nsec / 1000ll;
}

static InputFormat get_input_format(wf_buffer& buffer)
{
    if (use_dmabuf && !use_hwupload) {
        return INPUT_FORMAT_DMABUF;
    }
    switch (buffer.format) {
    case WL_SHM_FORMAT_ARGB8888:
    case WL_SHM_FORMAT_XRGB8888:
        return INPUT_FORMAT_BGR0;
    case WL_SHM_FORMAT_XBGR8888:
    case WL_SHM_FORMAT_ABGR8888:
        return INPUT_FORMAT_RGB0;
    case WL_SHM_FORMAT_BGR888:
        return INPUT_FORMAT_BGR8;
    case WL_SHM_FORMAT_RGB565:
        return INPUT_FORMAT_RGB565;
    case WL_SHM_FORMAT_BGR565:
        return INPUT_FORMAT_BGR565;
    case WL_SHM_FORMAT_ARGB2101010:
    case WL_SHM_FORMAT_XRGB2101010:
        return INPUT_FORMAT_X2RGB10;
    case WL_SHM_FORMAT_ABGR2101010:
    case WL_SHM_FORMAT_XBGR2101010:
        return INPUT_FORMAT_X2BGR10;
    case WL_SHM_FORMAT_ABGR16161616:
    case WL_SHM_FORMAT_XBGR16161616:
        return INPUT_FORMAT_RGBX64;
    case WL_SHM_FORMAT_ARGB16161616:
    case WL_SHM_FORMAT_XRGB16161616:
        return INPUT_FORMAT_BGRX64;
    case WL_SHM_FORMAT_ABGR16161616F:
    case WL_SHM_FORMAT_XBGR16161616F:
        return INPUT_FORMAT_RGBX64F;
    default:
        fprintf(stderr, "Unsupported buffer format %d, exiting.", buffer.format);
        std::exit(0);
    }
}

static void write_loop(FrameWriterParams params)
{
    /* Ignore SIGTERM/SIGINT/SIGHUP, main loop is responsible for the exit_main_loop signal */
    sigset_t sigset;
    sigemptyset(&sigset);
    for (auto signo : GRACEFUL_TERMINATION_SIGNALS)
    {
        sigaddset(&sigset, signo);
    }
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);

    std::optional<uint64_t> first_frame_ts;

    while(!exit_main_loop)
    {
        // Wait for a frame to become available without busy polling.
        while (!exit_main_loop && !buffers.wait_for_encode(exit_main_loop))
        {
            // Keep waiting.
        }
        if (exit_main_loop) {
            break;
        }

        auto& buffer = buffers.encode();

        frame_writer_pending_mutex.lock();
        frame_writer_mutex.lock();
        frame_writer_pending_mutex.unlock();

        if (!frame_writer)
        {
            /* This is the first time buffer attributes are available */
            params.format = get_input_format(buffer);
            params.drm_format = buffer.drm_format;
            params.width = buffer.width;
            params.height = buffer.height;
            params.stride = buffer.stride;
            frame_writer = std::unique_ptr<FrameWriter> (new FrameWriter(params));
        }

        uint64_t sync_timestamp = 0;
        if (first_frame_ts.has_value()) {
            sync_timestamp = buffer.base_usec - first_frame_ts.value();
        } else {
            first_frame_ts = buffer.base_usec;
        }

        bool do_cont = false;

        if (use_dmabuf) {
            if (use_hwupload) {
                uint32_t stride = 0;
                void *map_data = NULL;
                void *data = gbm_bo_map(buffer.bo, 0, 0, buffer.width, buffer.height,
                    GBM_BO_TRANSFER_READ, &stride, &map_data);
                if (!data) {
                    std::cerr << "Failed to map bo" << std::endl;
                    break;
                }
                do_cont = frame_writer->add_frame((unsigned char*)data,
                    sync_timestamp, buffer.y_invert);
                gbm_bo_unmap(buffer.bo, map_data);
            } else {
                do_cont = frame_writer->add_frame(buffer.bo,
                    sync_timestamp, buffer.y_invert);
            }
        } else {
            do_cont = frame_writer->add_frame((unsigned char*)buffer.data,
                sync_timestamp, buffer.y_invert);
        }

        frame_writer_mutex.unlock();

        if (!do_cont) {
            exit_main_loop = true;
            break;
        }

        buffers.next_encode();
    }

    std::lock_guard<std::mutex> lock(frame_writer_mutex);
    frame_writer = nullptr;
}

void handle_graceful_termination(int)
{
    exit_main_loop = true;
}

static void check_has_protos()
{
    if (shm == NULL) {
        fprintf(stderr, "compositor is missing wl_shm\n");
        exit(EXIT_FAILURE);
    }
    if (screencopy_manager == NULL) {
        fprintf(stderr, "compositor doesn't support wlr-screencopy-unstable-v1\n");
        exit(EXIT_FAILURE);
    }

    if (xdg_output_manager == NULL)
    {
        fprintf(stderr, "compositor doesn't support xdg-output-unstable-v1\n");
        exit(EXIT_FAILURE);
    }

    if (use_dmabuf && dmabuf == NULL) {
        fprintf(stderr, "compositor doesn't support linux-dmabuf-unstable-v1\n");
        exit(EXIT_FAILURE);
    }

    if (available_outputs.empty())
    {
        fprintf(stderr, "no outputs available\n");
        exit(EXIT_FAILURE);
    }
}

wl_display *display = NULL;
static void sync_wayland()
{
    wl_display_dispatch(display);
    wl_display_roundtrip(display);
}

static void load_output_info()
{
    for (auto& wo : available_outputs)
    {
        wo.zxdg_output = zxdg_output_manager_v1_get_xdg_output(
            xdg_output_manager, wo.output);
        zxdg_output_v1_add_listener(wo.zxdg_output,
            &xdg_output_implementation, NULL);
    }

    sync_wayland();
}

static void help()
{
    printf(R"(Usage: wayviewer [OPTION]...

Streams the screen of a wlroots-based compositor as low-latency MPEG-TS.

  --port <port>      TCP port to listen on (default: 1234)
  --host <address>   Address to bind (default: 0.0.0.0)
  -h, --help         Show this help

Environment:
  WAYVIEWER_FPS           Capture/encode frame rate (default: 30)
  WAYVIEWER_BITRATE       Encoder bitrate cap in kbit/s (default: 5000)
  WAYVIEWER_GOP           Keyframe interval in frames (default: FPS)
  WAYVIEWER_CRF           x264/x265 quality, lower is better (default: 23)
  WAYVIEWER_PRESET        x264/x265 preset (default: ultrafast)
  WAYVIEWER_THREADS       Encoder threads, 0 = auto (default: 0)
  WAYVIEWER_UNIX_SOCKET   Also serve MPEG-TS on a Unix stream socket

On another device run:
  ffplay -probesize 65536 -analyzeduration 100000 \
    -fflags nobuffer -flags low_delay -framedrop tcp://<IP>:<port>

The probe flags are required for a fast start: FFmpeg's MPEG-TS demuxer
otherwise spends 5 seconds in avformat_find_stream_info() before playing.

Multiple clients can connect to the same port. When WAYVIEWER_UNIX_SOCKET
is set, a local process (for example Python) can read the same stream from
the Unix socket. For SRT and multiple clients, see
python/wayviewer_relay.py.

Press Ctrl+C to stop.
)");
    exit(EXIT_SUCCESS);
}

wf_recorder_output *chosen_output = nullptr;
zwlr_screencopy_frame_v1 *frame = NULL;

void request_next_frame()
{
    if (frame != NULL)
    {
        zwlr_screencopy_frame_v1_destroy(frame);
    }

    frame = zwlr_screencopy_manager_v1_capture_output(
        screencopy_manager, 1, chosen_output->output);

    zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, NULL);
}

static void init_wayland_client()
{
    display = wl_display_connect(NULL);
    if (display == NULL)
    {
        fprintf(stderr, "failed to create display: %m\n");
        exit(EXIT_FAILURE);
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    sync_wayland();
}

static int env_int(const char* name, int fallback, int minimum, int maximum)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0')
        return fallback;

    char* end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0')
    {
        std::cerr << "Ignoring invalid " << name << "=" << value << std::endl;
        return fallback;
    }

    if (parsed < minimum)
        parsed = minimum;
    if (parsed > maximum)
        parsed = maximum;

    return static_cast<int>(parsed);
}

static std::string env_string(const char* name, const std::string& fallback)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0')
        return fallback;
    return value;
}

int main(int argc, char *argv[])
{
    const int target_fps = env_int("WAYVIEWER_FPS", 30, 1, 240);
    const int target_bitrate = env_int("WAYVIEWER_BITRATE", 5000, 100, 100000);
    const int gop_size = env_int("WAYVIEWER_GOP", target_fps, 1, 3600);
    const int target_crf = env_int("WAYVIEWER_CRF", 23, 0, 51);
    int target_threads = env_int("WAYVIEWER_THREADS", 0, 0, 64);
    if (target_threads == 0)
    {
        // Half of the logical CPUs keeps latency low without oversubscribing
        // the encoder with more slices than the frame needs.
        unsigned hw = std::thread::hardware_concurrency();
        target_threads = hw > 1 ? static_cast<int>(hw / 2) : 1;
        if (target_threads > 8)
            target_threads = 8;
    }
    const std::string preset = env_string("WAYVIEWER_PRESET", "ultrafast");

    FrameWriterParams params = FrameWriterParams(exit_main_loop);
    params.codec = DEFAULT_CODEC;
    params.pix_fmt = DEFAULT_PIX_FMT;
    params.enable_ffmpeg_debug_output = false;
    params.framerate = target_fps;
    params.buffrate = target_fps;
    params.bframes = 0;

    // Low-latency x264/x265 defaults.
    if (params.codec.find("libx264") != std::string::npos)
    {
        params.codec_options["tune"] = "zerolatency";
        params.codec_options["preset"] = preset;
        params.codec_options["crf"] = std::to_string(target_crf);
        params.codec_options["g"] = std::to_string(gop_size);
        params.codec_options["keyint_min"] = std::to_string(gop_size);
        // Keep the encoder latency deterministic and cap bitrate spikes so a
        // congested Wi-Fi link is not flooded by keyframes/scene changes.
        params.codec_options["scenecut"] = "0";
        params.codec_options["rc-lookahead"] = "0";
        params.codec_options["sync-lookahead"] = "0";
        params.codec_options["sliced-threads"] = "1";
        params.codec_options["bframes"] = "0";
        params.codec_options["ref"] = "1";
        params.codec_options["maxrate"] = std::to_string(target_bitrate) + "k";
        params.codec_options["bufsize"] = std::to_string(target_bitrate / 2) + "k";
        params.codec_options["vbv-init"] = "0.5";
        if (target_threads > 0)
            params.codec_options["threads"] = std::to_string(target_threads);
    }
    else if (params.codec.find("libx265") != std::string::npos)
    {
        params.codec_options["tune"] = "zerolatency";
        params.codec_options["preset"] = preset;
        params.codec_options["crf"] = std::to_string(target_crf);
        params.codec_options["g"] = std::to_string(gop_size);
        params.codec_options["maxrate"] = std::to_string(target_bitrate) + "k";
        params.codec_options["bufsize"] = std::to_string(target_bitrate / 2) + "k";
        params.codec_options["bframes"] = "0";
        params.codec_options["frame-threads"] = "1";
        if (target_threads > 0)
            params.codec_options["pools"] = std::to_string(target_threads);
    }

    std::string host = "0.0.0.0";
    int port = 1234;

    static const struct option opts[] = {
        { "port", required_argument, NULL, 'p' },
        { "host", required_argument, NULL, 'H' },
        { "bind", required_argument, NULL, 'H' },
        { "help", no_argument,       NULL, 'h' },
        { 0,      0,                 NULL,  0  }
    };

    int c;
    while ((c = getopt_long(argc, argv, "p:H:h", opts, NULL)) != -1)
    {
        switch (c)
        {
            case 'p':
                port = atoi(optarg);
                break;

            case 'H':
                host = optarg;
                break;

            case 'h':
                help();
                break;

            default:
                help();
        }
    }

    if (argc > optind)
    {
        std::cerr << "Unexpected argument: " << argv[optind] << std::endl;
        return EXIT_FAILURE;
    }

    if (port < 1 || port > 65535)
    {
        std::cerr << "Invalid port: " << port << std::endl;
        return EXIT_FAILURE;
    }

    std::string bind_host = host;
    if (bind_host.size() >= 2 && bind_host.front() == '[' && bind_host.back() == ']')
        bind_host = bind_host.substr(1, bind_host.size() - 2);

    const char* unix_socket_env = std::getenv("WAYVIEWER_UNIX_SOCKET");
    const std::string unix_socket = unix_socket_env ? unix_socket_env : "";

    TcpServer server(bind_host, port, unix_socket);
    if (!server.start())
        return EXIT_FAILURE;

    params.server = &server;

    std::cerr << "Streaming on tcp://" << host << ":" << port << std::endl;
    if (!unix_socket.empty())
        std::cerr << "Streaming on unix://" << unix_socket << std::endl;
    std::cerr << "Connect with: ffplay tcp://<IP>:" << port << std::endl;

    init_wayland_client();
    check_has_protos();
    load_output_info();

    if (available_outputs.empty())
    {
        std::cerr << "No outputs available" << std::endl;
        return EXIT_FAILURE;
    }

    chosen_output = &available_outputs.front();
    params.transform = chosen_output->transform;

    bool spawned_thread = false;
    std::thread writer_thread;

    for (auto signo : GRACEFUL_TERMINATION_SIGNALS)
        signal(signo, handle_graceful_termination);

    const auto frame_interval = std::chrono::microseconds(1000000 / target_fps);
    auto next_frame_time = std::chrono::steady_clock::now();

    while (!exit_main_loop)
    {
        // Wait for a free buffer without busy polling.
        while (!exit_main_loop && !buffers.wait_for_capture(exit_main_loop))
        {
            // Keep waiting.
        }
        if (exit_main_loop)
            break;

        buffer_copy_done = false;
        request_next_frame();

        while (!buffer_copy_done && !exit_main_loop)
        {
            if (wl_display_dispatch(display) == -1)
            {
                if (!exit_main_loop)
                {
                    std::cerr << "wl_display_dispatch failed: "
                        << std::strerror(errno) << std::endl;
                    exit_main_loop = true;
                }
                break;
            }
        }

        if (exit_main_loop)
            break;

        auto& buffer = buffers.capture();

        if (!spawned_thread)
        {
            writer_thread = std::thread([=]() { write_loop(params); });
            spawned_thread = true;
        }

        buffer.base_usec = timespec_to_usec(buffer.presented);
        buffers.next_capture();

        // Pace capture instead of capturing as fast as the compositor allows
        // and letting an fps filter drop/duplicate frames. This keeps the
        // encoder and the network at a predictable rate.
        next_frame_time += frame_interval;
        const auto now = std::chrono::steady_clock::now();
        if (next_frame_time < now)
            next_frame_time = now;
        else
            std::this_thread::sleep_until(next_frame_time);
    }

    buffers.notify_all();

    if (writer_thread.joinable())
        writer_thread.join();

    server.stop();

    for (size_t i = 0; i < buffers.size(); ++i)
    {
        auto buffer = buffers.at(i);
        if (buffer && buffer->wl_buffer)
            wl_buffer_destroy(buffer->wl_buffer);
    }

    if (gbm_device)
    {
        gbm_device_destroy(gbm_device);
        close(drm_fd);
    }

    return EXIT_SUCCESS;
}
