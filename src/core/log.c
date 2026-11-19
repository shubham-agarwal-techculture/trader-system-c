#include "trading/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static ts_log_level g_level = TS_LOG_INFO;

static const char *level_name(ts_log_level lvl)
{
    switch (lvl) {
    case TS_LOG_TRACE: return "TRACE";
    case TS_LOG_DEBUG: return "DEBUG";
    case TS_LOG_INFO:  return "INFO";
    case TS_LOG_WARN:  return "WARN";
    case TS_LOG_ERROR: return "ERROR";
    }
    return "?";
}

void ts_log_set_level(ts_log_level lvl)
{
    g_level = lvl;
}

ts_log_level ts_log_get_level(void)
{
    return g_level;
}

void ts_log(ts_log_level lvl, const char *fmt, ...)
{
    if (lvl < g_level) {
        return;
    }
    time_t now = time(NULL);
    struct tm tmv;
    char tsbuf[32];
#if defined(_WIN32)
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", &tmv);

    fprintf(stderr, "[%s] %-5s ", tsbuf, level_name(lvl));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
