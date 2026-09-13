#include "render_utils.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static int channel_is(struct fb_channel_layout channel,
                      uint32_t offset, uint32_t length)
{
    return channel.offset == offset && channel.length == length &&
           channel.msb_right == 0;
}

static int fail(const char **reason, const char *message)
{
    if (reason)
        *reason = message;
    return -1;
}

int fb_surface_init(struct fb_surface *surface, void *mapping, size_t mapping_size,
                    uint32_t width, uint32_t height,
                    uint32_t virtual_width, uint32_t virtual_height,
                    uint32_t xoffset, uint32_t yoffset,
                    uint32_t bits_per_pixel, uint32_t line_length,
                    struct fb_channel_layout red,
                    struct fb_channel_layout green,
                    struct fb_channel_layout blue,
                    struct fb_channel_layout alpha,
                    const char **reason)
{
    uint32_t bytes_per_pixel;
    enum fb_pixel_format format;

    if (!surface || !mapping)
        return fail(reason, "missing framebuffer mapping");
    if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX)
        return fail(reason, "invalid visible geometry");
    if (virtual_width == 0 || virtual_height == 0 ||
        xoffset > virtual_width || width > virtual_width - xoffset ||
        yoffset > virtual_height || height > virtual_height - yoffset)
        return fail(reason, "visible geometry exceeds virtual framebuffer");

    if (bits_per_pixel == 16 &&
        channel_is(red, 11, 5) && channel_is(green, 5, 6) &&
        channel_is(blue, 0, 5) && alpha.length == 0) {
        bytes_per_pixel = 2;
        format = FB_PIXEL_RGB565;
    } else if (bits_per_pixel == 32 &&
               channel_is(red, 16, 8) && channel_is(green, 8, 8) &&
               channel_is(blue, 0, 8) && alpha.length == 0) {
        bytes_per_pixel = 4;
        format = FB_PIXEL_XRGB8888;
    } else if (bits_per_pixel == 32 &&
               channel_is(red, 16, 8) && channel_is(green, 8, 8) &&
               channel_is(blue, 0, 8) && channel_is(alpha, 24, 8)) {
        bytes_per_pixel = 4;
        format = FB_PIXEL_ARGB8888;
    } else {
        return fail(reason, "unsupported framebuffer pixel layout");
    }

    uint64_t row_end = (uint64_t)(xoffset + width) * bytes_per_pixel;
    if (line_length == 0 || line_length % bytes_per_pixel != 0 ||
        row_end > line_length)
        return fail(reason, "visible row exceeds framebuffer stride");

    uint64_t visible_offset = (uint64_t)yoffset * line_length +
                              (uint64_t)xoffset * bytes_per_pixel;
    uint64_t visible_end = (uint64_t)(yoffset + height - 1) * line_length +
                           row_end;
    if (visible_offset > SIZE_MAX || visible_end > mapping_size)
        return fail(reason, "visible framebuffer exceeds mapped memory");

    surface->mapping = mapping;
    surface->mapping_size = mapping_size;
    surface->visible_offset = (size_t)visible_offset;
    surface->stride = line_length;
    surface->width = width;
    surface->height = height;
    surface->format = format;
    if (reason)
        *reason = NULL;
    return 0;
}

const char *fb_pixel_format_name(enum fb_pixel_format format)
{
    switch (format) {
    case FB_PIXEL_RGB565:
        return "RGB565";
    case FB_PIXEL_XRGB8888:
        return "XRGB8888";
    case FB_PIXEL_ARGB8888:
        return "ARGB8888";
    }
    return "unknown";
}

