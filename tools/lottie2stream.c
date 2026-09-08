/*
 * lottie2stream: pack a Lottie animation into a prerendered frame stream.
 *
 * usage: lottie2stream <lottie.json> <width> <height> <fps> <out.lsba> [--loop]
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>
#include <thorvg_capi.h>

#include "../stream.h"

static int parse_positive_u32(const char *text, const char *name,
                              uint32_t *value)
{
    char *end = NULL;
    uintmax_t parsed;

    if (!text || !isdigit((unsigned char)text[0])) {
        fprintf(stderr, "%s must be a positive integer\n", name);
        return -1;
    }
    errno = 0;
    parsed = strtoumax(text, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || parsed == 0 ||
        parsed > UINT32_MAX) {
        fprintf(stderr, "%s must be an integer from 1 to %" PRIu32 "\n",
                name, UINT32_MAX);
        return -1;
    }
    *value = (uint32_t)parsed;
    return 0;
}

static int target_is_publishable(const char *path, mode_t *output_mode)
{
    struct stat status;

    if (lstat(path, &status) == 0) {
        if (!S_ISREG(status.st_mode)) {
            errno = EINVAL;
            perror("output target is not a regular file");
            return -1;
        }
        if (access(path, W_OK) < 0) {
            perror("output target is not writable");
            return -1;
        }
        *output_mode = status.st_mode & 0777;
    } else if (errno != ENOENT) {
        perror("inspect output target");
        return -1;
    } else {
        mode_t mask = umask(0);
        umask(mask);
        *output_mode = (mode_t)(0666 & ~mask);
    }
    return 0;
}

static int open_atomic_output(const char *path, mode_t output_mode,
                              FILE **file, char **temp_path)
{
    size_t path_length = strlen(path);
    static const char suffix[] = ".tmp.XXXXXX";

    if (path_length > SIZE_MAX - sizeof(suffix)) {
        errno = ENAMETOOLONG;
        perror("temporary output path");
        return -1;
    }

    char *temporary = malloc(path_length + sizeof(suffix));
    if (!temporary) {
        perror("allocate temporary output path");
        return -1;
    }
    memcpy(temporary, path, path_length);
    memcpy(temporary + path_length, suffix, sizeof(suffix));

    int descriptor = mkstemp(temporary);
    if (descriptor < 0) {
        perror("create temporary output");
        free(temporary);
        return -1;
    }

    if (fchmod(descriptor, output_mode) < 0) {
        int saved_errno = errno;
        close(descriptor);
        unlink(temporary);
        free(temporary);
        errno = saved_errno;
        perror("set temporary output permissions");
        return -1;
    }

    FILE *stream = fdopen(descriptor, "wb");
    if (!stream) {
        int saved_errno = errno;
        close(descriptor);
        unlink(temporary);
        free(temporary);
        errno = saved_errno;
        perror("open temporary output stream");
        return -1;
    }

    *file = stream;
    *temp_path = temporary;
    return 0;
}

static int discard_atomic_output(FILE **file, char **temp_path)
{
    int result = 0;

    if (*file) {
        FILE *stream = *file;
        *file = NULL;
        if (fclose(stream) != 0) {
            perror("close incomplete output");
            result = -1;
        }
    }
    if (*temp_path) {
        if (unlink(*temp_path) < 0 && errno != ENOENT) {
            perror("remove incomplete output");
            result = -1;
        }
        free(*temp_path);
        *temp_path = NULL;
    }
    return result;
}

static int publish_atomic_output(FILE **file, char **temp_path,
                                 const char *target_path)
{
    int saved_errno = 0;
    const char *operation = NULL;
    FILE *stream = *file;

    if (fflush(stream) != 0) {
        saved_errno = errno;
        operation = "flush output";
    }
    if (!operation && fsync(fileno(stream)) != 0) {
        saved_errno = errno;
        operation = "sync output";
    }

    *file = NULL;
    if (fclose(stream) != 0 && !operation) {
        saved_errno = errno;
        operation = "close output";
    }

    if (operation) {
        errno = saved_errno;
        perror(operation);
        discard_atomic_output(file, temp_path);
        return -1;
    }

    if (rename(*temp_path, target_path) != 0) {
        saved_errno = errno;
        perror("publish output");
        discard_atomic_output(file, temp_path);
        errno = saved_errno;
        return -1;
    }

    free(*temp_path);
    *temp_path = NULL;
    return 0;
}

static void argb_to_rgb565(const uint32_t *source, uint16_t *destination,
                           size_t count)
{
    for (size_t i = 0; i < count; i++) {
        uint32_t pixel = source[i];
        uint8_t red = (uint8_t)((pixel >> 16) & 0xff);
        uint8_t green = (uint8_t)((pixel >> 8) & 0xff);
        uint8_t blue = (uint8_t)(pixel & 0xff);
        destination[i] = (uint16_t)(((red >> 3) << 11) |
                                    ((green >> 2) << 5) | (blue >> 3));
    }
}

int main(int argc, char *argv[])
{
    int result = 1;
    int engine_started = 0;
    Tvg_Canvas canvas = NULL;
    Tvg_Animation animation = NULL;
    uint32_t *argb = NULL;
    uint16_t *rgb565 = NULL;
    uint8_t *packed = NULL;
    FILE *output = NULL;
    char *temp_path = NULL;

    if (argc != 6 && argc != 7) {
        fprintf(stderr, "usage: %s <lottie.json> <width> <height> <fps> <out.lsba> [--loop]\n",
                argv[0]);
        return 1;
    }
    if (argc == 7 && strcmp(argv[6], "--loop") != 0) {
        fprintf(stderr, "unknown option: %s\n", argv[6]);
        return 1;
    }

    const char *lottie_path = argv[1];
    const char *out_path = argv[5];
    int loop = argc == 7;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    if (parse_positive_u32(argv[2], "width", &width) < 0 ||
        parse_positive_u32(argv[3], "height", &height) < 0 ||
        parse_positive_u32(argv[4], "fps", &fps) < 0)
        return 1;

    uint32_t interval_ms = 1000u / fps;
    if (interval_ms == 0) {
        fprintf(stderr, "fps must not exceed 1000 (stream interval would be zero)\n");
        return 1;
    }

    if (width > INT_MAX || height > INT_MAX) {
        fprintf(stderr, "width and height must not exceed %d\n", INT_MAX);
        return 1;
    }
    uint64_t pixel_count_value = (uint64_t)width * (uint64_t)height;
    if (pixel_count_value > SIZE_MAX ||
        pixel_count_value > UINT32_MAX / sizeof(uint32_t)) {
        fprintf(stderr, "width and height exceed 32-bit render-buffer limits\n");
        return 1;
    }
    size_t pixel_count = (size_t)pixel_count_value;
    if (pixel_count > SIZE_MAX / sizeof(*rgb565) ||
        pixel_count > SIZE_MAX / sizeof(*argb)) {
        fprintf(stderr, "width and height overflow host allocation sizes\n");
        return 1;
    }
    size_t frame_bytes = pixel_count * sizeof(*rgb565);
    if (frame_bytes > ULONG_MAX) {
        fprintf(stderr, "frame is too large for zlib on this host\n");
        return 1;
    }
    uLongf compressed_bound = compressBound((uLong)frame_bytes);
    if (compressed_bound >
        (uLongf)(STREAM_MAX_PAYLOAD_BYTES - sizeof(uint32_t))) {
        fprintf(stderr, "frame exceeds stream payload limit\n");
        return 1;
    }
    mode_t output_mode;
    if (target_is_publishable(out_path, &output_mode) < 0)
        return 1;

    if (tvg_engine_init(0) != TVG_RESULT_SUCCESS) {
        fprintf(stderr, "tvg_engine_init failed\n");
        goto cleanup;
    }
    engine_started = 1;

    canvas = tvg_swcanvas_create(TVG_ENGINE_OPTION_DEFAULT);
    argb = calloc(pixel_count, sizeof(*argb));
    rgb565 = malloc(frame_bytes);
    packed = malloc((size_t)compressed_bound);
    if (!canvas || !argb || !rgb565 || !packed) {
        fprintf(stderr, "allocation failed\n");
        goto cleanup;
    }
    if (tvg_swcanvas_set_target(canvas, argb, width, width, height,
                                TVG_COLORSPACE_ARGB8888) !=
        TVG_RESULT_SUCCESS) {
        fprintf(stderr, "tvg_swcanvas_set_target failed\n");
        goto cleanup;
    }

    animation = tvg_animation_new();
    if (!animation) {
        fprintf(stderr, "tvg_animation_new failed\n");
        goto cleanup;
    }
    Tvg_Paint picture = tvg_animation_get_picture(animation);
    if (!picture || tvg_picture_load(picture, lottie_path) != TVG_RESULT_SUCCESS) {
        fprintf(stderr, "tvg_picture_load(%s) failed\n", lottie_path);
        goto cleanup;
    }

    float picture_width = 0;
    float picture_height = 0;
    if (tvg_picture_get_size(picture, &picture_width, &picture_height) !=
        TVG_RESULT_SUCCESS) {
        fprintf(stderr, "tvg_picture_get_size failed\n");
        goto cleanup;
    }
    if (picture_width > 0 && picture_height > 0) {
        float scale_x = (float)width / picture_width;
        float scale_y = (float)height / picture_height;
        float scale = scale_x < scale_y ? scale_x : scale_y;
        if (tvg_paint_scale(picture, scale) != TVG_RESULT_SUCCESS ||
            tvg_paint_translate(picture,
                                (width - picture_width * scale) / 2.0f,
                                (height - picture_height * scale) / 2.0f) !=
            TVG_RESULT_SUCCESS) {
            fprintf(stderr, "picture transform failed\n");
            goto cleanup;
        }
    }

    float total_frames = 0;
    float duration = 0;
    if (tvg_animation_get_total_frame(animation, &total_frames) !=
            TVG_RESULT_SUCCESS ||
        tvg_animation_get_duration(animation, &duration) != TVG_RESULT_SUCCESS ||
        !isfinite(total_frames) || total_frames < 1.0f) {
        fprintf(stderr, "animation has no valid frames\n");
        goto cleanup;
    }
    if (!(duration > 0.0f) || !isfinite(duration))
        duration = total_frames / (float)fps;
    if (!(duration > 0.0f) || !isfinite(duration)) {
        fprintf(stderr, "animation has invalid duration\n");
        goto cleanup;
    }

    /* The stream stores an integer millisecond interval, so derive both the
     * sample count and source-frame step from that completed representation.
     * Otherwise non-divisor rates (for example 501 fps -> 1 ms) would encode
     * a playback duration unrelated to the frames we packed. */
    double count_value = (double)duration * 1000.0 / interval_ms;
    if (!isfinite(count_value) || count_value > STREAM_MAX_FRAME_COUNT) {
        fprintf(stderr, "animation exceeds %u-frame stream limit\n",
                STREAM_MAX_FRAME_COUNT);
        goto cleanup;
    }
    uint32_t frame_count = count_value < 1.0 ? 1u : (uint32_t)count_value;
    float native_fps = total_frames / duration;
    float step = native_fps * (float)interval_ms / 1000.0f;
    if (!(native_fps > 0.0f) || !isfinite(native_fps) ||
        !(step > 0.0f) || !isfinite(step)) {
        fprintf(stderr, "animation has invalid timing\n");
        goto cleanup;
    }

    struct stream_header header = {
        .magic = { 'L', 'S', 'B', 'A' },
        .version = STREAM_VERSION,
        .width = width,
        .height = height,
        .format = STREAM_FMT_RGB565LE,
        .frame_count = frame_count,
        .interval_ms = interval_ms,
        .flags = loop ? STREAM_FLAG_LOOP : 0,
    };
    enum stream_validation validation = stream_validate_header(&header);
    if (validation != STREAM_VALID) {
        fprintf(stderr, "refusing invalid stream header: %s\n",
                stream_validation_name(validation));
        goto cleanup;
    }

    if (tvg_canvas_add(canvas, picture) != TVG_RESULT_SUCCESS) {
        fprintf(stderr, "tvg_canvas_add failed\n");
        goto cleanup;
    }
    if (open_atomic_output(out_path, output_mode, &output, &temp_path) < 0)
        goto cleanup;
    if (fwrite(&header, sizeof(header), 1, output) != 1) {
        perror("write header");
        goto cleanup;
    }

    size_t total_packed = 0;
    size_t payload_size = 0;
    for (uint32_t i = 0; i < frame_count; i++) {
        float frame = (float)i * step;
        if (i == frame_count - 1 || frame > total_frames - 1.0f)
            frame = total_frames - 1.0f;

        Tvg_Result frame_result = tvg_animation_set_frame(animation, frame);
        if ((frame_result != TVG_RESULT_SUCCESS &&
             frame_result != TVG_RESULT_INSUFFICIENT_CONDITION) ||
            tvg_canvas_update(canvas) != TVG_RESULT_SUCCESS ||
            tvg_canvas_draw(canvas, true) != TVG_RESULT_SUCCESS ||
            tvg_canvas_sync(canvas) != TVG_RESULT_SUCCESS) {
            fprintf(stderr, "render failed on frame %u\n", i);
            goto cleanup;
        }
        argb_to_rgb565(argb, rgb565, pixel_count);

        uLongf compressed_length = compressed_bound;
        if (compress2(packed, &compressed_length, (const Bytef *)rgb565,
                      (uLong)frame_bytes, 9) != Z_OK) {
            fprintf(stderr, "compress failed on frame %u\n", i);
            goto cleanup;
        }
        if (compressed_length > UINT32_MAX ||
            payload_size > STREAM_MAX_PAYLOAD_BYTES - sizeof(uint32_t) ||
            compressed_length > STREAM_MAX_PAYLOAD_BYTES - payload_size -
                                sizeof(uint32_t)) {
            fprintf(stderr, "stream payload exceeds %u-byte limit\n",
                    STREAM_MAX_PAYLOAD_BYTES);
            goto cleanup;
        }

        uint32_t length = (uint32_t)compressed_length;
        if (fwrite(&length, sizeof(length), 1, output) != 1 ||
            fwrite(packed, 1, (size_t)compressed_length, output) !=
                (size_t)compressed_length) {
            perror("write frame");
            goto cleanup;
        }
        total_packed += (size_t)compressed_length;
        payload_size += sizeof(uint32_t) + (size_t)compressed_length;
    }

    if (publish_atomic_output(&output, &temp_path, out_path) < 0)
        goto cleanup;

    fprintf(stderr,
            "%s: %u frames, %" PRIu32 "x%" PRIu32
            ", %ums interval, %.2f MiB (mean %zu KiB/frame)%s\n",
            out_path, frame_count, width, height, header.interval_ms,
            total_packed / 1024.0 / 1024.0,
            total_packed / frame_count / 1024,
            loop ? ", looping" : "");
    result = 0;

cleanup:
    if (output || temp_path) {
        if (discard_atomic_output(&output, &temp_path) < 0)
            result = 1;
    }
    if (animation)
        tvg_animation_del(animation);
    if (canvas)
        tvg_canvas_destroy(canvas);
    if (engine_started)
        tvg_engine_term();
    free(argb);
    free(rgb565);
    free(packed);
    return result;
}
