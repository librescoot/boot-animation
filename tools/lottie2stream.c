/*
 * lottie2stream: pack a Lottie animation into a prerendered frame stream.
 *
 * Build-time companion to boot-animation. Rasterises with the same ThorVG
 * setup the player uses, so the output is identical to what live rendering
 * would have produced, then stores each frame as independently decodable zlib.
 *
 * usage: lottie2stream <lottie.json> <width> <height> <fps> <out.lsba> [--loop]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <thorvg_capi.h>

#include "../stream.h"

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

int main(int argc, char *argv[])
{
    if (argc < 6) {
        fprintf(stderr, "usage: %s <lottie.json> <width> <height> <fps> <out.lsba> [--loop]\n",
                argv[0]);
        return 1;
    }

    const char *lottie_path = argv[1];
    int width = atoi(argv[2]);
    int height = atoi(argv[3]);
    int fps = atoi(argv[4]);
    const char *out_path = argv[5];
    int loop = argc > 6 && strcmp(argv[6], "--loop") == 0;

    if (width <= 0 || height <= 0 || fps <= 0) {
        fprintf(stderr, "width, height and fps must be positive\n");
        return 1;
    }

    if (tvg_engine_init(0) != TVG_RESULT_SUCCESS) {
        fprintf(stderr, "tvg_engine_init failed\n");
        return 1;
    }

    Tvg_Canvas canvas = tvg_swcanvas_create(TVG_ENGINE_OPTION_DEFAULT);
    uint32_t *argb = calloc((size_t)width * height, sizeof(uint32_t));
    uint16_t *rgb565 = malloc((size_t)width * height * 2);
    if (!canvas || !argb || !rgb565) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }
    tvg_swcanvas_set_target(canvas, argb, width, width, height, TVG_COLORSPACE_ARGB8888);

    Tvg_Animation anim = tvg_animation_new();
    Tvg_Paint picture = tvg_animation_get_picture(anim);
    if (tvg_picture_load(picture, lottie_path) != TVG_RESULT_SUCCESS) {
        fprintf(stderr, "tvg_picture_load(%s) failed\n", lottie_path);
        return 1;
    }

    /* Same proportional fit as the player's live path. */
    float pw = 0, ph = 0;
    tvg_picture_get_size(picture, &pw, &ph);
    if (pw > 0 && ph > 0) {
        float sx = (float)width / pw, sy = (float)height / ph;
        float scale = sx < sy ? sx : sy;
        tvg_paint_scale(picture, scale);
        tvg_paint_translate(picture, (width - pw * scale) / 2.0f,
                            (height - ph * scale) / 2.0f);
    }

    float total_frames = 0, duration = 0;
    tvg_animation_get_total_frame(anim, &total_frames);
    tvg_animation_get_duration(anim, &duration);
    if (total_frames < 1) {
        fprintf(stderr, "animation has no frames\n");
        return 1;
    }
    if (duration <= 0.0f)
        duration = total_frames / (float)fps;

    float native_fps = total_frames / duration;
    float step = native_fps / (float)fps;
    uint32_t count = (uint32_t)(duration * fps);
    if (count < 1)
        count = 1;

    tvg_canvas_add(canvas, picture);

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        perror("open output");
        return 1;
    }

    struct stream_header h = {
        .magic = { 'L', 'S', 'B', 'A' },
        .version = STREAM_VERSION,
        .width = width,
        .height = height,
        .format = STREAM_FMT_RGB565LE,
        .frame_count = count,
        .interval_ms = 1000 / fps,
        .flags = loop ? STREAM_FLAG_LOOP : 0,
    };
    if (fwrite(&h, sizeof(h), 1, out) != 1) {
        perror("write header");
        return 1;
    }

    const size_t frame_bytes = (size_t)width * height * 2;
    uLongf bound = compressBound(frame_bytes);
    uint8_t *packed = malloc(bound);
    size_t total_packed = 0;

    for (uint32_t i = 0; i < count; i++) {
        /* Clamp so the last entry is exactly the final frame; a --once
           animation holds this one, and anything matching against it (a
           crossfade, say) needs it to be the real end of the clip. */
        float frame = (float)i * step;
        if (i == count - 1 || frame > total_frames - 1)
            frame = total_frames - 1;

        tvg_animation_set_frame(anim, frame);
        tvg_canvas_update(canvas);
        tvg_canvas_draw(canvas, true);
        tvg_canvas_sync(canvas);
        argb_to_rgb565(argb, rgb565, width * height);

        uLongf clen = bound;
        if (compress2(packed, &clen, (const Bytef *)rgb565, frame_bytes, 9) != Z_OK) {
            fprintf(stderr, "compress failed on frame %u\n", i);
            return 1;
        }

        uint32_t len32 = (uint32_t)clen;
        if (fwrite(&len32, sizeof(len32), 1, out) != 1 ||
            fwrite(packed, 1, clen, out) != clen) {
            perror("write frame");
            return 1;
        }
        total_packed += clen;
    }

    fclose(out);

    fprintf(stderr, "%s: %u frames, %dx%d, %ums interval, %.2f MiB (mean %zu KiB/frame)%s\n",
            out_path, count, width, height, h.interval_ms,
            total_packed / 1024.0 / 1024.0, total_packed / count / 1024,
            loop ? ", looping" : "");

    tvg_animation_del(anim);
    tvg_canvas_destroy(canvas);
    tvg_engine_term();
    free(argb);
    free(rgb565);
    free(packed);
    return 0;
}
