/*
 * boot-animation: Lottie animation renderer for /dev/fb0
 *
 * Renders a Lottie JSON animation directly to the framebuffer using ThorVG's
 * software renderer. Designed for embedded boot splash on i.MX6 (Cortex-A9).
 *
 * Usage: boot-animation <lottie.json> [--fps N] [--fade-ms N] [--once]
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fb.h>
#include <math.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include <thorvg_capi.h>

#include "render_utils.h"
#include "signal_utils.h"
#include "stream.h"

static volatile sig_atomic_t stop_signals = 0;

struct run_stats {
    uint64_t frames_shown;
    uint64_t frames_skipped;
    uint64_t fade_frames_shown;
    uint64_t fade_frames_skipped;
    uint64_t missed_deadlines;
    uint64_t max_lateness_ms;
    uint64_t last_slot;
    uint64_t last_fade_slot;
    int have_slot;
    int have_fade_slot;
};

static void handle_signal(int sig)
{
    (void)sig;
    if (stop_signals < SIG_ATOMIC_MAX)
        stop_signals++;
}

static void sd_notify_ready(void)
{
    const char *sock_path = getenv("NOTIFY_SOCKET");
    if (!sock_path)
        return;

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
        return;

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    if (addr.sun_path[0] == '@')
        addr.sun_path[0] = '\0';

    sendto(fd, "READY=1", 7, 0, (struct sockaddr *)&addr,
           offsetof(struct sockaddr_un, sun_path) + strlen(sock_path));
    close(fd);
    fprintf(stderr, "sd_notify: READY=1\n");
}

static void sleep_until(const struct timespec *deadline)
{
    while (!stop_signals) {
        int result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                     deadline, NULL);
        if (result == 0 || result != EINTR)
            return;
    }
}

static uint64_t elapsed_since(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return timespec_elapsed_ms(start, &now);
}

static void stats_start_run(struct run_stats *stats)
{
    stats->have_slot = 0;
}

static void stats_note(struct run_stats *stats, uint64_t slot,
                       uint64_t displayed_ms, uint64_t deadline_ms,
                       uint32_t budget_ms, int fade)
{
    uint64_t *shown = fade ? &stats->fade_frames_shown : &stats->frames_shown;
    uint64_t *skipped = fade ? &stats->fade_frames_skipped : &stats->frames_skipped;
    uint64_t *last = fade ? &stats->last_fade_slot : &stats->last_slot;
    int *have = fade ? &stats->have_fade_slot : &stats->have_slot;

    if (*have) {
        if (slot > *last + 1)
            *skipped += slot - *last - 1;
    } else if (slot > 0) {
        *skipped += slot;
    }
    *last = slot;
    *have = 1;
    (*shown)++;

    uint64_t lateness = displayed_ms > deadline_ms ?
                        displayed_ms - deadline_ms : 0;
    if (lateness > stats->max_lateness_ms)
        stats->max_lateness_ms = lateness;
    if (budget_ms > 0 && lateness >= budget_ms)
        stats->missed_deadlines++;
}

/* ---------------------------------------------------------------- streams */

struct stream {
    uint8_t *data;
    size_t size;
    struct stream_header h;
    uint32_t *clen;
    size_t *offset;
    uint16_t *frame;
    uint32_t *lut;
    int has_frame;
};

static void stream_free(struct stream *stream)
{
    if (!stream)
        return;
    free(stream->data);
    free(stream->clen);
    free(stream->offset);
    free(stream->frame);
    free(stream->lut);
    free(stream);
}

static int size_product(size_t left, size_t right, size_t *result)
{
    if (right != 0 && left > SIZE_MAX / right)
        return -1;
    *result = left * right;
    return 0;
}

