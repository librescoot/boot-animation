#define _GNU_SOURCE

/*
 * boot-animation: Lottie animation renderer for /dev/fb0
 *
 * Renders a Lottie JSON animation directly to the framebuffer using ThorVG's
 * software renderer. Designed for embedded boot splash on i.MX6 (Cortex-A9).
 *
 * Usage: boot-animation <lottie.json> [--fps N] [--fade-ms N] [--once]
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/fb.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include <alsa/asoundlib.h>
#include <zlib.h>
#include <thorvg_capi.h>

#include "stream.h"

static volatile sig_atomic_t quit = 0;
static volatile sig_atomic_t audio_stop = 0;

static void handle_signal(int sig)
{
    (void)sig;
    quit = 1;
}

struct audio_playback {
    const char *path;
    const char *configured_device;
    pthread_t thread;
    int started;
};

static char *select_audio_device(const char *configured)
{
    if (configured && configured[0] && strcmp(configured, "auto") != 0)
        return strdup(configured);

    void **hints = NULL;
    char *builtin = NULL;
    char *usb = NULL;
    char *fallback = NULL;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0)
        return NULL;

    for (void **hint = hints; *hint; hint++) {
        char *name = snd_device_name_get_hint(*hint, "NAME");
        char *desc = snd_device_name_get_hint(*hint, "DESC");
        char *io = snd_device_name_get_hint(*hint, "IOID");
        int output = !io || strcmp(io, "Input") != 0;
        if (output && name) {
            if (!fallback && strcmp(name, "null") != 0)
                fallback = strdup(name);
            if (!builtin && (strcasestr(name, "tas5720") ||
                             (desc && strcasestr(desc, "tas5720"))))
                builtin = strdup(name);
            if (!usb && (strcasestr(name, "usb") ||
                         (desc && strcasestr(desc, "usb"))))
                usb = strdup(name);
        }
        free(name);
        free(desc);
        free(io);
    }
    snd_device_name_free_hint(hints);

    char *selected = builtin ? builtin : (usb ? usb : fallback);
    if (selected != builtin)
        free(builtin);
    if (selected != usb)
        free(usb);
    if (selected != fallback)
        free(fallback);
    return selected;
}

static int audio_timed_out(const struct timespec *deadline)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec > deadline->tv_sec ||
           (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec);
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void *play_wav(void *arg)
{
    struct audio_playback *audio = arg;
    const char *path = audio->path;
    int fd = -1;
    uint8_t *file = MAP_FAILED;
    snd_pcm_t *pcm = NULL;
    char *device = NULL;
    struct stat st;

    fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) < 0 || st.st_size < 44)
        goto done;

    file = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file == MAP_FAILED)
        goto done;
    if (memcmp(file, "RIFF", 4) != 0 || memcmp(file + 8, "WAVE", 4) != 0)
        goto invalid;

    const uint8_t *fmt = NULL;
    const uint8_t *data = NULL;
    uint32_t fmt_size = 0;
    uint32_t data_size = 0;
    size_t offset = 12;
    while (offset + 8 <= (size_t)st.st_size) {
        uint32_t size = read_le32(file + offset + 4);
        size_t payload = offset + 8;
        if (payload + size > (size_t)st.st_size)
            goto invalid;
        if (memcmp(file + offset, "fmt ", 4) == 0) {
            fmt = file + payload;
            fmt_size = size;
        } else if (memcmp(file + offset, "data", 4) == 0) {
            data = file + payload;
            data_size = size;
        }
        offset = payload + size + (size & 1u);
    }

    if (!fmt || fmt_size < 16 || !data || read_le16(fmt) != 1 ||
        read_le16(fmt + 2) != 2 || read_le32(fmt + 4) != 48000 ||
        read_le16(fmt + 14) != 16) {
invalid:
        fprintf(stderr, "%s: expected 48 kHz stereo 16-bit PCM WAV\n", path);
        goto done;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 15;

    int err = -ENODEV;
    while (!audio_stop && !audio_timed_out(&deadline)) {
        device = select_audio_device(audio->configured_device);
        if (device) {
            err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK,
                               SND_PCM_NONBLOCK);
            if (err >= 0) {
                err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                                         SND_PCM_ACCESS_RW_INTERLEAVED,
                                         2, 48000, 1, 200000);
                if (err >= 0)
                    break;
                snd_pcm_close(pcm);
                pcm = NULL;
            }
            free(device);
            device = NULL;
        }
        struct timespec retry = { .tv_sec = 0, .tv_nsec = 250000000L };
        nanosleep(&retry, NULL);
    }
    if (!pcm) {
        if (!audio_stop)
            fprintf(stderr, "startup audio disabled: no usable output after 15s\n");
        goto done;
    }
    fprintf(stderr, "startup audio: using %s\n", device);

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += data_size / 192000 + 3;

    const uint8_t *cursor = data;
    snd_pcm_uframes_t frames = data_size / 4;
    while (frames > 0 && !audio_stop && !audio_timed_out(&deadline)) {
        snd_pcm_sframes_t written = snd_pcm_writei(pcm, cursor, frames);
        if (written == -EAGAIN) {
            snd_pcm_wait(pcm, 100);
            continue;
        }
        if (written < 0) {
            int recovered = snd_pcm_recover(pcm, (int)written, 1);
            if (recovered >= 0)
                continue;
            fprintf(stderr, "startup audio disabled: %s\n", snd_strerror(recovered));
            break;
        }
        cursor += (size_t)written * 4;
        frames -= (snd_pcm_uframes_t)written;
    }

    if (frames == 0 && !audio_stop) {
        while ((err = snd_pcm_drain(pcm)) == -EAGAIN &&
               !audio_timed_out(&deadline))
            snd_pcm_wait(pcm, 100);
    }
    if (frames > 0 || err < 0 || audio_stop)
        snd_pcm_drop(pcm);

done:
    if (pcm)
        snd_pcm_close(pcm);
    free(device);
    if (file != MAP_FAILED)
        munmap(file, (size_t)st.st_size);
    if (fd >= 0)
        close(fd);
    return NULL;
}

static void audio_start(struct audio_playback *audio, const char *path,
                        const char *configured_device)
{
    if (!path)
        return;
    audio->path = path;
    audio->configured_device = configured_device;
    if (pthread_create(&audio->thread, NULL, play_wav, audio) == 0) {
        audio->started = 1;
    } else {
        fprintf(stderr, "startup audio disabled: failed to create playback thread\n");
    }
}

static void audio_join(struct audio_playback *audio)
{
    if (audio->started) {
        pthread_join(audio->thread, NULL);
        audio->started = 0;
    }
}

static void argb_to_rgb565(const uint32_t *src, uint16_t *dst, int count)
{
    for (int i = 0; i < count; i++) {
        uint32_t px = src[i];
        uint8_t r = (px >> 16) & 0xFF;
        uint8_t g = (px >>  8) & 0xFF;
        uint8_t b =  px        & 0xFF;
        dst[i] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }
}

static void sd_notify_ready(void)
{
    const char *sock_path = getenv("NOTIFY_SOCKET");
    if (!sock_path) return;

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return;

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    if (addr.sun_path[0] == '@')
        addr.sun_path[0] = '\0';

    sendto(fd, "READY=1", 7, 0, (struct sockaddr *)&addr,
           offsetof(struct sockaddr_un, sun_path) + strlen(sock_path));
    close(fd);
    fprintf(stderr, "sd_notify: READY=1\n");
}

static void sleep_until(struct timespec *next)
{
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL) != 0)
        ;
}

static void timespec_add_ms(struct timespec *ts, long ms)
{
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static long elapsed_ms(const struct timespec *since)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - since->tv_sec) * 1000L +
           (now.tv_nsec - since->tv_nsec) / 1000000L;
}

/* ---------------------------------------------------------------- streams */

