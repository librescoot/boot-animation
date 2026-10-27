#define _POSIX_C_SOURCE 200809L
#define clock_gettime playback_clock_gettime
#define clock_nanosleep playback_clock_nanosleep
#define main player_entrypoint
#include "../main.c"
#undef main
#undef clock_gettime
#undef clock_nanosleep

#include <assert.h>

static uint64_t now_ms;
static uint64_t interrupt_ms;
static uint16_t framebuffer[64];
static uint16_t observed[64];
static size_t observed_count;

int playback_clock_gettime(clockid_t clock, struct timespec *value)
{
    assert(clock == CLOCK_MONOTONIC);
    value->tv_sec = (time_t)(now_ms / 1000);
    value->tv_nsec = (long)(now_ms % 1000) * 1000000;
    return 0;
}

int playback_clock_nanosleep(clockid_t clock, int flags,
                             const struct timespec *deadline,
                             struct timespec *remaining)
{
    (void)remaining;
    assert(clock == CLOCK_MONOTONIC && flags == TIMER_ABSTIME);
    assert(observed_count < sizeof(observed) / sizeof(observed[0]));
    observed[observed_count++] = framebuffer[0];
    now_ms = (uint64_t)deadline->tv_sec * 1000 +
             (uint64_t)deadline->tv_nsec / 1000000;
    if (now_ms >= interrupt_ms) {
        handle_signal(SIGTERM);
        interrupt_ms = UINT64_MAX;
    }
    return 0;
}

static struct fb_surface reset(void)
{
    now_ms = 0;
    interrupt_ms = UINT64_MAX;
    stop_signals = 0;
    observed_count = 0;
    memset(framebuffer, 0, sizeof(framebuffer));
    struct fb_surface surface = {
        .mapping = (uint8_t *)framebuffer,
        .mapping_size = sizeof(framebuffer),
        .stride = 16,
        .width = 8,
        .height = 8,
        .format = FB_PIXEL_RGB565,
    };
    return surface;
}

static void write_stream(const char *path, uint16_t first, uint16_t last,
                          int corrupt)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    struct stream_header header = {
        .magic = STREAM_MAGIC,
        .version = STREAM_VERSION,
        .width = 8,
        .height = 8,
        .format = STREAM_FMT_RGB565LE,
        .frame_count = 2,
        .interval_ms = 40,
        .flags = STREAM_FLAG_LOOP,
    };
    assert(fwrite(&header, sizeof(header), 1, file) == 1);
    for (unsigned frame = 0; frame < 2; frame++) {
        uint16_t pixels[64];
        for (unsigned i = 0; i < 64; i++)
            pixels[i] = frame ? last : first;
        uint8_t encoded[256];
        uLongf length = sizeof(encoded);
        assert(compress(encoded, &length, (const Bytef *)pixels,
                         sizeof(pixels)) == Z_OK);
        if (corrupt)
            encoded[0] ^= 0xff;
        uint32_t size = (uint32_t)length;
        assert(fwrite(&size, sizeof(size), 1, file) == 1);
        assert(fwrite(encoded, 1, size, file) == size);
    }
    assert(fclose(file) == 0);
}

static void write_live(const char *path)
{
    FILE *file = fopen(path, "w");
    assert(file);
    assert(fputs("{\"v\":\"5.7.5\",\"fr\":25,\"ip\":0,\"op\":2,"
                 "\"w\":8,\"h\":8,\"assets\":[],\"layers\":[{\"ty\":1,"
                 "\"ip\":0,\"op\":2,\"st\":0,\"sw\":8,\"sh\":8,"
                 "\"sc\":\"#00ff00\",\"ks\":{\"o\":{\"a\":0,\"k\":100},"
                 "\"r\":{\"a\":0,\"k\":0},\"p\":{\"a\":0,\"k\":[0,0,0]},"
                 "\"a\":{\"a\":0,\"k\":[0,0,0]},\"s\":{\"a\":0,"
                 "\"k\":[100,100,100]}}}]}", file) >= 0);
    assert(fclose(file) == 0);
}

