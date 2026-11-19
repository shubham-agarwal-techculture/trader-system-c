#ifndef TRADING_CORE_H
#define TRADING_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Nanosecond timestamp. Monotonic unless noted otherwise. */
typedef int64_t ts_ts_ns;

typedef enum {
    TS_OK = 0,
    TS_ERR_INVALID_ARG = -1,
    TS_ERR_NOT_FOUND = -2,
    TS_ERR_BAD_STATE = -3,
    TS_ERR_FULL = -4,
    TS_ERR_IO = -5,
    TS_ERR_PARSE = -6,
    TS_ERR_NOMEM = -7,
} ts_status;

const char *ts_status_str(ts_status s);

/* Injectable clock. now_ns returns nanoseconds from an arbitrary epoch;
 * only differences are meaningful unless the clock is wall-clock based. */
typedef struct ts_clock {
    ts_ts_ns (*now_ns)(void *ctx);
    void *ctx;
} ts_clock;

/* Real monotonic clock (QueryPerformanceCounter / clock_gettime). */
ts_ts_ns ts_clock_mono_now(void);
ts_clock ts_clock_mono(void);

/* Virtual clock for deterministic tests and replay. */
typedef struct ts_vclock {
    ts_ts_ns now;
} ts_vclock;

ts_clock ts_clock_virtual(ts_vclock *vc);
void ts_vclock_set(ts_vclock *vc, ts_ts_ns ns);
void ts_vclock_advance(ts_vclock *vc, ts_ts_ns delta_ns);

#ifdef __cplusplus
}
#endif

#endif /* TRADING_CORE_H */
