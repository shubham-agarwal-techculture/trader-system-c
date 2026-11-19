#include "trading/core.h"

#include <string.h>

const char *ts_status_str(ts_status s)
{
    switch (s) {
    case TS_OK: return "OK";
    case TS_ERR_INVALID_ARG: return "INVALID_ARG";
    case TS_ERR_NOT_FOUND: return "NOT_FOUND";
    case TS_ERR_BAD_STATE: return "BAD_STATE";
    case TS_ERR_FULL: return "FULL";
    case TS_ERR_IO: return "IO";
    case TS_ERR_PARSE: return "PARSE";
    case TS_ERR_NOMEM: return "NOMEM";
    }
    return "UNKNOWN";
}
