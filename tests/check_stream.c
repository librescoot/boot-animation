#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "stream.h"

static int fail(const char *message)
{
    fprintf(stderr, "check-stream: %s\n", message);
    return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
    if (argc != 5 && argc != 6)
        return fail("usage: check-stream FILE WIDTH HEIGHT INTERVAL_MS [FRAME_COUNT]");

    uint32_t expected_width = (uint32_t)strtoul(argv[2], NULL, 10);
    uint32_t expected_height = (uint32_t)strtoul(argv[3], NULL, 10);
    uint32_t expected_interval = (uint32_t)strtoul(argv[4], NULL, 10);
    uint32_t expected_frame_count = argc == 6 ?
                                    (uint32_t)strtoul(argv[5], NULL, 10) : 0;
    FILE *file = fopen(argv[1], "rb");
    if (!file)
        return fail("cannot open stream");

    struct stream_header header;
    if (fread(&header, sizeof(header), 1, file) != 1) {
        fclose(file);
        return fail("cannot read header");
    }
    enum stream_validation validation = stream_validate_header(&header);
    if (validation != STREAM_VALID) {
        fclose(file);
        return fail(stream_validation_name(validation));
    }
    if (header.width != expected_width || header.height != expected_height ||
        header.interval_ms != expected_interval ||
        (expected_frame_count != 0 &&
         header.frame_count != expected_frame_count)) {
        fclose(file);
        return fail("header values do not match packer arguments");
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return fail("cannot seek stream");
    }
    long end = ftell(file);
    if (end < (long)sizeof(header) ||
        (unsigned long)(end - (long)sizeof(header)) > STREAM_MAX_PAYLOAD_BYTES ||
        fseek(file, (long)sizeof(header), SEEK_SET) != 0) {
        fclose(file);
        return fail("invalid payload size");
    }
    size_t payload_size = (size_t)(end - (long)sizeof(header));
    uint8_t *payload = malloc(payload_size ? payload_size : 1);
    uint32_t *lengths = malloc((size_t)header.frame_count * sizeof(*lengths));
    size_t *offsets = malloc((size_t)header.frame_count * sizeof(*offsets));
    uint64_t pixels = (uint64_t)header.width * header.height;
    if (pixels > SIZE_MAX / sizeof(uint16_t) ||
        pixels * sizeof(uint16_t) > ULONG_MAX ||
        !payload || !lengths || !offsets) {
        free(payload);
        free(lengths);
        free(offsets);
        fclose(file);
        return fail("allocation size overflow");
    }
    size_t frame_bytes = (size_t)pixels * sizeof(uint16_t);
    uint8_t *frame = malloc(frame_bytes ? frame_bytes : 1);
    if (!frame || fread(payload, 1, payload_size, file) != payload_size) {
        free(frame);
        free(payload);
        free(lengths);
        free(offsets);
        fclose(file);
        return fail("cannot read payload");
    }
    fclose(file);

    validation = stream_index_frames(payload, payload_size, header.frame_count,
                                     stream_frame_compressed_limit(frame_bytes),
                                     lengths, offsets);
    if (validation != STREAM_VALID) {
        free(frame);
        free(payload);
        free(lengths);
        free(offsets);
        return fail(stream_validation_name(validation));
    }

    for (uint32_t i = 0; i < header.frame_count; i++) {
        uLongf output_size = (uLongf)frame_bytes;
        if (uncompress(frame, &output_size, payload + offsets[i], lengths[i]) !=
                Z_OK ||
            output_size != frame_bytes) {
            free(frame);
            free(payload);
            free(lengths);
            free(offsets);
            return fail("frame does not round-trip through zlib");
        }
    }

    free(frame);
    free(payload);
    free(lengths);
    free(offsets);
    puts("stream round-trip passed");
    return EXIT_SUCCESS;
}