struct stream {
    uint8_t *data;
    size_t size;
    struct stream_header h;
    uint32_t *clen;      /* compressed length per frame */
    size_t *offset;      /* offset into data of each frame's payload */
    uint16_t *frame;     /* decode scratch, one frame */
    uint32_t *lut;       /* RGB565 to XRGB8888, only for 32bpp output */
    int fb_bpp;
};

static void stream_free(struct stream *s)
{
    if (!s)
        return;
    free(s->data);
    free(s->clen);
    free(s->offset);
    free(s->frame);
    free(s->lut);
    free(s);
}

/*
 * Load <lottie>.lsba, or the given path if it already is one. Returns NULL
 * whenever the stream is missing, malformed, or does not match the panel we
 * opened, so every failure lands on live rasterising rather than a blank
 * screen.
 */
static struct stream *stream_load(const char *lottie_path, int width, int height, int bpp)
{
    char path[PATH_MAX];
    const char *dot = strrchr(lottie_path, '.');

    if (dot && strcmp(dot, ".lsba") == 0) {
        snprintf(path, sizeof(path), "%s", lottie_path);
    } else {
        size_t stem = dot ? (size_t)(dot - lottie_path) : strlen(lottie_path);
        if (stem + sizeof(".lsba") > sizeof(path))
            return NULL;
        snprintf(path, sizeof(path), "%.*s.lsba", (int)stem, lottie_path);
    }

    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    struct stream *s = calloc(1, sizeof(*s));
    if (!s) {
        fclose(f);
        return NULL;
    }

