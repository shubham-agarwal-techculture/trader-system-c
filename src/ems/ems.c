#include "trading/ems.h"

#include "trading/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ems {
    venue_t *venue;
    oms_t *oms;
    rms_t *rms;
    ts_metrics *metrics;
    journal_t *journal;
    ts_clock clock;
    uint64_t next_venue_seq;
};

ems_t *ems_create(const ems_config *cfg)
{
    if (!cfg || !cfg->venue || !cfg->oms || !cfg->rms) {
        return NULL;
    }
    ems_t *e = calloc(1, sizeof(*e));
    if (!e) {
        return NULL;
    }
    e->venue = cfg->venue;
    e->oms = cfg->oms;
    e->rms = cfg->rms;
    e->metrics = cfg->metrics;
    e->journal = cfg->journal;
    e->clock = cfg->clock;
    e->next_venue_seq = 1;
    return e;
}

void ems_destroy(ems_t *e)
{
    free(e);
}

static ts_ts_ns now_ns(const ems_t *e)
{
    return e->clock.now_ns(e->clock.ctx);
}

/* Build the venue-facing order from the OMS order. The venue sees the same
 * normalized contract; adapters translate to wire protocols internally. */
static void to_venue_order(const ts_order *ord, ts_order *vord)
{
    *vord = *ord;
}

ts_status ems_submit(ems_t *e, const ts_order *ord)
{
    if (!e || !ord) {
        return TS_ERR_INVALID_ARG;
    }
    if (ord->state != TS_ORD_RISK_APPROVED &&
        ord->state != TS_ORD_NEW && ord->state != TS_ORD_HELD) {
        ts_log(TS_LOG_WARN, "ems: refusing order %s in state %s",
               ord->client_id, ts_order_state_str(ord->state));
        return TS_ERR_BAD_STATE;
    }

    ts_order vord;
    to_venue_order(ord, &vord);
    /* Ensure a venue order id exists for correlation even before ACK. */
    if (vord.venue_order_id[0] == '\0') {
        snprintf(vord.venue_order_id, sizeof(vord.venue_order_id),
                 "V%llu", (unsigned long long)++e->next_venue_seq);
    }

    ts_ts_ns t0 = now_ns(e);
    ts_status st = e->venue->ops->submit(e->venue, &vord);
    ts_metrics_latency(e->metrics, 2, "dispatch_ns", now_ns(e) - t0);
    if (st != TS_OK) {
        /* Transport-level failure: order state is uncertain. Never retry
         * blindly — mark for reconciliation. */
        ts_log(TS_LOG_ERROR, "ems: venue submit failed for %s: %s",
               ord->client_id, ts_status_str(st));
        oms_on_unknown(e->oms, ord->client_id, "venue submit transport error");
        return st;
    }
    return TS_OK;
}

ts_status ems_cancel(ems_t *e, const char *client_id)
{
    if (!e || !client_id) {
        return TS_ERR_INVALID_ARG;
    }
    return e->venue->ops->cancel(e->venue, client_id);
}

void ems_on_market_event(ems_t *e, const ts_mkt_event *ev)
{
    if (e && e->venue && e->venue->ops->on_market_event) {
        e->venue->ops->on_market_event(e->venue, ev);
    }
}

static void dispatch_event(ems_t *e, const ts_venue_event *ev)
{
    ts_ts_ns t0 = now_ns(e);
    switch (ev->type) {
    case TS_VEV_ACK:
        oms_on_ack(e->oms, ev->client_id, ev->venue_order_id);
        break;
    case TS_VEV_EXEC: {
        ts_execution exec = ev->exec;
        if (exec.client_id[0] == '\0') {
            ts_copy_str(exec.client_id, sizeof(exec.client_id), ev->client_id);
        }
        if (exec.venue_exec_id[0] == '\0') {
            ts_copy_str(exec.venue_exec_id, sizeof(exec.venue_exec_id),
                        "X");
        }
        oms_on_execution(e->oms, &exec);
        rms_on_execution(e->rms, &exec);
        ts_metrics_latency(e->metrics, 4, "fill_ns", now_ns(e) - t0);
        break;
    }
    case TS_VEV_REJECT:
        oms_on_reject(e->oms, ev->client_id, ev->reason);
        break;
    case TS_VEV_CANCEL_ACK:
        oms_on_cancel(e->oms, ev->client_id);
        break;
    case TS_VEV_EXPIRED:
        oms_on_expire(e->oms, ev->client_id, ev->reason);
        break;
    case TS_VEV_UNKNOWN:
        oms_on_unknown(e->oms, ev->client_id, ev->reason);
        break;
    }
}

ts_status ems_poll(ems_t *e)
{
    if (!e || !e->venue || !e->venue->ops->poll) {
        return TS_ERR_BAD_STATE;
    }
    ts_venue_event ev;
    ts_status st;
    size_t drained = 0;
    while ((st = e->venue->ops->poll(e->venue, &ev)) == TS_OK) {
        dispatch_event(e, &ev);
        drained++;
        if (drained > 100000) {
            ts_log(TS_LOG_ERROR, "ems: poll loop guard tripped");
            break;
        }
    }
    return st == TS_ERR_NOT_FOUND ? TS_OK : st;
}

bool ems_venue_connected(ems_t *e)
{
    return e && e->venue && e->venue->ops->connected &&
           e->venue->ops->connected(e->venue);
}

ts_status ems_mark_unknown(ems_t *e, const char *client_id, const char *reason)
{
    if (!e || !client_id) {
        return TS_ERR_INVALID_ARG;
    }
    return oms_on_unknown(e->oms, client_id, reason);
}