/* Every failure is reported to the caller so it can select live rendering. */
static struct stream *stream_load(const char *lottie_path,
                                  const struct fb_surface *surface,
                                  const char **fallback_reason)
{
    char path[PATH_MAX];
    FILE *file = NULL;
    struct stream *stream = NULL;

    if (animation_stream_path(path, sizeof(path), lottie_path) < 0) {
        *fallback_reason = "stream path too long";
        return NULL;
    }

    file = fopen(path, "rb");
    if (!file) {
        *fallback_reason = errno == ENOENT ? "stream not found" :
                                             "stream open failed";
        return NULL;
    }

    stream = calloc(1, sizeof(*stream));
    if (!stream) {
        *fallback_reason = "stream allocation failed";
        goto fail;
    }

    if (fread(&stream->h, sizeof(stream->h), 1, file) != 1) {
        *fallback_reason = "truncated stream header";
        goto fail;
    }

    enum stream_validation validation = stream_validate_header(&stream->h);
    if (validation != STREAM_VALID) {
        *fallback_reason = stream_validation_name(validation);
        goto fail;
    }
    if (stream->h.width != surface->width ||
        stream->h.height != surface->height) {
        *fallback_reason = "stream geometry mismatch";
        goto fail;
    }

    size_t frame_pixels;
    size_t frame_bytes;
    size_t lengths_bytes;
    size_t offsets_bytes;
    if (size_product(surface->width, surface->height, &frame_pixels) < 0 ||
        size_product(frame_pixels, sizeof(*stream->frame), &frame_bytes) < 0 ||
        size_product(stream->h.frame_count, sizeof(*stream->clen),
                     &lengths_bytes) < 0 ||
        size_product(stream->h.frame_count, sizeof(*stream->offset),
                     &offsets_bytes) < 0) {
        *fallback_reason = "stream dimensions too large";
        goto fail;
    }

    long payload_start = ftell(file);
    if (payload_start < 0 || fseek(file, 0, SEEK_END) != 0) {
        *fallback_reason = "stream seek failed";
        goto fail;
    }
    long file_end = ftell(file);
    if (file_end < payload_start || fseek(file, payload_start, SEEK_SET) != 0) {
        *fallback_reason = "invalid stream size";
        goto fail;
    }
    stream->size = (size_t)(file_end - payload_start);
    validation = stream_validate_payload_size(stream->size);
    if (validation != STREAM_VALID) {
        *fallback_reason = stream_validation_name(validation);
        goto fail;
    }
    if (stream->h.frame_count > stream->size / 5u) {
        *fallback_reason = "truncated frame table";
        goto fail;
    }

    stream->data = malloc(stream->size ? stream->size : 1);
    stream->clen = malloc(lengths_bytes);
    stream->offset = malloc(offsets_bytes);
    if (!stream->data || !stream->clen || !stream->offset) {
        *fallback_reason = "stream allocation failed";
        goto fail;
    }
    if (fread(stream->data, 1, stream->size, file) != stream->size) {
        *fallback_reason = "short stream read";
        goto fail;
    }

    validation = stream_index_frames(stream->data, stream->size,
                                     stream->h.frame_count,
                                     stream_frame_compressed_limit(frame_bytes),
                                     stream->clen, stream->offset);
    if (validation != STREAM_VALID) {
        *fallback_reason = stream_validation_name(validation);
        goto fail;
    }

    stream->frame = malloc(frame_bytes);
    if (!stream->frame) {
        *fallback_reason = "stream frame allocation failed";
        goto fail;
    }

    if (surface->format != FB_PIXEL_RGB565) {
        stream->lut = malloc(65536u * sizeof(*stream->lut));
        if (!stream->lut) {
            *fallback_reason = "stream conversion allocation failed";
            goto fail;
        }
        for (uint32_t pixel = 0; pixel < 65536u; pixel++)
            stream->lut[pixel] = rgb565_to_32_pixel((uint16_t)pixel,
                                                    surface->format);
    }

    fclose(file);
    fprintf(stderr, "render mode: stream (%u frames, %ums, %s%s)\n",
            stream->h.frame_count, stream->h.interval_ms,
            fb_pixel_format_name(surface->format),
            (stream->h.flags & STREAM_FLAG_LOOP) ? ", looping" : "");
    *fallback_reason = "none";
    return stream;

fail:
    fclose(file);
    stream_free(stream);
    return NULL;
}

