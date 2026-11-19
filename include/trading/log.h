#ifndef TRADING_LOG_H
#define TRADING_LOG_H

#include "trading/core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TS_LOG_TRACE = 0,
    TS_LOG_DEBUG,
    TS_LOG_INFO,
    TS_LOG_WARN,
    TS_LOG_ERROR,
} ts_log_level;

void ts_log_set_level(ts_log_level lvl);
ts_log_level ts_log_get_level(void);
void ts_log(ts_log_level lvl, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* TRADING_LOG_H */