    if (fread(&s->h, sizeof(s->h), 1, f) != 1 ||
        memcmp(s->h.magic, STREAM_MAGIC, 4) != 0 ||
        s->h.version != STREAM_VERSION ||
        s->h.format != STREAM_FMT_RGB565LE ||
        s->h.frame_count == 0 || s->h.interval_ms == 0) {
        fprintf(stderr, "%s: not a usable stream\n", path);
        goto fail;
    }

    if ((int)s->h.width != width || (int)s->h.height != height ||
        (bpp != 16 && bpp != 32)) {
        fprintf(stderr, "%s: %ux%u RGB565 does not match fb0 %dx%d %dbpp\n",
                path, s->h.width, s->h.height, width, height, bpp);
        goto fail;
    }

    long start = ftell(f);
    if (fseek(f, 0, SEEK_END) != 0)
        goto fail;
    s->size = (size_t)(ftell(f) - start);
    if (fseek(f, start, SEEK_SET) != 0)
        goto fail;

    s->data = malloc(s->size);
    if (!s->data || fread(s->data, 1, s->size, f) != s->size) {
        fprintf(stderr, "%s: short read\n", path);
        goto fail;
    }
    fclose(f);
    f = NULL;

    /* Walk the length prefixes to index the frames. No decompression, so this
       stays cheap even for a long animation. */
    s->clen = malloc(s->h.frame_count * sizeof(*s->clen));
    s->offset = malloc(s->h.frame_count * sizeof(*s->offset));
    s->frame = malloc((size_t)width * height * 2);
    s->fb_bpp = bpp;
    if (!s->clen || !s->offset || !s->frame)
        goto fail;

    if (bpp == 32) {
        s->lut = malloc(65536 * sizeof(*s->lut));
        if (!s->lut)
            goto fail;
        for (uint32_t px = 0; px < 65536; px++) {
            uint32_t r = (px >> 11) & 0x1f;
            uint32_t g = (px >> 5) & 0x3f;
            uint32_t b = px & 0x1f;
            s->lut[px] = (((r << 3) | (r >> 2)) << 16) |
                         (((g << 2) | (g >> 4)) << 8) |
                         ((b << 3) | (b >> 2));
        }
    }

    size_t off = 0;
    for (uint32_t i = 0; i < s->h.frame_count; i++) {
        if (off + 4 > s->size)
            goto truncated;
        memcpy(&s->clen[i], s->data + off, 4);
        off += 4;
        if (s->clen[i] == 0 || off + s->clen[i] > s->size)
            goto truncated;
        s->offset[i] = off;
        off += s->clen[i];
    }

    fprintf(stderr, "stream %s: %u frames, %ums interval%s%s\n",
            path, s->h.frame_count, s->h.interval_ms,
            (s->h.flags & STREAM_FLAG_LOOP) ? ", looping" : "",
            bpp == 32 ? ", expanding to 32bpp" : "");
    return s;

truncated:
    fprintf(stderr, "%s: truncated frame table\n", path);
fail:
    if (f)
        fclose(f);
    stream_free(s);
    return NULL;
}

static void stream_copy_to_fb(const struct stream *s, const uint16_t *src,
                              void *fb, int pixels)
{
    if (s->fb_bpp == 16) {
        memcpy(fb, src, (size_t)pixels * 2);
        return;
    }

    uint32_t *dst = fb;
    for (int i = 0; i < pixels; i++)
        dst[i] = s->lut[src[i]];
}