static int stream_show(struct stream *stream, uint32_t index,
                       struct fb_surface *surface)
{
    size_t source_bytes = (size_t)surface->width * surface->height *
                          sizeof(*stream->frame);
    uLongf output_length = (uLongf)source_bytes;
    int result = uncompress((Bytef *)stream->frame, &output_length,
                            stream->data + stream->offset[index],
                            stream->clen[index]);
    if (result != Z_OK || output_length != source_bytes) {
        fprintf(stderr, "stream frame %u: decompression failed (%d)\n",
                index, result);
        return -1;
    }

    fb_copy_rgb565(surface, stream->frame, stream->lut);
    stream->has_frame = 1;
    return 0;
}

static int stream_play(struct stream *stream, struct fb_surface *surface,
                       int once, int *notified, struct run_stats *stats)
{
    while (!stop_signals) {
        struct timespec run_start;
        clock_gettime(CLOCK_MONOTONIC, &run_start);
        uint32_t shown_index = UINT32_MAX;
        stats_start_run(stats);

        for (;;) {
            uint64_t elapsed = elapsed_since(&run_start);
            int final;
            uint32_t index = cadence_frame_index(elapsed,
                                                 stream->h.interval_ms,
                                                 stream->h.frame_count,
                                                 &final);

            if (index != shown_index) {
                if (stream_show(stream, index, surface) < 0)
                    return -1;
                shown_index = index;
                uint64_t displayed = elapsed_since(&run_start);
                stats_note(stats, index, displayed,
                           cadence_deadline_ms(index, stream->h.interval_ms),
                           stream->h.interval_ms, 0);
                if (!*notified) {
                    sd_notify_ready();
                    *notified = 1;
                }
            }

            if (final || stop_signals)
                break;

            struct timespec deadline = run_start;
            timespec_add_ms(&deadline,
                            cadence_deadline_ms(index + 1,
                                                stream->h.interval_ms));
            sleep_until(&deadline);
        }

        if (once || stop_signals || !(stream->h.flags & STREAM_FLAG_LOOP))
            break;
    }
    return 0;
}

static int stream_fade_out(struct stream *stream, struct fb_surface *surface,
                           int fade_ms, struct run_stats *stats)
{
    int result = 0;

    if (fade_ms > 0 && stream->has_frame) {
        uint32_t frame_ms = stream->h.interval_ms;
        uint32_t steps = (uint32_t)fade_ms / frame_ms;
        if (steps < 2)
            steps = 2;

        size_t pixels = (size_t)surface->width * surface->height;
        uint16_t *last = malloc(pixels * sizeof(*last));
        if (last) {
            memcpy(last, stream->frame, pixels * sizeof(*last));
            struct timespec fade_start;
            clock_gettime(CLOCK_MONOTONIC, &fade_start);
            stats->have_fade_slot = 0;

            uint32_t next_step = 1;
            while (next_step <= steps && !stop_signals) {
                struct timespec deadline = fade_start;
                timespec_add_ms(&deadline,
                                fade_deadline_ms(next_step,
                                                 (uint32_t)fade_ms, steps));
                sleep_until(&deadline);
                if (stop_signals)
                    break;

                uint64_t elapsed = elapsed_since(&fade_start);
                uint32_t step = fade_step_at(elapsed, (uint32_t)fade_ms,
                                             steps, next_step);
                float alpha = 1.0f - (float)step / steps;
                for (size_t i = 0; i < pixels; i++) {
                    uint16_t pixel = last[i];
                    uint16_t red = (uint16_t)(((pixel >> 11) & 0x1f) * alpha);
                    uint16_t green = (uint16_t)(((pixel >> 5) & 0x3f) * alpha);
                    uint16_t blue = (uint16_t)((pixel & 0x1f) * alpha);
                    stream->frame[i] = (uint16_t)((red << 11) |
                                                   (green << 5) | blue);
                }
                fb_copy_rgb565(surface, stream->frame, stream->lut);
                uint64_t displayed = elapsed_since(&fade_start);
                uint64_t due = fade_deadline_ms(step, (uint32_t)fade_ms,
                                                steps);
                uint32_t budget = (uint32_t)(((uint64_t)fade_ms + steps - 1u) /
                                             steps);
                stats_note(stats, step - 1u, displayed, due, budget, 1);
                next_step = step + 1u;
            }
            free(last);
        } else {
            result = -1;
        }
    }

    fb_clear_visible(surface);
    return result;
}

