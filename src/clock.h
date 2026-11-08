#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>
#include <time.h>

static inline int64_t now_ns(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

static inline void sleep_until(int64_t ns)
{
    struct timespec t = {ns / 1000000000LL, ns % 1000000000LL};

    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
}

#endif