/* Decode one frame into scratch and push it to the panel. */
static int stream_show(struct stream *s, uint32_t idx, void *fb)
{
    const size_t source_bytes = (size_t)s->h.width * s->h.height * 2;
    uLongf out_len = source_bytes;
    int rc = uncompress((Bytef *)s->frame, &out_len,
                        s->data + s->offset[idx], s->clen[idx]);
    if (rc != Z_OK || out_len != source_bytes) {
        fprintf(stderr, "frame %u: uncompress failed (%d)\n", idx, rc);
        return -1;
    }
    stream_copy_to_fb(s, s->frame, fb,
                      (int)(s->h.width * s->h.height));
    return 0;
}

/*
 * Play the stream, picking each frame from elapsed wall time. Under boot load
 * a decode can overrun its slot, and skipping ahead keeps the run at its
 * intended length instead of stretching it; independently compressed frames
 * are what make the skip free. Leaves the last shown frame in s->frame for
 * the caller to fade out.
 */
static void stream_play(struct stream *s, void *fb, int once)
{
    const uint32_t last = s->h.frame_count - 1;
    int notified = 0;

    while (!quit) {
        struct timespec run_start;
        clock_gettime(CLOCK_MONOTONIC, &run_start);
        uint32_t shown = UINT32_MAX;

        for (;;) {
            long ms = elapsed_ms(&run_start);
            uint32_t idx = (uint32_t)(ms / (long)s->h.interval_ms);
            int final = idx >= last;
            if (final)
                idx = last;

            if (idx != shown) {
                if (stream_show(s, idx, fb) < 0)
                    return;
                shown = idx;
                if (!notified) {
                    sd_notify_ready();
                    notified = 1;
                }
            }

            if (final || quit)
                break;

            struct timespec next = run_start;
            timespec_add_ms(&next, (long)(idx + 1) * s->h.interval_ms);
            sleep_until(&next);
        }

        if (once || quit || !(s->h.flags & STREAM_FLAG_LOOP))
            break;
    }
}

/* Fade the frame we ended on down to black, then clear. */
static void stream_fade_out(struct stream *s, void *fb, int width, int height,
                            int bpp, int fade_ms, size_t fb_size)
{
    if (fade_ms > 0) {
        long frame_ms = s->h.interval_ms;
        int steps = fade_ms / (int)frame_ms;
        if (steps < 2)
            steps = 2;

        uint16_t *last = malloc((size_t)width * height * 2);
        if (last) {
            memcpy(last, s->frame, (size_t)width * height * 2);

            struct timespec next;
            clock_gettime(CLOCK_MONOTONIC, &next);
            for (int step = 1; step <= steps && !quit; step++) {
                float alpha = 1.0f - (float)step / steps;
                for (int i = 0; i < width * height; i++) {
                    uint16_t px = last[i];
                    uint16_t r = (uint16_t)(((px >> 11) & 0x1F) * alpha);
                    uint16_t g = (uint16_t)(((px >>  5) & 0x3F) * alpha);
                    uint16_t b = (uint16_t)(( px        & 0x1F) * alpha);
                    s->frame[i] = (uint16_t)((r << 11) | (g << 5) | b);
                }
                stream_copy_to_fb(s, s->frame, fb, width * height);
                timespec_add_ms(&next, frame_ms);
                sleep_until(&next);
            }
            free(last);
        }
    }

    memset(fb, 0, fb_size);
}

