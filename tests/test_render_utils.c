#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render_utils.h"
#include "signal_utils.h"
#include "stream.h"

static int failures;

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "%s:%d: check failed: %s\n",                       \
                __FILE__, __LINE__, #condition);                             \
        failures++;                                                          \
    }                                                                        \
} while (0)

static struct fb_channel_layout channel(uint32_t offset, uint32_t length)
{
    struct fb_channel_layout result = { offset, length, 0 };
    return result;
}

static void test_rgb565_padded_stride(void)
{
    uint8_t mapping[16];
    memset(mapping, 0xa5, sizeof(mapping));
    struct fb_surface surface;
    const char *reason = NULL;

    CHECK(fb_surface_init(&surface, mapping, sizeof(mapping),
                          2, 2, 2, 2, 0, 0, 16, 8,
                          channel(11, 5), channel(5, 6), channel(0, 5),
                          channel(0, 0), &reason) == 0);
    uint16_t source[] = { 0xf800, 0x07e0, 0x001f, 0xffff };
    fb_copy_rgb565(&surface, source, NULL);

    CHECK(((uint16_t *)mapping)[0] == 0xf800);
    CHECK(((uint16_t *)mapping)[1] == 0x07e0);
    CHECK(((uint16_t *)(mapping + 8))[0] == 0x001f);
    CHECK(((uint16_t *)(mapping + 8))[1] == 0xffff);
    for (size_t i = 4; i < 8; i++)
        CHECK(mapping[i] == 0xa5);
    for (size_t i = 12; i < 16; i++)
        CHECK(mapping[i] == 0xa5);

    uint32_t argb[] = { 0xffff0000, 0xff00ff00,
                        0xff0000ff, 0xffffffff };
    fb_copy_argb(&surface, argb);
    CHECK(((uint16_t *)mapping)[0] == 0xf800);
    CHECK(((uint16_t *)mapping)[1] == 0x07e0);
    CHECK(((uint16_t *)(mapping + 8))[0] == 0x001f);
    CHECK(((uint16_t *)(mapping + 8))[1] == 0xffff);
}

static void test_32bpp_padded_stride(void)
{
    uint8_t mapping[24];
    memset(mapping, 0x5a, sizeof(mapping));
    struct fb_surface surface;

    CHECK(fb_surface_init(&surface, mapping, sizeof(mapping),
                          2, 2, 2, 2, 0, 0, 32, 12,
                          channel(16, 8), channel(8, 8), channel(0, 8),
                          channel(0, 0), NULL) == 0);
    uint16_t source[] = { 0xf800, 0x07e0, 0x001f, 0xffff };
    fb_copy_rgb565(&surface, source, NULL);

    CHECK(((uint32_t *)mapping)[0] == 0x00ff0000u);
    CHECK(((uint32_t *)mapping)[1] == 0x0000ff00u);
    CHECK(((uint32_t *)(mapping + 12))[0] == 0x000000ffu);
    CHECK(((uint32_t *)(mapping + 12))[1] == 0x00ffffffu);
    for (size_t i = 8; i < 12; i++)
        CHECK(mapping[i] == 0x5a);
    for (size_t i = 20; i < 24; i++)
        CHECK(mapping[i] == 0x5a);

    memset(mapping, 0, sizeof(mapping));
    CHECK(fb_surface_init(&surface, mapping, sizeof(mapping),
                          2, 2, 2, 2, 0, 0, 32, 12,
                          channel(16, 8), channel(8, 8), channel(0, 8),
                          channel(24, 8), NULL) == 0);
    fb_copy_rgb565(&surface, source, NULL);
    CHECK(((uint32_t *)mapping)[0] == 0xffff0000u);
    CHECK(((uint32_t *)mapping)[1] == 0xff00ff00u);
}

