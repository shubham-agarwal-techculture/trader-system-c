#include "trading/config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <string.h>
#  define ts_strcasecmp _stricmp
#else
#  include <strings.h>
#  define ts_strcasecmp strcasecmp
#endif

#define MAX_ENTRIES 512
#define MAX_KEY 64
#define MAX_VAL 256
#define MAX_SECTION 64

typedef struct {
    char section[MAX_SECTION];
    char key[MAX_KEY];
    char value[MAX_VAL];
} config_entry;

struct ts_config {
    config_entry entries[MAX_ENTRIES];
    size_t count;
};

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

static void strip_comment(char *s)
{
    /* Remove '#' or ';' comments that are not inside quotes (we don't
     * support quoting in values anyway). */
    for (char *p = s; *p; ++p) {
        if (*p == '#' || *p == ';') {
            *p = '\0';
            break;
        }
    }
}

ts_config *ts_config_load(const char *path)
{
    if (!path) {
        return NULL;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return NULL;
    }
    ts_config *c = calloc(1, sizeof(*c));
    if (!c) {
        fclose(fp);
        return NULL;
    }

    char section[MAX_SECTION] = "";
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        strip_comment(line);
        char *s = trim(line);
        if (*s == '\0') {
            continue;
        }
        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close) {
                continue;
            }
            *close = '\0';
            snprintf(section, sizeof(section), "%s", trim(s + 1));
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);
        if (*key == '\0' || c->count >= MAX_ENTRIES) {
            continue;
        }
        config_entry *e = &c->entries[c->count++];
        snprintf(e->section, sizeof(e->section), "%s", section);
        snprintf(e->key, sizeof(e->key), "%s", key);
        snprintf(e->value, sizeof(e->value), "%s", val);
    }
    fclose(fp);
    return c;
}

void ts_config_free(ts_config *c)
{
    free(c);
}

const char *ts_config_get(const ts_config *c, const char *section,
                          const char *key, const char *def)
{
    if (!c) {
        return def;
    }
    for (size_t i = 0; i < c->count; ++i) {
        const config_entry *e = &c->entries[i];
        if (strcmp(e->section, section) == 0 && strcmp(e->key, key) == 0) {
            return e->value;
        }
    }
    return def;
}

static bool parse_bool_str(const char *s, bool def)
{
    if (!s) return def;
    if (ts_strcasecmp(s, "true") == 0 || ts_strcasecmp(s, "yes") == 0 ||
        strcmp(s, "1") == 0 || ts_strcasecmp(s, "on") == 0) {
        return true;
    }
    if (ts_strcasecmp(s, "false") == 0 || ts_strcasecmp(s, "no") == 0 ||
        strcmp(s, "0") == 0 || ts_strcasecmp(s, "off") == 0) {
        return false;
    }
    return def;
}

double ts_config_get_double(const ts_config *c, const char *section,
                            const char *key, double def)
{
    const char *v = ts_config_get(c, section, key, NULL);
    if (!v) {
        return def;
    }
    char *end = NULL;
    double d = strtod(v, &end);
    return (end && *end == '\0') ? d : def;
}

int64_t ts_config_get_i64(const ts_config *c, const char *section,
                          const char *key, int64_t def)
{
    const char *v = ts_config_get(c, section, key, NULL);
    if (!v) {
        return def;
    }
    char *end = NULL;
    long long d = strtoll(v, &end, 10);
    return (end && *end == '\0') ? (int64_t)d : def;
}

bool ts_config_get_bool(const ts_config *c, const char *section,
                        const char *key, bool def)
{
    const char *v = ts_config_get(c, section, key, NULL);
    return v ? parse_bool_str(v, def) : def;
}

ts_status ts_config_apply_risk(const ts_config *c, ts_risk_limits *lim)
{
    if (!c || !lim) {
        return TS_ERR_INVALID_ARG;
    }
    lim->max_order_qty =
        ts_config_get_double(c, "risk", "max_order_qty", lim->max_order_qty);
    lim->max_order_notional =
        ts_config_get_double(c, "risk", "max_order_notional",
                             lim->max_order_notional);
    lim->max_position_qty =
        ts_config_get_double(c, "risk", "max_position_qty",
                             lim->max_position_qty);
    lim->daily_loss_limit =
        ts_config_get_double(c, "risk", "daily_loss_limit",
                             lim->daily_loss_limit);
    lim->max_order_rate =
        ts_config_get_double(c, "risk", "max_order_rate",
                             lim->max_order_rate);
    lim->require_fresh_market =
        ts_config_get_bool(c, "risk", "require_fresh_market",
                           lim->require_fresh_market);
    lim->max_quote_age_ns =
        ts_config_get_i64(c, "risk", "max_quote_age_ns",
                         lim->max_quote_age_ns);
    return TS_OK;
}