int main(int argc, char *argv[])
{
    const char *lottie_path = NULL;
    int target_fps = 0;
    int fade_ms = 1000;
    int once = 0;
    const char *sound_path = NULL;
    const char *audio_device = "auto";
    struct audio_playback audio = {0};

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            target_fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fade-ms") == 0 && i + 1 < argc) {
            fade_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--once") == 0) {
            once = 1;
        } else if (strcmp(argv[i], "--sound") == 0 && i + 1 < argc) {
            sound_path = argv[++i];
        } else if (strcmp(argv[i], "--audio-device") == 0 && i + 1 < argc) {
            audio_device = argv[++i];
        } else if (argv[i][0] != '-') {
            lottie_path = argv[i];
        }
    }

    if (!lottie_path) {
        fprintf(stderr, "usage: boot-animation <lottie.json> [--fps N] [--fade-ms N] [--once] [--sound WAV] [--audio-device PCM]\n");
        return 1;
    }

    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    int fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        perror("open /dev/fb0");
        return 1;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("ioctl fb");
        close(fb_fd);
        return 1;
    }

    int width = vinfo.xres;
    int height = vinfo.yres;
    int bpp = vinfo.bits_per_pixel;
    size_t fb_size = finfo.smem_len;

    fprintf(stderr, "fb0: %dx%d %dbpp stride=%d fb_size=%zu\n",
            width, height, bpp, finfo.line_length, fb_size);

    if (bpp != 16 && bpp != 32) {
        fprintf(stderr, "unsupported bpp: %d (need 16 or 32)\n", bpp);
        close(fb_fd);
        return 1;
    }

    void *fb_mmap = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mmap == MAP_FAILED) {
        perror("mmap fb");
        close(fb_fd);
        return 1;
    }

    audio_start(&audio, sound_path, audio_device);

    /*
     * Prefer a prerendered stream. Rasterising Lottie costs more per frame
     * than the frame budget on the DBC, which both stretches the animation
     * and steals CPU from the dashboard we are waiting for; a packed stream
     * is a decompress and a memcpy. Any reason not to use one (absent,
     * malformed, different geometry) falls through to rendering it live.
     */
    struct stream *stream = stream_load(lottie_path, width, height, bpp);
    if (stream) {
        stream_play(stream, fb_mmap, once);

        if (once && !quit) {
            fprintf(stderr, "holding last frame until SIGTERM\n");
            while (!quit)
                pause();
        }

        quit = 0;
        stream_fade_out(stream, fb_mmap, width, height, bpp, fade_ms, fb_size);

        stream_free(stream);
        audio_stop = 1;
        audio_join(&audio);
        munmap(fb_mmap, fb_size);
        close(fb_fd);
        return 0;
    }

    if (tvg_engine_init(0) != TVG_RESULT_SUCCESS) {
        fprintf(stderr, "tvg_engine_init failed\n");
        goto cleanup_fb;
    }

    Tvg_Canvas canvas = tvg_swcanvas_create(TVG_ENGINE_OPTION_DEFAULT);
    if (!canvas) {
        fprintf(stderr, "tvg_swcanvas_create failed\n");
        goto cleanup_engine;
    }

    /* ARGB8888 render buffer for ThorVG */
    uint32_t *argb_buf = calloc(width * height, sizeof(uint32_t));
    if (!argb_buf) {
        perror("calloc argb buffer");
        goto cleanup_canvas;
    }

    if (tvg_swcanvas_set_target(canvas, argb_buf, width, width, height,
                                TVG_COLORSPACE_ARGB8888) != TVG_RESULT_SUCCESS) {
        fprintf(stderr, "tvg_swcanvas_set_target failed\n");
        goto cleanup_buf;
    }

    Tvg_Animation anim = tvg_animation_new();
    if (!anim) {
        fprintf(stderr, "tvg_animation_new failed\n");
        goto cleanup_buf;
    }

    Tvg_Paint picture = tvg_animation_get_picture(anim);
    if (!picture) {
        fprintf(stderr, "tvg_animation_get_picture failed\n");
        goto cleanup_anim;
    }

    if (tvg_picture_load(picture, lottie_path) != TVG_RESULT_SUCCESS) {
        fprintf(stderr, "tvg_picture_load(%s) failed\n", lottie_path);
        goto cleanup_anim;
    }

    float pw = 0, ph = 0;
    tvg_picture_get_size(picture, &pw, &ph);
    fprintf(stderr, "lottie size: %.0fx%.0f\n", pw, ph);
    if (pw > 0 && ph > 0) {
        float scale_x = (float)width / pw;
        float scale_y = (float)height / ph;
        float scale = scale_x < scale_y ? scale_x : scale_y;
        float offset_x = (width - pw * scale) / 2.0f;
        float offset_y = (height - ph * scale) / 2.0f;
        tvg_paint_scale(picture, scale);
        tvg_paint_translate(picture, offset_x, offset_y);
        fprintf(stderr, "scale=%.3f offset=(%.0f,%.0f)\n", scale, offset_x, offset_y);
    }

    float total_frames = 0;
    float duration = 0;
    tvg_animation_get_total_frame(anim, &total_frames);
    tvg_animation_get_duration(anim, &duration);

    if (total_frames < 1) {
        fprintf(stderr, "animation has no frames\n");
        goto cleanup_anim;
    }

    if (duration <= 0.0f) {
        if (target_fps <= 0) {
            fprintf(stderr, "animation duration is 0 — use --fps to specify frame rate\n");
            goto cleanup_anim;
        }
        duration = total_frames / (float)target_fps;
    }

    float native_fps = total_frames / duration;
    float render_fps = (target_fps > 0) ? target_fps : native_fps;
    long frame_ms = (long)(1000.0f / render_fps);

    fprintf(stderr, "animation: %.0f frames, %.2fs, native %.1f fps, render cap %.1f fps (frame_ms=%ld)\n",
            total_frames, duration, native_fps, render_fps, frame_ms);

    tvg_canvas_add(canvas, picture);

    int notified = 0;
    while (!quit) {
        /*
         * Pick the frame from elapsed wall time rather than stepping a counter.
         * On the DBC a render costs more than its slot during boot (we compete
         * with the dashboard's startup for the CPU), and a counter would stretch
         * an 8s animation to 16s. Dropping frames keeps the run at `duration`
         * seconds, so we reliably reach the final frame before the dashboard
         * takes the display over.
         */
        struct timespec run_start;
        clock_gettime(CLOCK_MONOTONIC, &run_start);
        int last_reported = -1;

        for (;;) {
            float frame = elapsed_ms(&run_start) * native_fps / 1000.0f;
            int final = frame >= total_frames - 1;
            if (final)
                frame = total_frames - 1;

            tvg_animation_set_frame(anim, frame);
            tvg_canvas_update(canvas);
            tvg_canvas_draw(canvas, true);
            tvg_canvas_sync(canvas);

            if (bpp == 16) {
                argb_to_rgb565(argb_buf, (uint16_t *)fb_mmap, width * height);
            } else {
                memcpy(fb_mmap, argb_buf, width * height * 4);
            }

            if (!notified) {
                sd_notify_ready();
                notified = 1;
            }

            if ((int)frame / 100 != last_reported) {
                last_reported = (int)frame / 100;
                fprintf(stderr, "frame %.0f/%.0f\n", frame, total_frames);
            }

            if (final || quit)
                break;

            struct timespec next_frame;
            clock_gettime(CLOCK_MONOTONIC, &next_frame);
            timespec_add_ms(&next_frame, frame_ms);
            sleep_until(&next_frame);
        }

        if (once || quit) break;
    }

    /* In --once mode, hold the last frame visible until SIGTERM */
    if (once && !quit) {
        fprintf(stderr, "holding last frame until SIGTERM\n");
        while (!quit)
            pause();
    }

    /* Always fade on exit — first SIGTERM triggers fade, second aborts it */
    quit = 0;
    if (fade_ms > 0) {
        int fade_steps = fade_ms / frame_ms;
        if (fade_steps < 2) fade_steps = 2;

        uint32_t *last_frame = malloc(width * height * sizeof(uint32_t));
        if (last_frame) {
            memcpy(last_frame, argb_buf, width * height * sizeof(uint32_t));

            struct timespec next_frame;
            clock_gettime(CLOCK_MONOTONIC, &next_frame);
            for (int step = 1; step <= fade_steps && !quit; step++) {
                float alpha = 1.0f - (float)step / fade_steps;
                for (int i = 0; i < width * height; i++) {
                    uint32_t px = last_frame[i];
                    uint8_t r = (uint8_t)(((px >> 16) & 0xFF) * alpha);
                    uint8_t g = (uint8_t)(((px >>  8) & 0xFF) * alpha);
                    uint8_t b = (uint8_t)(( px        & 0xFF) * alpha);
                    argb_buf[i] = (0xFF << 24) | (r << 16) | (g << 8) | b;
                }

                if (bpp == 16)
                    argb_to_rgb565(argb_buf, (uint16_t *)fb_mmap, width * height);
                else
                    memcpy(fb_mmap, argb_buf, width * height * 4);

                timespec_add_ms(&next_frame, frame_ms);
                sleep_until(&next_frame);
            }
            free(last_frame);
        }
    }

    memset(fb_mmap, 0, fb_size);

cleanup_anim:
    tvg_animation_del(anim);
cleanup_buf:
    free(argb_buf);
cleanup_canvas:
    tvg_canvas_destroy(canvas);
cleanup_engine:
    tvg_engine_term();
cleanup_fb:
    audio_stop = 1;
    audio_join(&audio);
    munmap(fb_mmap, fb_size);
    close(fb_fd);

    return 0;
}