static void test_surface_bounds_and_offsets(void)
{
    uint8_t mapping[64];
    memset(mapping, 0xcc, sizeof(mapping));
    struct fb_surface surface;

    CHECK(fb_surface_init(&surface, mapping, sizeof(mapping),
                          2, 1, 4, 2, 1, 1, 16, 8,
                          channel(11, 5), channel(5, 6), channel(0, 5),
                          channel(0, 0), NULL) == 0);
    CHECK(surface.visible_offset == 10);
    uint16_t source[] = { 0x1234, 0xabcd };
    fb_copy_rgb565(&surface, source, NULL);
    CHECK(*(uint16_t *)(mapping + 10) == 0x1234);
    CHECK(*(uint16_t *)(mapping + 12) == 0xabcd);
    CHECK(mapping[8] == 0xcc && mapping[9] == 0xcc && mapping[14] == 0xcc);

    CHECK(fb_surface_init(&surface, mapping, sizeof(mapping),
                          3, 1, 3, 1, 0, 0, 16, 4,
                          channel(11, 5), channel(5, 6), channel(0, 5),
                          channel(0, 0), NULL) < 0);
    CHECK(fb_surface_init(&surface, mapping, 7,
                          2, 1, 2, 1, 0, 0, 32, 8,
                          channel(16, 8), channel(8, 8), channel(0, 8),
                          channel(0, 0), NULL) < 0);
    CHECK(fb_surface_init(&surface, mapping, sizeof(mapping),
                          2, 1, 2, 1, 0, 0, 32, 8,
                          channel(0, 8), channel(8, 8), channel(16, 8),
                          channel(24, 8), NULL) < 0);
}

static void test_time_arithmetic(void)
{
    struct timespec value = { .tv_sec = 10, .tv_nsec = 900000000L };
    timespec_add_ms(&value, 2500);
    CHECK(value.tv_sec == 13);
    CHECK(value.tv_nsec == 400000000L);

    timespec_add_ms(&value, 3000000);
    CHECK(value.tv_sec == 3013);
    CHECK(value.tv_nsec == 400000000L);

    struct timespec start = { .tv_sec = 7, .tv_nsec = 900000000L };
    struct timespec now = { .tv_sec = 10, .tv_nsec = 100000000L };
    CHECK(timespec_elapsed_ms(&start, &now) == 2200);
}

static void test_cadence_selection(void)
{
    int complete = -1;
    CHECK(cadence_frame_index(0, 40, 5, &complete) == 0 && complete == 0);
    CHECK(cadence_frame_index(39, 40, 5, &complete) == 0 && complete == 0);
    CHECK(cadence_frame_index(40, 40, 5, &complete) == 1 && complete == 0);
    CHECK(cadence_frame_index(135, 40, 5, &complete) == 3 && complete == 0);
    CHECK(cadence_frame_index(160, 40, 5, &complete) == 4 && complete == 0);
    CHECK(cadence_frame_index(199, 40, 5, &complete) == 4 && complete == 0);
    CHECK(cadence_frame_index(200, 40, 5, &complete) == 4 && complete == 1);
    CHECK(cadence_frame_index(999, 40, 5, &complete) == 4 && complete == 1);

    CHECK(cadence_frame_index(0, 40, 1, &complete) == 0 && complete == 0);
    CHECK(cadence_frame_index(39, 40, 1, &complete) == 0 && complete == 0);
    CHECK(cadence_frame_index(40, 40, 1, &complete) == 0 && complete == 1);
    CHECK(cadence_deadline_ms(1, 40) == 40);
    CHECK(cadence_deadline_ms(75000, 40) == 3000000u);

    CHECK(!timeline_complete(999, 1000));
    CHECK(timeline_complete(1000, 1000));
}

