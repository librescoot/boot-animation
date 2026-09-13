#ifndef BOOT_ANIMATION_AUDIO_PLAYBACK_H
#define BOOT_ANIMATION_AUDIO_PLAYBACK_H

void audio_start(const char *path, const char *configured_device);
void audio_stop_and_wait(void);

#endif
