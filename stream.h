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

#include <stddef.h>
#include <stdint.h>

#define STREAM_MAGIC        "LSBA"
#define STREAM_VERSION      1u

#define STREAM_FMT_RGB565LE 0u

#define STREAM_FLAG_LOOP    (1u << 0)

/* Operational limits keep malformed boot-time input from causing large
 * allocations. Shipped streams are under 300 frames and 3 MiB. */
#define STREAM_MAX_FRAME_COUNT   4096u
#define STREAM_MAX_DURATION_MS   UINT32_MAX
#define STREAM_MAX_PAYLOAD_BYTES (16u * 1024u * 1024u)

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

enum stream_validation {
    STREAM_VALID = 0,
    STREAM_INVALID_MAGIC,
    STREAM_INVALID_VERSION,
    STREAM_INVALID_FORMAT,
    STREAM_INVALID_GEOMETRY,
    STREAM_INVALID_FRAME_COUNT,
    STREAM_OVERSIZED_FRAME_COUNT,
    STREAM_INVALID_INTERVAL,
    STREAM_OVERSIZED_DURATION,
    STREAM_INVALID_FLAGS,
    STREAM_OVERSIZED_PAYLOAD,
    STREAM_TRUNCATED_FRAME_TABLE,
    STREAM_INVALID_FRAME_LENGTH,
    STREAM_OVERSIZED_FRAME,
    STREAM_TRAILING_DATA,
};

enum stream_validation stream_validate_header(const struct stream_header *header);
enum stream_validation stream_validate_payload_size(size_t payload_size);
size_t stream_frame_compressed_limit(size_t expected_frame_bytes);
enum stream_validation stream_index_frames(const uint8_t *data, size_t size,
                                            uint32_t frame_count,
                                            size_t max_compressed_length,
                                            uint32_t *compressed_lengths,
                                            size_t *offsets);
const char *stream_validation_name(enum stream_validation result);

#endif /* BOOT_ANIMATION_STREAM_H */