int main(void)
{
    assert(unsetenv("NOTIFY_SOCKET") == 0);
    alarm(10);
    char directory[] = "/tmp/boot-playback-test-XXXXXX";
    assert(mkdtemp(directory));
    char intro[PATH_MAX], loop[PATH_MAX], live[PATH_MAX], corrupt[PATH_MAX];
    char corrupt_json[PATH_MAX], missing[PATH_MAX];
    snprintf(intro, sizeof(intro), "%s/intro.lsba", directory);
    snprintf(loop, sizeof(loop), "%s/loop.lsba", directory);
    snprintf(live, sizeof(live), "%s/live.json", directory);
    snprintf(corrupt, sizeof(corrupt), "%s/corrupt.lsba", directory);
    snprintf(corrupt_json, sizeof(corrupt_json), "%s/corrupt.json", directory);
    snprintf(missing, sizeof(missing), "%s/missing.json", directory);
    write_stream(intro, 0xf800, 0x07e0, 0);
    write_stream(loop, 0x001f, 0xffff, 0);
    write_stream(corrupt, 0xf800, 0xf800, 1);
    write_live(live);
    write_live(corrupt_json);

    struct fb_surface surface = reset();
    int notified = 0;
    assert(play_animation(intro, &surface, 25, 80, 1, 1, &notified) == 1);
    assert(notified == 1 && now_ms == 80 && stop_signals == 0);
    assert(observed_count == 2 && observed[0] == 0xf800 && observed[1] == 0x07e0);
    assert(framebuffer[0] == 0x07e0);
    interrupt_ms = 240;
    assert(play_animation(loop, &surface, 25, 0, 0, 0, &notified) == 0);
    assert(observed_count == 6);
    for (size_t i = 2; i < observed_count; i++)
        assert(observed[i] == (i % 2 ? 0xffff : 0x001f));
    assert(framebuffer[0] == 0);

    surface = reset();
    notified = 0;
    assert(play_animation(live, &surface, 25, 80, 1, 1, &notified) == 1);
    assert(notified == 1 && now_ms == 80 && framebuffer[0] == 0x07e0);

    surface = reset();
    notified = 0;
    assert(play_animation(corrupt, &surface, 25, 80, 1, 1, &notified) == 1);
    assert(notified == 1 && framebuffer[0] == 0x07e0);

    surface = reset();
    framebuffer[0] = 0x1234;
    notified = 0;
    assert(play_animation(missing, &surface, 25, 0, 1, 1, &notified) == -1);
    assert(framebuffer[0] == 0x1234 && notified == 0);

    surface = reset();
    notified = 0;
    interrupt_ms = 20;
    assert(play_animation(intro, &surface, 25, 0, 1, 1, &notified) == 0);
    assert(now_ms == 40 && framebuffer[0] == 0);

    surface = reset();
    notified = 0;
    interrupt_ms = 20;
    assert(play_animation(live, &surface, 25, 0, 1, 1, &notified) == 0);
    assert(now_ms == 40 && framebuffer[0] == 0);

    for (unsigned kind = 0; kind < 2; kind++) {
        surface = reset();
        notified = 0;
        interrupt_ms = 20;
        assert(play_animation(kind ? live : intro, &surface, 25, 80,
                               1, 1, &notified) == 0);
        assert(now_ms == 120 && framebuffer[0] == 0);
        assert(observed_count == 3 && observed[1] == 0x07e0 &&
               observed[2] == 0x03e0);
    }

    surface = reset();
    uint32_t framebuffer32[64] = {0};
    surface.mapping = (uint8_t *)framebuffer32;
    surface.mapping_size = sizeof(framebuffer32);
    surface.stride = 32;
    surface.format = FB_PIXEL_ARGB8888;
    notified = 0;
    assert(play_animation(intro, &surface, 25, 80, 1, 1, &notified) == 1);
    assert(framebuffer32[0] == 0xff00ff00u &&
           framebuffer32[63] == 0xff00ff00u);

    char *arguments[] = {"boot-animation", "--intro", NULL};
    assert(player_entrypoint(2, arguments) == 1);
    assert(unlink(intro) == 0 && unlink(loop) == 0 && unlink(live) == 0);
    assert(unlink(corrupt) == 0 && unlink(corrupt_json) == 0);
    assert(rmdir(directory) == 0);
    alarm(0);
    puts("intro/loop/live/fallback/stop playback tests passed");
    return 0;
}
