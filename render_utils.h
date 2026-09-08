#ifndef BOOT_ANIMATION_RENDER_UTILS_H
#define BOOT_ANIMATION_RENDER_UTILS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

enum fb_pixel_format {
    FB_PIXEL_RGB565,
    FB_PIXEL_XRGB8888,
    FB_PIXEL_ARGB8888,
};

struct fb_channel_layout {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
};

struct fb_surface {
    uint8_t *mapping;
    size_t mapping_size;
    size_t visible_offset;
    size_t stride;
    uint32_t width;
    uint32_t height;
    enum fb_pixel_format format;
};

int fb_surface_init(struct fb_surface *surface, void *mapping, size_t mapping_size,
                    uint32_t width, uint32_t height,
                    uint32_t virtual_width, uint32_t virtual_height,
                    uint32_t xoffset, uint32_t yoffset,
                    uint32_t bits_per_pixel, uint32_t line_length,
                    struct fb_channel_layout red,
                    struct fb_channel_layout green,
                    struct fb_channel_layout blue,
                    struct fb_channel_layout alpha,
                    const char **reason);

const char *fb_pixel_format_name(enum fb_pixel_format format);
uint16_t argb_to_rgb565_pixel(uint32_t pixel);
uint32_t rgb565_to_32_pixel(uint16_t pixel, enum fb_pixel_format format);
void fb_copy_argb(struct fb_surface *surface, const uint32_t *source);
void fb_copy_rgb565(struct fb_surface *surface, const uint16_t *source,
                    const uint32_t *rgb565_lut);
void fb_clear_visible(struct fb_surface *surface);

void timespec_add_ms(struct timespec *value, uint64_t milliseconds);
uint64_t timespec_elapsed_ms(const struct timespec *start,
                             const struct timespec *now);
uint32_t cadence_frame_index(uint64_t elapsed_ms, uint32_t interval_ms,
                             uint32_t frame_count, int *complete);
uint64_t cadence_deadline_ms(uint32_t slot, uint32_t interval_ms);
int timeline_complete(uint64_t elapsed_ms, uint64_t duration_ms);
uint32_t fade_step_at(uint64_t elapsed_ms, uint32_t fade_ms,
                      uint32_t steps, uint32_t next_step);
uint64_t fade_deadline_ms(uint32_t step, uint32_t fade_ms, uint32_t steps);

int animation_stream_path(char *path, size_t path_size, const char *input_path);
int animation_live_path(char *path, size_t path_size, const char *input_path);

#endif
