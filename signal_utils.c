#define _POSIX_C_SOURCE 200809L

#include "signal_utils.h"

#include <errno.h>
#include <stddef.h>

int signal_prepare_fade(volatile sig_atomic_t *signal_count, int wait)
{
    sigset_t blocked;
    sigset_t previous;
    sigset_t wait_mask;

    if (!signal_count) {
        errno = EINVAL;
        return -1;
    }

    if (sigemptyset(&blocked) < 0 ||
        sigaddset(&blocked, SIGTERM) < 0 ||
        sigaddset(&blocked, SIGINT) < 0 ||
        sigprocmask(SIG_BLOCK, &blocked, &previous) < 0)
        return -1;

    wait_mask = previous;
    if (sigdelset(&wait_mask, SIGTERM) < 0 ||
        sigdelset(&wait_mask, SIGINT) < 0) {
        int saved_errno = errno;
        sigprocmask(SIG_SETMASK, &previous, NULL);
        errno = saved_errno;
        return -1;
    }

    while (wait && *signal_count == 0) {
        if (sigsuspend(&wait_mask) < 0 && errno != EINTR) {
            int saved_errno = errno;
            sigprocmask(SIG_SETMASK, &previous, NULL);
            errno = saved_errno;
            return -1;
        }
    }

    if (*signal_count > 0)
        --*signal_count;

    return sigprocmask(SIG_SETMASK, &previous, NULL);
}
