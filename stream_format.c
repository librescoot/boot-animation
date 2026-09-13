#include "stream.h"

#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(struct stream_header) == 32,
               "stream header layout must remain stable");

enum stream_validation stream_validate_header(const struct stream_header *header)
{
    if (!header || memcmp(header->magic, STREAM_MAGIC, 4) != 0)
        return STREAM_INVALID_MAGIC;
    if (header->version != STREAM_VERSION)
        return STREAM_INVALID_VERSION;
    if (header->format != STREAM_FMT_RGB565LE)
        return STREAM_INVALID_FORMAT;
    if (header->width == 0 || header->height == 0)
        return STREAM_INVALID_GEOMETRY;
    if (header->frame_count == 0)
        return STREAM_INVALID_FRAME_COUNT;
    if (header->frame_count > STREAM_MAX_FRAME_COUNT)
        return STREAM_OVERSIZED_FRAME_COUNT;
    if (header->interval_ms == 0)
        return STREAM_INVALID_INTERVAL;
    if ((uint64_t)header->frame_count * header->interval_ms >
        STREAM_MAX_DURATION_MS)
        return STREAM_OVERSIZED_DURATION;
    if (header->flags & ~STREAM_FLAG_LOOP)
        return STREAM_INVALID_FLAGS;
    return STREAM_VALID;
}

enum stream_validation stream_validate_payload_size(size_t payload_size)
{
    return payload_size > STREAM_MAX_PAYLOAD_BYTES ?
           STREAM_OVERSIZED_PAYLOAD : STREAM_VALID;
}

size_t stream_frame_compressed_limit(size_t expected_frame_bytes)
{
    /* zlib's bound is source + tiny per-block overhead. One eighth plus 64 is
     * deliberately more conservative while remaining bounded and zlib-free. */
    if (expected_frame_bytes > SIZE_MAX - expected_frame_bytes / 8u - 64u)
        return SIZE_MAX;
    return expected_frame_bytes + expected_frame_bytes / 8u + 64u;
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           (uint32_t)data[1] << 8 |
           (uint32_t)data[2] << 16 |
           (uint32_t)data[3] << 24;
}

enum stream_validation stream_index_frames(const uint8_t *data, size_t size,
                                            uint32_t frame_count,
                                            size_t max_compressed_length,
                                            uint32_t *compressed_lengths,
                                            size_t *offsets)
{
    enum stream_validation validation = stream_validate_payload_size(size);
    if (validation != STREAM_VALID)
        return validation;
    if (frame_count == 0)
        return STREAM_INVALID_FRAME_COUNT;
    if (frame_count > STREAM_MAX_FRAME_COUNT)
        return STREAM_OVERSIZED_FRAME_COUNT;
    if ((!data && size != 0) || !compressed_lengths || !offsets)
        return STREAM_TRUNCATED_FRAME_TABLE;

    size_t offset = 0;
    for (uint32_t i = 0; i < frame_count; i++) {
        if (size - offset < 4)
            return STREAM_TRUNCATED_FRAME_TABLE;

        uint32_t length = read_le32(data + offset);
        offset += 4;
        if (length == 0)
            return STREAM_INVALID_FRAME_LENGTH;
        if ((size_t)length > max_compressed_length)
            return STREAM_OVERSIZED_FRAME;
        if ((size_t)length > size - offset)
            return STREAM_TRUNCATED_FRAME_TABLE;

        compressed_lengths[i] = length;
        offsets[i] = offset;
        offset += length;
    }

    if (offset != size)
        return STREAM_TRAILING_DATA;
    return STREAM_VALID;
}

const char *stream_validation_name(enum stream_validation result)
{
    switch (result) {
    case STREAM_VALID:
        return "valid";
    case STREAM_INVALID_MAGIC:
        return "bad magic";
    case STREAM_INVALID_VERSION:
        return "unsupported version";
    case STREAM_INVALID_FORMAT:
        return "unsupported stream format";
    case STREAM_INVALID_GEOMETRY:
        return "invalid geometry";
    case STREAM_INVALID_FRAME_COUNT:
        return "invalid frame count";
    case STREAM_OVERSIZED_FRAME_COUNT:
        return "frame count exceeds operational limit";
    case STREAM_INVALID_INTERVAL:
        return "invalid frame interval";
    case STREAM_OVERSIZED_DURATION:
        return "stream duration exceeds operational limit";
    case STREAM_INVALID_FLAGS:
        return "unsupported flags";
    case STREAM_OVERSIZED_PAYLOAD:
        return "stream payload exceeds operational limit";
    case STREAM_TRUNCATED_FRAME_TABLE:
        return "truncated frame table";
    case STREAM_INVALID_FRAME_LENGTH:
        return "invalid compressed frame length";
    case STREAM_OVERSIZED_FRAME:
        return "compressed frame exceeds safe bound";
    case STREAM_TRAILING_DATA:
        return "trailing data";
    }
    return "unknown validation error";
}
