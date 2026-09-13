#define _GNU_SOURCE
#include "audio_playback.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <alsa/asoundlib.h>


static volatile sig_atomic_t audio_stop = 0;

struct audio_playback {
    const char *path;
    const char *configured_device;
    pthread_t thread;
    int started;
};

static char *select_audio_device(const char *configured)
{
    if (configured && configured[0] && strcmp(configured, "auto") != 0)
        return strdup(configured);

    void **hints = NULL;
    char *builtin = NULL;
    char *usb = NULL;
    char *fallback = NULL;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0)
        return NULL;

    for (void **hint = hints; *hint; hint++) {
        char *name = snd_device_name_get_hint(*hint, "NAME");
        char *desc = snd_device_name_get_hint(*hint, "DESC");
        char *io = snd_device_name_get_hint(*hint, "IOID");
        int output = !io || strcmp(io, "Input") != 0;
        if (output && name) {
            if (!fallback && strcmp(name, "null") != 0)
                fallback = strdup(name);
            if (!builtin && (strcasestr(name, "tas5720") ||
                             (desc && strcasestr(desc, "tas5720"))))
                builtin = strdup(name);
            if (!usb && (strcasestr(name, "usb") ||
                         (desc && strcasestr(desc, "usb"))))
                usb = strdup(name);
        }
        free(name);
        free(desc);
        free(io);
    }
    snd_device_name_free_hint(hints);

    char *selected = builtin ? builtin : (usb ? usb : fallback);
    if (selected != builtin)
        free(builtin);
    if (selected != usb)
        free(usb);
    if (selected != fallback)
        free(fallback);
    return selected;
}

static int audio_timed_out(const struct timespec *deadline)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec > deadline->tv_sec ||
           (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec);
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void *play_wav(void *arg)
{
    struct audio_playback *audio = arg;
    const char *path = audio->path;
    int fd = -1;
    uint8_t *file = MAP_FAILED;
    snd_pcm_t *pcm = NULL;
    char *device = NULL;
    struct stat st;

    fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) < 0 || st.st_size < 44)
        goto done;

    file = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file == MAP_FAILED)
        goto done;
    if (memcmp(file, "RIFF", 4) != 0 || memcmp(file + 8, "WAVE", 4) != 0)
        goto invalid;

    const uint8_t *fmt = NULL;
    const uint8_t *data = NULL;
    uint32_t fmt_size = 0;
    uint32_t data_size = 0;
    size_t offset = 12;
    while (offset + 8 <= (size_t)st.st_size) {
        uint32_t size = read_le32(file + offset + 4);
        size_t payload = offset + 8;
        if (payload + size > (size_t)st.st_size)
            goto invalid;
        if (memcmp(file + offset, "fmt ", 4) == 0) {
            fmt = file + payload;
            fmt_size = size;
        } else if (memcmp(file + offset, "data", 4) == 0) {
            data = file + payload;
            data_size = size;
        }
        offset = payload + size + (size & 1u);
    }

    if (!fmt || fmt_size < 16 || !data || read_le16(fmt) != 1 ||
        read_le16(fmt + 2) != 2 || read_le32(fmt + 4) != 48000 ||
        read_le16(fmt + 14) != 16) {
invalid:
        fprintf(stderr, "%s: expected 48 kHz stereo 16-bit PCM WAV\n", path);
        goto done;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 15;

    int err = -ENODEV;
    while (!audio_stop && !audio_timed_out(&deadline)) {
        device = select_audio_device(audio->configured_device);
        if (device) {
            err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK,
                               SND_PCM_NONBLOCK);
            if (err >= 0) {
                err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                                         SND_PCM_ACCESS_RW_INTERLEAVED,
                                         2, 48000, 1, 200000);
                if (err >= 0)
                    break;
                snd_pcm_close(pcm);
                pcm = NULL;
            }
            free(device);
            device = NULL;
        }
        struct timespec retry = { .tv_sec = 0, .tv_nsec = 250000000L };
        nanosleep(&retry, NULL);
    }
    if (!pcm) {
        if (!audio_stop)
            fprintf(stderr, "startup audio disabled: no usable output after 15s\n");
        goto done;
    }
    fprintf(stderr, "startup audio: using %s\n", device);

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += data_size / 192000 + 3;

    const uint8_t *cursor = data;
    snd_pcm_uframes_t frames = data_size / 4;
    while (frames > 0 && !audio_stop && !audio_timed_out(&deadline)) {
        snd_pcm_sframes_t written = snd_pcm_writei(pcm, cursor, frames);
        if (written == -EAGAIN) {
            snd_pcm_wait(pcm, 100);
            continue;
        }
        if (written < 0) {
            int recovered = snd_pcm_recover(pcm, (int)written, 1);
            if (recovered >= 0)
                continue;
            fprintf(stderr, "startup audio disabled: %s\n", snd_strerror(recovered));
            break;
        }
        cursor += (size_t)written * 4;
        frames -= (snd_pcm_uframes_t)written;
    }

    if (frames == 0 && !audio_stop) {
        while ((err = snd_pcm_drain(pcm)) == -EAGAIN &&
               !audio_timed_out(&deadline))
            snd_pcm_wait(pcm, 100);
    }
    if (frames > 0 || err < 0 || audio_stop)
        snd_pcm_drop(pcm);

done:
    if (pcm)
        snd_pcm_close(pcm);
    free(device);
    if (file != MAP_FAILED)
        munmap(file, (size_t)st.st_size);
    if (fd >= 0)
        close(fd);
    return NULL;
}

static struct audio_playback playback;

void audio_start(const char *path, const char *configured_device)
{
    if (!path)
        return;
    playback.path = path;
    playback.configured_device = configured_device;
    audio_stop = 0;
    if (pthread_create(&playback.thread, NULL, play_wav, &playback) == 0) {
        playback.started = 1;
    } else {
        fprintf(stderr, "startup audio disabled: failed to create playback thread\n");
    }
}

void audio_stop_and_wait(void)
{
    audio_stop = 1;
    if (playback.started) {
        pthread_join(playback.thread, NULL);
        playback.started = 0;
    }
}
