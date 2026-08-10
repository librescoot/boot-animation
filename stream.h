/*
 * Prerendered animation stream container, shared by the player and the
 * build-time packer.
 *
 * Layout, little endian, no padding (all fields are u32 after the magic):
 *
 *   0   char[4]  "LSBA"
 *   4   u32      version
 *   8   u32      width
 *   12  u32      height
 *   16  u32      format             STREAM_FMT_*
 *   20  u32      frame_count
 *   24  u32      interval_ms        playback pacing
 *   28  u32      flags              STREAM_FLAG_*
 *   32  frames, each: u32 compressed_len, then that many bytes of zlib data
 *
 * Frames are compressed independently rather than as one stream. It costs
 * almost nothing in size (the frames share little beyond a black background,
 * which zlib finds within each frame anyway) and buys two things the player
 * wants: scratch memory stays at one frame no matter how long the animation
 * is, and any frame can be decoded without touching its predecessors, so
 * playback can skip ahead when it falls behind.
 *
 * There is no on-disk offset table; the player walks the length prefixes once
 * at load to build one in memory, which needs no decompression.
 */

#ifndef BOOT_ANIMATION_STREAM_H
#define BOOT_ANIMATION_STREAM_H

#include <stdint.h>

#define STREAM_MAGIC        "LSBA"
#define STREAM_VERSION      1u

#define STREAM_FMT_RGB565LE 0u

#define STREAM_FLAG_LOOP    (1u << 0)

struct stream_header {
    char magic[4];
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t frame_count;
    uint32_t interval_ms;
    uint32_t flags;
};

#endif /* BOOT_ANIMATION_STREAM_H */
