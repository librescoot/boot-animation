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
#include <thorvg_capi.h>

static volatile sig_atomic_t quit = 0;

static void handle_signal(int sig)
{
    (void)sig;
    quit = 1;
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
    ts->tv_nsec += ms * 1000000L;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
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
        fprintf(stderr, "usage: boot-animation <lottie.json> [--fps N] [--fade-ms N] [--once]\n");
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

    float native_fps = total_frames / duration;
    float render_fps = (target_fps > 0) ? target_fps : native_fps;
    float frame_step = native_fps / render_fps;
    long frame_ms = (long)(1000.0f / render_fps);

    fprintf(stderr, "animation: %.0f frames, %.2fs, native %.1f fps, render %.1f fps (step=%.2f, frame_ms=%ld)\n",
            total_frames, duration, native_fps, render_fps, frame_step, frame_ms);

    tvg_canvas_add(canvas, picture);

    struct timespec next_frame;
    clock_gettime(CLOCK_MONOTONIC, &next_frame);

    int notified = 0;
    while (!quit) {
        for (float frame = 0; frame < total_frames && !quit; frame += frame_step) {
            tvg_animation_set_frame(anim, frame);
            tvg_canvas_update(canvas);
            tvg_canvas_draw(canvas, true);
            tvg_canvas_sync(canvas);

            if (bpp == 16) {
                argb_to_rgb565(argb_buf, (uint16_t *)fb_mmap, width * height);
            } else {
                memcpy(fb_mmap, argb_buf, width * height * 4);
            }

            if ((int)frame % 100 == 0)
                fprintf(stderr, "frame %.0f/%.0f\n", frame, total_frames);

            timespec_add_ms(&next_frame, frame_ms);
            sleep_until(&next_frame);
        }

        if (!notified) {
            sd_notify_ready();
            notified = 1;
        }

        if (once) break;
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
    munmap(fb_mmap, fb_size);
    close(fb_fd);

    return 0;
}