/* ---------------------------------------------------------- live rendering */

static int live_fade_out(uint32_t *argb_buffer, struct fb_surface *surface,
                         uint32_t frame_ms, int fade_ms,
                         struct run_stats *stats)
{
    if (fade_ms <= 0)
        return 0;

    uint32_t steps = (uint32_t)fade_ms / frame_ms;
    if (steps < 2)
        steps = 2;
    size_t pixels = (size_t)surface->width * surface->height;
    uint32_t *last = malloc(pixels * sizeof(*last));
    if (!last)
        return -1;
    memcpy(last, argb_buffer, pixels * sizeof(*last));

    struct timespec fade_start;
    clock_gettime(CLOCK_MONOTONIC, &fade_start);
    stats->have_fade_slot = 0;
    uint32_t next_step = 1;
    while (next_step <= steps && !stop_signals) {
        struct timespec deadline = fade_start;
        timespec_add_ms(&deadline,
                        fade_deadline_ms(next_step, (uint32_t)fade_ms, steps));
        sleep_until(&deadline);
        if (stop_signals)
            break;

        uint64_t elapsed = elapsed_since(&fade_start);
        uint32_t step = fade_step_at(elapsed, (uint32_t)fade_ms, steps,
                                     next_step);
        float alpha = 1.0f - (float)step / steps;
        for (size_t i = 0; i < pixels; i++) {
            uint32_t pixel = last[i];
            uint8_t red = (uint8_t)(((pixel >> 16) & 0xff) * alpha);
            uint8_t green = (uint8_t)(((pixel >> 8) & 0xff) * alpha);
            uint8_t blue = (uint8_t)((pixel & 0xff) * alpha);
            argb_buffer[i] = 0xff000000u | (uint32_t)red << 16 |
                             (uint32_t)green << 8 | blue;
        }
        fb_copy_argb(surface, argb_buffer);
        uint64_t displayed = elapsed_since(&fade_start);
        uint64_t due = fade_deadline_ms(step, (uint32_t)fade_ms, steps);
        uint32_t budget = (uint32_t)(((uint64_t)fade_ms + steps - 1u) /
                                     steps);
        stats_note(stats, step - 1u, displayed, due, budget, 1);
        next_step = step + 1u;
    }

    free(last);
    return 0;
}

static int render_live(const char *lottie_path, struct fb_surface *surface,
                       int target_fps, int fade_ms, int once, int *notified,
                       struct run_stats *stats, const char **failure_reason)
{
    int engine_started = 0;
    Tvg_Canvas canvas = NULL;
    Tvg_Animation animation = NULL;
    uint32_t *argb_buffer = NULL;
    int result = -1;

    if (tvg_engine_init(0) != TVG_RESULT_SUCCESS) {
        *failure_reason = "ThorVG engine initialization failed";
        goto cleanup;
    }
    engine_started = 1;

    canvas = tvg_swcanvas_create(TVG_ENGINE_OPTION_DEFAULT);
    if (!canvas) {
        *failure_reason = "ThorVG canvas creation failed";
        goto cleanup;
    }

    size_t render_pixels;
    if (size_product(surface->width, surface->height, &render_pixels) < 0 ||
        render_pixels > SIZE_MAX / sizeof(*argb_buffer)) {
        *failure_reason = "live render dimensions overflow";
        goto cleanup;
    }
    argb_buffer = calloc(render_pixels, sizeof(*argb_buffer));
    if (!argb_buffer) {
        *failure_reason = "live render buffer allocation failed";
        goto cleanup;
    }

    if (tvg_swcanvas_set_target(canvas, argb_buffer, surface->width,
                                surface->width, surface->height,
                                TVG_COLORSPACE_ARGB8888) != TVG_RESULT_SUCCESS) {
        *failure_reason = "ThorVG target setup failed";
        goto cleanup;
    }

