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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* weak: if IDF or the app ever grows its own, theirs wins */
#define GLUE __attribute__((weak))

/*
 * os.sleep(). Always blocks in vTaskDelay(), never spins: IDF's usleep()
 * busy-waits anything under a tick, which keeps the CPU out of light sleep.
 * Rounded up to whole ticks, +1 because the current tick is partly gone
 * (POSIX allows sleeping longer). Never interrupted -> rem = 0.
 */
GLUE int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (!req || req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    const uint64_t tick_ns = 1000000000ull / configTICK_RATE_HZ;
    uint64_t sec = req->tv_sec > 1000000000 ? 1000000000 : req->tv_sec;   /* 31 years, no overflow */
    uint64_t ns = sec * 1000000000ull + req->tv_nsec;
    uint64_t ticks = ns ? (ns + tick_ns - 1) / tick_ns + 1 : 0;
    while (ticks) {
        TickType_t t = ticks > 0x7fffffff ? 0x7fffffff : (TickType_t)ticks;
        vTaskDelay(t);
        ticks -= t;
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
