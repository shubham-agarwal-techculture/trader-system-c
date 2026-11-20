#ifndef TRADING_CONFIG_H
#define TRADING_CONFIG_H

#include "trading/core.h"
#include "trading/rms.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal INI-style config loader.
 *
 *   [section]
 *   key = value      # comments with # or ;
 *
 * Sections/keys are case-sensitive. Values are strings; typed getters
 * parse them. Missing keys return the caller's default. */

typedef struct ts_config ts_config;

ts_config *ts_config_load(const char *path);
void ts_config_free(ts_config *c);

const char *ts_config_get(const ts_config *c, const char *section,
                          const char *key, const char *def);
double ts_config_get_double(const ts_config *c, const char *section,
                            const char *key, double def);
int64_t ts_config_get_i64(const ts_config *c, const char *section,
                          const char *key, int64_t def);
bool ts_config_get_bool(const ts_config *c, const char *section,
                        const char *key, bool def);

/* Fill a ts_risk_limits from the [risk] section, starting from
 * ts_risk_limits_default(). Returns TS_OK always (unknown keys ignored);
 * callers validate semantics. */
ts_status ts_config_apply_risk(const ts_config *c, ts_risk_limits *lim);

#ifdef __cplusplus
}
#endif

#endif /* TRADING_CONFIG_H */