    animation = tvg_animation_new();
    if (!animation) {
        *failure_reason = "ThorVG animation creation failed";
        goto cleanup;
    }
    Tvg_Paint picture = tvg_animation_get_picture(animation);
    if (!picture) {
        *failure_reason = "ThorVG picture creation failed";
        goto cleanup;
    }
    if (tvg_picture_load(picture, lottie_path) != TVG_RESULT_SUCCESS) {
        *failure_reason = "live JSON load failed";
        goto cleanup;
    }

    float picture_width = 0;
    float picture_height = 0;
    if (tvg_picture_get_size(picture, &picture_width, &picture_height) !=
        TVG_RESULT_SUCCESS) {
        *failure_reason = "live picture size query failed";
        goto cleanup;
    }
    if (picture_width > 0 && picture_height > 0) {
        float scale_x = (float)surface->width / picture_width;
        float scale_y = (float)surface->height / picture_height;
        float scale = scale_x < scale_y ? scale_x : scale_y;
        if (tvg_paint_scale(picture, scale) != TVG_RESULT_SUCCESS ||
            tvg_paint_translate(
                picture,
                (surface->width - picture_width * scale) / 2.0f,
                (surface->height - picture_height * scale) / 2.0f) !=
            TVG_RESULT_SUCCESS) {
            *failure_reason = "live picture transform failed";
            goto cleanup;
        }
    }

    float total_frames = 0;
    float duration = 0;
    if (tvg_animation_get_total_frame(animation, &total_frames) !=
            TVG_RESULT_SUCCESS ||
        tvg_animation_get_duration(animation, &duration) != TVG_RESULT_SUCCESS) {
        *failure_reason = "live animation timing query failed";
        goto cleanup;
    }
    if (!(total_frames >= 1.0f) || !isfinite(total_frames)) {
        *failure_reason = "live animation has no valid frames";
        goto cleanup;
    }
    if (duration <= 0.0f) {
        if (target_fps <= 0) {
            *failure_reason = "live animation duration is zero without --fps";
            goto cleanup;
        }
        duration = total_frames / (float)target_fps;
    }

    float native_fps = total_frames / duration;
    float render_fps = target_fps > 0 ? (float)target_fps : native_fps;
    if (!(duration > 0.0f) || !isfinite(duration) ||
        !(native_fps > 0.0f) || !isfinite(native_fps) ||
        !(render_fps > 0.0f) || !isfinite(render_fps)) {
        *failure_reason = "live animation has invalid timing";
        goto cleanup;
    }
    double frame_ms_value = 1000.0 / (double)render_fps;
    if (!isfinite(frame_ms_value) || frame_ms_value > UINT32_MAX) {
        *failure_reason = "live frame interval is out of range";
        goto cleanup;
    }
    uint32_t frame_ms = (uint32_t)frame_ms_value;
    if (frame_ms < 1)
        frame_ms = 1;
    double duration_ms_value = ceil((double)duration * 1000.0);
    if (!(duration_ms_value >= 1.0) || duration_ms_value > UINT32_MAX) {
        *failure_reason = "live animation duration is out of range";
        goto cleanup;
    }
    uint64_t duration_ms = (uint64_t)duration_ms_value;
    uint64_t final_start_ms = 0;
    if (total_frames > 1.0f) {
        double final_start_value = ceil(((double)total_frames - 1.0) * 1000.0 /
                                        (double)native_fps);
        if (final_start_value > 0.0)
            final_start_ms = (uint64_t)final_start_value;
        if (final_start_ms > duration_ms)
            final_start_ms = duration_ms;
    }

    fprintf(stderr,
            "live animation: %.0f frames, %.2fs, native %.1f fps, cap %.1f fps\n",
            total_frames, duration, native_fps, render_fps);
    if (tvg_canvas_add(canvas, picture) != TVG_RESULT_SUCCESS) {
        *failure_reason = "ThorVG canvas setup failed";
        goto cleanup;
    }