uint16_t argb_to_rgb565_pixel(uint32_t pixel)
{
    uint16_t red = (uint16_t)((pixel >> 19) & 0x1f);
    uint16_t green = (uint16_t)((pixel >> 10) & 0x3f);
    uint16_t blue = (uint16_t)((pixel >> 3) & 0x1f);
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

uint32_t rgb565_to_32_pixel(uint16_t pixel, enum fb_pixel_format format)
{
    uint32_t red = (pixel >> 11) & 0x1f;
    uint32_t green = (pixel >> 5) & 0x3f;
    uint32_t blue = pixel & 0x1f;
    uint32_t alpha = format == FB_PIXEL_ARGB8888 ? 0xff000000u : 0;

    return alpha |
           (((red << 3) | (red >> 2)) << 16) |
           (((green << 2) | (green >> 4)) << 8) |
           ((blue << 3) | (blue >> 2));
}

void fb_copy_argb(struct fb_surface *surface, const uint32_t *source)
{
    for (uint32_t y = 0; y < surface->height; y++) {
        uint8_t *row = surface->mapping + surface->visible_offset +
                       (size_t)y * surface->stride;
        const uint32_t *source_row = source + (size_t)y * surface->width;

        if (surface->format == FB_PIXEL_RGB565) {
            uint16_t *destination = (uint16_t *)row;
            for (uint32_t x = 0; x < surface->width; x++)
                destination[x] = argb_to_rgb565_pixel(source_row[x]);
        } else if (surface->format == FB_PIXEL_XRGB8888) {
            uint32_t *destination = (uint32_t *)row;
            for (uint32_t x = 0; x < surface->width; x++)
                destination[x] = source_row[x] & 0x00ffffffu;
        } else {
            memcpy(row, source_row, (size_t)surface->width * sizeof(uint32_t));
        }
    }
}

void fb_copy_rgb565(struct fb_surface *surface, const uint16_t *source,
                    const uint32_t *rgb565_lut)
{
    for (uint32_t y = 0; y < surface->height; y++) {
        uint8_t *row = surface->mapping + surface->visible_offset +
                       (size_t)y * surface->stride;
        const uint16_t *source_row = source + (size_t)y * surface->width;

        if (surface->format == FB_PIXEL_RGB565) {
            memcpy(row, source_row, (size_t)surface->width * sizeof(uint16_t));
        } else {
            uint32_t *destination = (uint32_t *)row;
            for (uint32_t x = 0; x < surface->width; x++) {
                uint16_t pixel = source_row[x];
                destination[x] = rgb565_lut ? rgb565_lut[pixel] :
                    rgb565_to_32_pixel(pixel, surface->format);
            }
        }
    }
}

void fb_clear_visible(struct fb_surface *surface)
{
    size_t row_bytes = (size_t)surface->width *
                       (surface->format == FB_PIXEL_RGB565 ? 2u : 4u);
    for (uint32_t y = 0; y < surface->height; y++) {
        uint8_t *row = surface->mapping + surface->visible_offset +
                       (size_t)y * surface->stride;
        memset(row, 0, row_bytes);
    }
}

void timespec_add_ms(struct timespec *value, uint64_t milliseconds)
{
    uint64_t seconds = milliseconds / 1000u;
    uint64_t nanoseconds = (milliseconds % 1000u) * 1000000u;

    value->tv_sec += (time_t)seconds;
    value->tv_nsec += (long)nanoseconds;
    if (value->tv_nsec >= 1000000000L) {
        value->tv_sec++;
        value->tv_nsec -= 1000000000L;
    }
}

uint64_t timespec_elapsed_ms(const struct timespec *start,
                             const struct timespec *now)
{
    if (now->tv_sec < start->tv_sec ||
        (now->tv_sec == start->tv_sec && now->tv_nsec <= start->tv_nsec))
        return 0;

    time_t seconds = now->tv_sec - start->tv_sec;
    long nanoseconds = now->tv_nsec - start->tv_nsec;
    if (nanoseconds < 0) {
        seconds--;
        nanoseconds += 1000000000L;
    }
    return (uint64_t)seconds * 1000u + (uint64_t)nanoseconds / 1000000u;
}

uint32_t cadence_frame_index(uint64_t elapsed_ms, uint32_t interval_ms,
                             uint32_t frame_count, int *complete)
{
    if (interval_ms == 0 || frame_count == 0) {
        if (complete)
            *complete = 1;
        return 0;
    }

    uint64_t index = elapsed_ms / interval_ms;
    if (complete)
        *complete = index >= frame_count;
    if (index >= frame_count)
        index = frame_count - 1u;
    return (uint32_t)index;
}

uint64_t cadence_deadline_ms(uint32_t slot, uint32_t interval_ms)
{
    return (uint64_t)slot * interval_ms;
}

int timeline_complete(uint64_t elapsed_ms, uint64_t duration_ms)
{
    return duration_ms == 0 || elapsed_ms >= duration_ms;
}

uint32_t fade_step_at(uint64_t elapsed_ms, uint32_t fade_ms,
                      uint32_t steps, uint32_t next_step)
{
    if (fade_ms == 0 || steps == 0)
        return steps;
    if (elapsed_ms >= fade_ms)
        return steps;

    uint32_t step = (uint32_t)((elapsed_ms * steps) / fade_ms);
    if (step < next_step)
        step = next_step;
    if (step > steps)
        step = steps;
    return step;
}

uint64_t fade_deadline_ms(uint32_t step, uint32_t fade_ms, uint32_t steps)
{
    if (steps == 0)
        return 0;
    return ((uint64_t)step * fade_ms + steps - 1u) / steps;
}

static const char *path_extension(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(path, '.');
    return dot && (!slash || dot > slash) ? dot : NULL;
}

static int replace_extension(char *path, size_t path_size,
                             const char *input_path, const char *extension,
                             int preserve_non_stream)
{
    if (!path || path_size == 0 || !input_path || !extension)
        return -1;

    const char *dot = path_extension(input_path);
    int is_stream = dot && strcmp(dot, ".lsba") == 0;
    if (preserve_non_stream && !is_stream) {
        int written = snprintf(path, path_size, "%s", input_path);
        return written >= 0 && (size_t)written < path_size ? 0 : -1;
    }

    size_t stem = dot ? (size_t)(dot - input_path) : strlen(input_path);
    if (stem > INT_MAX)
        return -1;
    int written = snprintf(path, path_size, "%.*s%s", (int)stem,
                           input_path, extension);
    return written >= 0 && (size_t)written < path_size ? 0 : -1;
}

int animation_stream_path(char *path, size_t path_size, const char *input_path)
{
    if (!path || path_size == 0 || !input_path)
        return -1;
    const char *dot = path_extension(input_path);
    if (dot && strcmp(dot, ".lsba") == 0) {
        int written = snprintf(path, path_size, "%s", input_path);
        return written >= 0 && (size_t)written < path_size ? 0 : -1;
    }
    return replace_extension(path, path_size, input_path, ".lsba", 0);
}

int animation_live_path(char *path, size_t path_size, const char *input_path)
{
    return replace_extension(path, path_size, input_path, ".json", 1);
}
