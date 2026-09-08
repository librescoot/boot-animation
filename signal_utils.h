#ifndef BOOT_ANIMATION_SIGNAL_UTILS_H
#define BOOT_ANIMATION_SIGNAL_UTILS_H

#include <signal.h>

/*
 * Block termination signals around the transition into a fade.  If wait is
 * nonzero, atomically wait for the first signal.  Exactly one observed signal
 * is consumed; a second one remains visible to the fade loop and aborts it.
 */
int signal_prepare_fade(volatile sig_atomic_t *signal_count, int wait);

#endif