    while (!stop_signals) {
        struct timespec run_start;
        clock_gettime(CLOCK_MONOTONIC, &run_start);
        stats_start_run(stats);
        int displayed_final = 0;

        for (;;) {
            uint64_t elapsed = elapsed_since(&run_start);
            int complete = timeline_complete(elapsed, duration_ms);
            if (complete && displayed_final)
                break;

            uint64_t slot = elapsed / frame_ms;
            float frame = (float)elapsed * native_fps / 1000.0f;
            int selected_final = frame >= total_frames - 1.0f;
            if (selected_final)
                frame = total_frames - 1.0f;

            Tvg_Result frame_result = tvg_animation_set_frame(animation, frame);
            if ((frame_result != TVG_RESULT_SUCCESS &&
                 frame_result != TVG_RESULT_INSUFFICIENT_CONDITION) ||
                tvg_canvas_update(canvas) != TVG_RESULT_SUCCESS ||
                tvg_canvas_draw(canvas, true) != TVG_RESULT_SUCCESS ||
                tvg_canvas_sync(canvas) != TVG_RESULT_SUCCESS) {
                *failure_reason = "live frame rendering failed";
                goto cleanup;
            }
            fb_copy_argb(surface, argb_buffer);
            displayed_final |= selected_final;

            uint64_t displayed = elapsed_since(&run_start);
            stats_note(stats, slot, displayed, slot * frame_ms,
                       frame_ms, 0);
            if (!*notified) {
                sd_notify_ready();
                *notified = 1;
            }

            if (complete || stop_signals)
                break;

            uint64_t next_ms = displayed_final ? duration_ms :
                               (slot + 1u) * frame_ms;
            if (!displayed_final && final_start_ms > elapsed &&
                final_start_ms < next_ms)
                next_ms = final_start_ms;
            if (next_ms > duration_ms)
                next_ms = duration_ms;
            struct timespec deadline = run_start;
            timespec_add_ms(&deadline, next_ms);
            sleep_until(&deadline);
        }

        if (once || stop_signals)
            break;
    }

    if (once && !stop_signals)
        fprintf(stderr, "holding last frame until SIGTERM\n");
    if (signal_prepare_fade(&stop_signals, once) < 0) {
        *failure_reason = "signal wait setup failed";
        fb_clear_visible(surface);
        goto cleanup;
    }

    if (live_fade_out(argb_buffer, surface, frame_ms, fade_ms, stats) < 0) {
        *failure_reason = "live fade snapshot allocation failed";
        fb_clear_visible(surface);
        goto cleanup;
    }
    fb_clear_visible(surface);
    result = 0;

cleanup:
    if (animation)
        tvg_animation_del(animation);
    free(argb_buffer);
    if (canvas)
        tvg_canvas_destroy(canvas);
    if (engine_started)
        tvg_engine_term();
    return result;
}

static struct fb_channel_layout channel_layout(struct fb_bitfield field)
{
    struct fb_channel_layout result = {
        .offset = field.offset,
        .length = field.length,
        .msb_right = field.msb_right,
    };
    return result;
}