static void test_fade_timing(void)
{
    CHECK(fade_deadline_ms(1, 1000, 30) == 34);
    CHECK(fade_step_at(33, 1000, 30, 1) == 1);
    CHECK(fade_step_at(34, 1000, 30, 1) == 1);
    CHECK(fade_step_at(34, 1000, 30, 2) == 2);
    CHECK(fade_deadline_ms(29, 1000, 30) == 967);
    CHECK(fade_step_at(999, 1000, 30, 1) == 29);
    CHECK(fade_deadline_ms(30, 1000, 30) == 1000);
    CHECK(fade_step_at(1000, 1000, 30, 1) == 30);
}

static volatile sig_atomic_t test_signal_count;

static void count_signal(int signal_number)
{
    (void)signal_number;
    if (test_signal_count < SIG_ATOMIC_MAX)
        test_signal_count++;
}

static void test_atomic_signal_wait(void)
{
    struct sigaction action;
    struct sigaction previous_term;
    struct sigaction previous_int;
    sigset_t blocked;
    sigset_t previous_mask;

    memset(&action, 0, sizeof(action));
    action.sa_handler = count_signal;
    CHECK(sigemptyset(&action.sa_mask) == 0);
    CHECK(sigaction(SIGTERM, &action, &previous_term) == 0);
    CHECK(sigaction(SIGINT, &action, &previous_int) == 0);
    CHECK(sigemptyset(&blocked) == 0);
    CHECK(sigaddset(&blocked, SIGTERM) == 0);
    CHECK(sigaddset(&blocked, SIGINT) == 0);
    CHECK(sigprocmask(SIG_BLOCK, &blocked, &previous_mask) == 0);

    /* A signal already pending at the check/wait boundary must wake the wait. */
    test_signal_count = 0;
    CHECK(raise(SIGTERM) == 0);
    CHECK(signal_prepare_fade(&test_signal_count, 1) == 0);
    CHECK(test_signal_count == 0);

    /* Consume only the first signal so a second one still aborts the fade. */
    test_signal_count = 2;
    CHECK(signal_prepare_fade(&test_signal_count, 1) == 0);
    CHECK(test_signal_count == 1);
    test_signal_count = 0;
    CHECK(signal_prepare_fade(&test_signal_count, 0) == 0);
    CHECK(test_signal_count == 0);

    CHECK(sigprocmask(SIG_SETMASK, &previous_mask, NULL) == 0);
    CHECK(sigaction(SIGTERM, &previous_term, NULL) == 0);
    CHECK(sigaction(SIGINT, &previous_int, NULL) == 0);
}

static void test_animation_paths(void)
{
    char path[64];
    CHECK(animation_stream_path(path, sizeof(path), "/boot/demo.json") == 0);
    CHECK(strcmp(path, "/boot/demo.lsba") == 0);
    CHECK(animation_live_path(path, sizeof(path), "/boot/demo.json") == 0);
    CHECK(strcmp(path, "/boot/demo.json") == 0);
    CHECK(animation_stream_path(path, sizeof(path), "/boot/demo.lsba") == 0);
    CHECK(strcmp(path, "/boot/demo.lsba") == 0);
    CHECK(animation_live_path(path, sizeof(path), "/boot/demo.lsba") == 0);
    CHECK(strcmp(path, "/boot/demo.json") == 0);
    CHECK(animation_stream_path(path, sizeof(path), "/boot.v1/demo") == 0);
    CHECK(strcmp(path, "/boot.v1/demo.lsba") == 0);
    CHECK(animation_live_path(path, 8, "/boot/demo.lsba") < 0);
}

static void put_u32_le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

