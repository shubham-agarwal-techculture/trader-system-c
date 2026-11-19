#include "trading/core.h"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <time.h>
#endif

static ts_ts_ns mono_now_ns(void *ctx)
{
    (void)ctx;
#if defined(_WIN32)
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    /* freq is ticks/sec; convert to ns */
    return (ts_ts_ns)((count.QuadPart * 1000000000LL) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts_ts_ns)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

static ts_ts_ns vclock_now_ns(void *ctx)
{
    return ((ts_vclock *)ctx)->now;
}

ts_ts_ns ts_clock_mono_now(void)
{
    return mono_now_ns(NULL);
}

ts_clock ts_clock_mono(void)
{
    ts_clock c;
    c.now_ns = mono_now_ns;
    c.ctx = NULL;
    return c;
}

ts_clock ts_clock_virtual(ts_vclock *vc)
{
    ts_clock c;
    c.now_ns = vclock_now_ns;
    c.ctx = vc;
    return c;
}

void ts_vclock_set(ts_vclock *vc, ts_ts_ns ns)
{
    vc->now = ns;
}

void ts_vclock_advance(ts_vclock *vc, ts_ts_ns delta_ns)
{
    vc->now += delta_ns;
}