int main(int argc, char *argv[])
{
    const char *lottie_path = NULL;
    int target_fps = 0;
    int fade_ms = 1000;
    int once = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            target_fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fade-ms") == 0 && i + 1 < argc) {
            fade_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--once") == 0) {
            once = 1;
        } else if (argv[i][0] != '-') {
            lottie_path = argv[i];
        }
    }

    if (!lottie_path) {
        fprintf(stderr,
                "usage: boot-animation <lottie.json> [--fps N] [--fade-ms N] [--once]\n");
        return 1;
    }

    struct sigaction signal_action;
    memset(&signal_action, 0, sizeof(signal_action));
    signal_action.sa_handler = handle_signal;
    sigemptyset(&signal_action.sa_mask);
    sigaddset(&signal_action.sa_mask, SIGTERM);
    sigaddset(&signal_action.sa_mask, SIGINT);
    if (sigaction(SIGTERM, &signal_action, NULL) < 0 ||
        sigaction(SIGINT, &signal_action, NULL) < 0) {
        perror("install signal handler");
        return 1;
    }

    int fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        perror("open /dev/fb0");
        return 1;
    }

    struct fb_var_screeninfo variable;
    struct fb_fix_screeninfo fixed;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &variable) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &fixed) < 0) {
        perror("ioctl fb");
        close(fb_fd);
        return 1;
    }

    if (variable.nonstd != 0 || variable.grayscale != 0 ||
        fixed.type != FB_TYPE_PACKED_PIXELS ||
        fixed.visual != FB_VISUAL_TRUECOLOR) {
        fprintf(stderr, "fb0 rejected: unsupported framebuffer organization\n");
        close(fb_fd);
        return 1;
    }

    size_t mapping_size = fixed.smem_len;
    void *mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fb_fd, 0);
    if (mapping == MAP_FAILED) {
        perror("mmap fb");
        close(fb_fd);
        return 1;
    }

    struct fb_surface surface;
    const char *layout_error = NULL;
    if (fb_surface_init(&surface, mapping, mapping_size,
                        variable.xres, variable.yres,
                        variable.xres_virtual, variable.yres_virtual,
                        variable.xoffset, variable.yoffset,
                        variable.bits_per_pixel, fixed.line_length,
                        channel_layout(variable.red),
                        channel_layout(variable.green),
                        channel_layout(variable.blue),
                        channel_layout(variable.transp),
                        &layout_error) < 0) {
        fprintf(stderr, "fb0 rejected: %s\n", layout_error);
        munmap(mapping, mapping_size);
        close(fb_fd);
        return 1;
    }

    fprintf(stderr, "fb0: %ux%u %s, stride=%zu, offset=%zu, map=%zu\n",
            surface.width, surface.height,
            fb_pixel_format_name(surface.format), surface.stride,
            surface.visible_offset, surface.mapping_size);

    struct run_stats stats = {0};
    const char *stream_failure = "none";
    const char *live_failure = "not attempted";
    const char *selected_mode = "stream";
    int notified = 0;
    int result = 0;

    struct stream *stream = stream_load(lottie_path, &surface,
                                        &stream_failure);
    if (stream) {
        if (stream_play(stream, &surface, once, &notified, &stats) == 0) {
            if (once && !stop_signals)
                fprintf(stderr, "holding last frame until SIGTERM\n");
            if (signal_prepare_fade(&stop_signals, once) < 0) {
                perror("signal wait setup failed");
                stream_failure = "signal wait setup failed";
                selected_mode = "failed";
                result = 1;
                fb_clear_visible(&surface);
                goto finished;
            }
            if (stream_fade_out(stream, &surface, fade_ms, &stats) < 0) {
                stream_failure = "stream fade snapshot allocation failed";
                fprintf(stderr, "stream failure: %s\n", stream_failure);
                selected_mode = "failed";
                result = 1;
            }
            goto finished;
        }
        stream_failure = "stream decompression failed";
        fprintf(stderr, "stream failure: %s\n", stream_failure);
        stream_free(stream);
        stream = NULL;
    }

    char live_path[PATH_MAX];
    selected_mode = "live";
    fprintf(stderr, "render mode: live (stream failure: %s)\n",
            stream_failure);
    if (animation_live_path(live_path, sizeof(live_path), lottie_path) < 0) {
        live_failure = "live fallback path is too long";
        selected_mode = "failed";
        result = 1;
    } else if (render_live(live_path, &surface, target_fps, fade_ms, once,
                           &notified, &stats, &live_failure) < 0) {
        selected_mode = "failed";
        result = 1;
    } else {
        live_failure = "none";
    }
    if (result != 0)
        fprintf(stderr, "live failure: %s\n", live_failure);

finished:
    stream_free(stream);
    fprintf(stderr,
            "summary: mode=%s stream_failure=\"%s\" live_failure=\"%s\" "
            "shown=%llu skipped=%llu fade_shown=%llu fade_skipped=%llu "
            "missed_deadlines=%llu max_lateness_ms=%llu\n",
            selected_mode, stream_failure, live_failure,
            (unsigned long long)stats.frames_shown,
            (unsigned long long)stats.frames_skipped,
            (unsigned long long)stats.fade_frames_shown,
            (unsigned long long)stats.fade_frames_skipped,
            (unsigned long long)stats.missed_deadlines,
            (unsigned long long)stats.max_lateness_ms);

    munmap(mapping, mapping_size);
    close(fb_fd);
    return result;
}