static void test_stream_validation(void)
{
    struct stream_header header = {
        .magic = { 'L', 'S', 'B', 'A' },
        .version = STREAM_VERSION,
        .width = 2,
        .height = 2,
        .format = STREAM_FMT_RGB565LE,
        .frame_count = 2,
        .interval_ms = 40,
        .flags = 0,
    };
    CHECK(stream_validate_header(&header) == STREAM_VALID);
    header.frame_count = STREAM_MAX_FRAME_COUNT + 1u;
    CHECK(stream_validate_header(&header) == STREAM_OVERSIZED_FRAME_COUNT);
    header.frame_count = 2;
    header.interval_ms = 0;
    CHECK(stream_validate_header(&header) == STREAM_INVALID_INTERVAL);
    header.interval_ms = UINT32_MAX;
    CHECK(stream_validate_header(&header) == STREAM_OVERSIZED_DURATION);
    header.frame_count = STREAM_MAX_FRAME_COUNT;
    header.interval_ms = UINT32_MAX / STREAM_MAX_FRAME_COUNT;
    CHECK(stream_validate_header(&header) == STREAM_VALID);
    header.interval_ms++;
    CHECK(stream_validate_header(&header) == STREAM_OVERSIZED_DURATION);
    header.frame_count = 2;
    header.interval_ms = 40;
    header.flags = 2;
    CHECK(stream_validate_header(&header) == STREAM_INVALID_FLAGS);
    header.flags = 0;
    header.magic[0] = 'X';
    CHECK(stream_validate_header(&header) == STREAM_INVALID_MAGIC);

    uint8_t payload[13] = {0};
    put_u32_le(payload, 3);
    payload[4] = 1;
    payload[5] = 2;
    payload[6] = 3;
    put_u32_le(payload + 7, 2);
    payload[11] = 4;
    payload[12] = 5;
    uint32_t lengths[2];
    size_t offsets[2];

    size_t frame_limit = stream_frame_compressed_limit(8);
    CHECK(frame_limit >= 3);
    CHECK(stream_validate_payload_size(STREAM_MAX_PAYLOAD_BYTES) == STREAM_VALID);
    CHECK(stream_validate_payload_size((size_t)STREAM_MAX_PAYLOAD_BYTES + 1u) ==
          STREAM_OVERSIZED_PAYLOAD);
    CHECK(stream_index_frames(NULL,
                              (size_t)STREAM_MAX_PAYLOAD_BYTES + 1u,
                              2, frame_limit, lengths, offsets) ==
          STREAM_OVERSIZED_PAYLOAD);
    CHECK(stream_index_frames(payload, sizeof(payload),
                              STREAM_MAX_FRAME_COUNT + 1u, frame_limit,
                              lengths, offsets) == STREAM_OVERSIZED_FRAME_COUNT);

    CHECK(stream_index_frames(payload, sizeof(payload), 2, frame_limit,
                              lengths, offsets) == STREAM_VALID);
    CHECK(lengths[0] == 3 && offsets[0] == 4);
    CHECK(lengths[1] == 2 && offsets[1] == 11);
    CHECK(stream_index_frames(payload, sizeof(payload) - 1, 2, frame_limit,
                              lengths, offsets) == STREAM_TRUNCATED_FRAME_TABLE);
    CHECK(stream_index_frames(payload, sizeof(payload), 3, frame_limit,
                              lengths, offsets) == STREAM_TRUNCATED_FRAME_TABLE);

    put_u32_le(payload, 0);
    CHECK(stream_index_frames(payload, sizeof(payload), 2, frame_limit,
                              lengths, offsets) == STREAM_INVALID_FRAME_LENGTH);
    put_u32_le(payload, (uint32_t)frame_limit + 1u);
    CHECK(stream_index_frames(payload, sizeof(payload), 2, frame_limit,
                              lengths, offsets) == STREAM_OVERSIZED_FRAME);
    put_u32_le(payload, 3);
    CHECK(stream_index_frames(payload, sizeof(payload) - 1, 1, frame_limit,
                              lengths, offsets) == STREAM_TRAILING_DATA);
}

int main(void)
{
    test_rgb565_padded_stride();
    test_32bpp_padded_stride();
    test_surface_bounds_and_offsets();
    test_time_arithmetic();
    test_cadence_selection();
    test_fade_timing();
    test_atomic_signal_wait();
    test_animation_paths();
    test_stream_validation();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("all render utility tests passed");
    return EXIT_SUCCESS;
}
