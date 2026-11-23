/*
 * libc bits quickjs-libc.c links against but ESP-IDF doesn't ship.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <sys/stat.h>
#include <sys/time.h>

/* weak: if IDF or the app ever grows its own, theirs wins */
#define GLUE __attribute__((weak))

/* os.sleep(). usleep() in IDF already does vTaskDelay() for anything >= 1 tick
 * and a busy-wait below that, so just feed it. Never interrupted -> rem = 0. */
GLUE int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (!req || req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    uint64_t us = (uint64_t)req->tv_sec * 1000000u + ((uint64_t)req->tv_nsec + 999) / 1000;
    while (us) {
        uint32_t chunk = us > 1000000u ? 1000000u : (uint32_t)us;  /* usleep() wants < 1 s */
        usleep(chunk);
        us -= chunk;
    }
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

/* VFS only has utime(), which is seconds anyway. */
GLUE int utimes(const char *path, const struct timeval tv[2])
{
    if (!tv) {
        return utime(path, NULL);
    }
    struct utimbuf ub = { .actime = tv[0].tv_sec, .modtime = tv[1].tv_sec };
    return utime(path, &ub);
}

/* No symlinks in any IDF filesystem, so lstat == stat. */
GLUE int lstat(const char *path, struct stat *st)
{
    return stat(path, st);
}
