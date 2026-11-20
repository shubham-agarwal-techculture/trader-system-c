#include "trading/oms.h"

#include "trading/log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OMS_EXEC_DEDUP_SIZE 4096

struct oms {
    ts_order *orders;
    size_t cap;
    size_t count;
    journal_t *journal;
    ts_metrics *metrics;
    ts_clock clock;
    /* Ring of recently seen venue_exec_ids for duplicate suppression.
     * seen_next is the next write position; the seen_count most recent
     * entries ending at seen_next are valid. */
    char seen_exec[OMS_EXEC_DEDUP_SIZE][TS_MAX_ID];
    size_t seen_next;
    size_t seen_count;
    /* Open-addressing index: client_id -> orders[] index+1 (0 = empty).
     * Power-of-two capacity >= 2x order capacity; orders are never
     * removed so no deletion is needed. */
    uint32_t *id_hash;
    /* Same scheme keyed on venue_order_id; entries added lazily when a
     * venue id is first assigned (ACK) or restored with one. */
    uint32_t *vid_hash;
    size_t hash_mask;
};

static uint32_t hash_client_id(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

static void id_hash_insert(oms_t *o, size_t idx)
{
    uint32_t h = hash_client_id(o->orders[idx].client_id) & (uint32_t)o->hash_mask;
    while (o->id_hash[h] != 0) {
        h = (h + 1) & (uint32_t)o->hash_mask;
    }
    o->id_hash[h] = (uint32_t)idx + 1;
}

static size_t hash_str_find(const oms_t *o, const uint32_t *tab,
                            const char *key)
{
    uint32_t h = hash_client_id(key) & (uint32_t)o->hash_mask;
    while (tab[h] != 0) {
        size_t idx = tab[h] - 1;
        const char *stored = idx < o->count
            ? (tab == o->id_hash ? o->orders[idx].client_id
                                 : o->orders[idx].venue_order_id)
            : "";
        if (strcmp(stored, key) == 0) {
            return idx;
        }
        h = (h + 1) & (uint32_t)o->hash_mask;
    }
    return SIZE_MAX;
}

static void vid_hash_insert(oms_t *o, size_t idx)
{
    const char *key = o->orders[idx].venue_order_id;
    if (!key[0]) {
        return;
    }
    /* avoid duplicates if already indexed (e.g. restore overwrite) */
    if (hash_str_find(o, o->vid_hash, key) != SIZE_MAX) {
        return;
    }
    uint32_t h = hash_client_id(key) & (uint32_t)o->hash_mask;
    while (o->vid_hash[h] != 0) {
        h = (h + 1) & (uint32_t)o->hash_mask;
    }
    o->vid_hash[h] = (uint32_t)idx + 1;
}

static size_t id_hash_find(const oms_t *o, const char *client_id)
{
    return hash_str_find(o, o->id_hash, client_id);
}

static bool exec_id_seen(oms_t *o, const char *exec_id)
{
    for (size_t k = 0; k < o->seen_count; ++k) {
        size_t idx = (o->seen_next + OMS_EXEC_DEDUP_SIZE - o->seen_count + k) %
                     OMS_EXEC_DEDUP_SIZE;
        if (strcmp(o->seen_exec[idx], exec_id) == 0) {
            return true;
        }
    }
    return false;
}

static void exec_id_remember(oms_t *o, const char *exec_id)
{
    ts_copy_str(o->seen_exec[o->seen_next], TS_MAX_ID, exec_id);
    o->seen_next = (o->seen_next + 1) % OMS_EXEC_DEDUP_SIZE;
    if (o->seen_count < OMS_EXEC_DEDUP_SIZE) {
        o->seen_count++;
    }
}

static ts_ts_ns now_ns(const oms_t *o)
{
    return o->clock.now_ns(o->clock.ctx);
}

oms_t *oms_create(const oms_config *cfg, size_t capacity)
{
    if (!cfg || capacity == 0) {
        return NULL;
    }
    oms_t *o = calloc(1, sizeof(*o));
    if (!o) {
        return NULL;
    }
    o->orders = calloc(capacity, sizeof(ts_order));
    if (!o->orders) {
        free(o);
        return NULL;
    }
    o->cap = capacity;
    o->journal = cfg->journal;
    o->metrics = cfg->metrics;
    o->clock = cfg->clock;
    /* hash capacity: pow2 >= 2*capacity, min 16 */
    size_t hc = 16;
    while (hc < capacity * 2) {
        hc *= 2;
    }
    o->id_hash = calloc(hc, sizeof(uint32_t));
    o->vid_hash = calloc(hc, sizeof(uint32_t));
    if (!o->id_hash || !o->vid_hash) {
        free(o->id_hash);
        free(o->vid_hash);
        free(o->orders);
        free(o);
        return NULL;
    }
    o->hash_mask = hc - 1;
    return o;
}

void oms_destroy(oms_t *o)
{
    if (!o) {
        return;
    }
    free(o->vid_hash);
    free(o->id_hash);
    free(o->orders);
    free(o);
}

static ts_order *find_by_client(oms_t *o, const char *client_id)
{
    size_t idx = id_hash_find(o, client_id);
    return idx == SIZE_MAX ? NULL : &o->orders[idx];
}

static ts_order *find_by_venue(oms_t *o, const char *venue_order_id)
{
    if (!venue_order_id || !venue_order_id[0]) {
        return NULL;
    }
    size_t idx = hash_str_find(o, o->vid_hash, venue_order_id);
    return idx == SIZE_MAX ? NULL : &o->orders[idx];
}

const ts_order *oms_find(const oms_t *o, const char *client_id)
{
    size_t idx = id_hash_find(o, client_id);
    return idx == SIZE_MAX ? NULL : &o->orders[idx];
}

const ts_order *oms_find_venue(const oms_t *o, const char *venue_order_id)
{
    if (!venue_order_id || !venue_order_id[0]) {
        return NULL;
    }
    size_t idx = hash_str_find(o, o->vid_hash, venue_order_id);
    return idx == SIZE_MAX ? NULL : &o->orders[idx];
}

const ts_order *oms_at(const oms_t *o, size_t idx)
{
    return idx < o->count ? &o->orders[idx] : NULL;
}

size_t oms_count(const oms_t *o)
{
    return o->count;
}

size_t oms_open_count(const oms_t *o)
{
    size_t n = 0;
    for (size_t i = 0; i < o->count; ++i) {
        if (!ts_order_state_is_terminal(o->orders[i].state)) {
            n++;
        }
    }
    return n;
}

static void persist_snap(oms_t *o, const ts_order *ord)
{
    if (o->journal) {
        journal_append_snap(o->journal, ord);
    }
}

/* Transition legality table. Terminal states accept no further transitions
 * except UNKNOWN -> reconciliation resolution handled explicitly. */
static bool transition_allowed(ts_order_state from, ts_order_state to)
{
    if (from == to) {
        return true; /* idempotent re-report */
    }
    if (ts_order_state_is_terminal(from)) {
        /* allow UNKNOWN recovery out of terminal-looking states only via
         * explicit oms_on_* calls; keep it strict here */
        return from == TS_ORD_UNKNOWN &&
               (to == TS_ORD_WORKING || to == TS_ORD_FILLED ||
                to == TS_ORD_CANCELED || to == TS_ORD_REJECTED ||
                to == TS_ORD_EXPIRED || to == TS_ORD_PARTIAL);
    }
    switch (from) {
    case TS_ORD_NEW:
        return to == TS_ORD_HELD || to == TS_ORD_RISK_APPROVED ||
               to == TS_ORD_REJECTED || to == TS_ORD_CANCELED ||
               to == TS_ORD_UNKNOWN;
    case TS_ORD_HELD:
        return to == TS_ORD_RISK_APPROVED || to == TS_ORD_REJECTED ||
               to == TS_ORD_CANCELED || to == TS_ORD_UNKNOWN;
    case TS_ORD_RISK_APPROVED:
        return to == TS_ORD_WORKING || to == TS_ORD_PARTIAL ||
               to == TS_ORD_FILLED || to == TS_ORD_REJECTED ||
               to == TS_ORD_CANCELED || to == TS_ORD_EXPIRED ||
               to == TS_ORD_UNKNOWN;
    case TS_ORD_WORKING:
    case TS_ORD_PARTIAL:
        return to == TS_ORD_WORKING || to == TS_ORD_PARTIAL ||
               to == TS_ORD_FILLED || to == TS_ORD_CANCELED ||
               to == TS_ORD_REJECTED || to == TS_ORD_EXPIRED ||
               to == TS_ORD_UNKNOWN;
    case TS_ORD_UNKNOWN:
        return true; /* reconciliation may move it anywhere */
    default:
        return false;
    }
}

static ts_status set_state(oms_t *o, ts_order *ord, ts_order_state to,
                           const char *reason)
{
    if (!transition_allowed(ord->state, to)) {
        ts_log(TS_LOG_WARN, "oms: illegal transition %s -> %s for %s",
               ts_order_state_str(ord->state), ts_order_state_str(to),
               ord->client_id);
        ts_metrics_inc(o->metrics, TS_MET_RECON_BREAKS, 1);
        return TS_ERR_BAD_STATE;
    }
    ord->state = to;
    ord->updated_ns = now_ns(o);
    if (reason) {
        ts_copy_str(ord->reject_reason, sizeof(ord->reject_reason), reason);
    }
    persist_snap(o, ord);
    return TS_OK;
}

ts_status oms_submit(oms_t *o, const ts_order_request *req, ts_order *out)
{
    if (!o || !req) {
        return TS_ERR_INVALID_ARG;
    }
    ts_status vs = ts_order_request_validate(req);
    if (vs != TS_OK) {
        return vs;
    }

    ts_order *existing = find_by_client(o, req->client_id);
    if (existing) {
        if (out) {
            *out = *existing;
        }
        ts_metrics_inc(o->metrics, TS_MET_ORDERS_DUPLICATE, 1);
        return TS_OK;
    }
    if (o->count >= o->cap) {
        return TS_ERR_FULL;
    }

    ts_order *ord = &o->orders[o->count++];
    memset(ord, 0, sizeof(*ord));
    ts_copy_str(ord->client_id, sizeof(ord->client_id), req->client_id);
    ts_copy_str(ord->instrument, sizeof(ord->instrument), req->instrument);
    ts_copy_str(ord->strategy, sizeof(ord->strategy), req->strategy);
    ord->side = req->side;
    ord->type = req->type;
    ord->tif = req->tif;
    ord->qty = req->qty;
    ord->price = req->price;
    ord->state = TS_ORD_NEW;
    ord->created_ns = now_ns(o);
    ord->updated_ns = ord->created_ns;
    id_hash_insert(o, o->count - 1);

    persist_snap(o, ord);
    ts_metrics_inc(o->metrics, TS_MET_ORDERS_SUBMITTED, 1);
    if (out) {
        *out = *ord;
    }
    return TS_OK;
}

ts_status oms_mark_risk(oms_t *o, const char *client_id, bool approved,
                        const char *rule, const char *msg)
{
    ts_order *ord = find_by_client(o, client_id);
    if (!ord) {
        return TS_ERR_NOT_FOUND;
    }
    if (o->journal) {
        journal_append_risk(o->journal, now_ns(o), client_id, approved,
                            rule ? rule : "", msg ? msg : "");
    }
    ts_order_state to = approved ? TS_ORD_RISK_APPROVED : TS_ORD_REJECTED;
    ts_status st = set_state(o, ord, to, approved ? NULL : (rule ? rule : "risk"));
    if (approved) {
        ts_metrics_inc(o->metrics, TS_MET_RISK_APPROVED, 1);
    } else {
        ts_metrics_inc(o->metrics, TS_MET_RISK_REJECTED, 1);
    }
    return st;
}

ts_status oms_on_ack(oms_t *o, const char *client_id,
                     const char *venue_order_id)
{
    ts_order *ord = find_by_client(o, client_id);
    if (!ord && venue_order_id) {
        ord = find_by_venue(o, venue_order_id);
    }
    if (!ord) {
        return TS_ERR_NOT_FOUND;
    }
    if (venue_order_id && venue_order_id[0]) {
        bool had_id = ord->venue_order_id[0] != '\0';
        ts_copy_str(ord->venue_order_id, sizeof(ord->venue_order_id),
                    venue_order_id);
        if (!had_id) {
            vid_hash_insert(o, (size_t)(ord - o->orders));
        }
    }
    ts_metrics_inc(o->metrics, TS_MET_VENUE_ACKS, 1);
    return set_state(o, ord, TS_ORD_WORKING, NULL);
}

ts_status oms_on_execution(oms_t *o, const ts_execution *exec)
{
    if (!o || !exec) {
        return TS_ERR_INVALID_ARG;
    }
    /* Duplicate execution suppression by venue exec id. Executions
     * without a venue id (e.g. tests) are not deduplicated. */
    if (exec->venue_exec_id[0] != '\0') {
        if (exec_id_seen(o, exec->venue_exec_id)) {
            ts_metrics_inc(o->metrics, TS_MET_EXEC_DUPLICATE, 1);
            return TS_OK;
        }
        exec_id_remember(o, exec->venue_exec_id);
    }

    ts_order *ord = find_by_client(o, exec->client_id);
    if (!ord && exec->venue_order_id[0]) {
        ord = find_by_venue(o, exec->venue_order_id);
    }
    if (!ord) {
        return TS_ERR_NOT_FOUND;
    }

    /* Update cumulative fill state. */
    double new_filled = ord->filled_qty + exec->qty;
    if (new_filled > ord->qty + 1e-9) {
        ts_log(TS_LOG_WARN, "oms: overfill on %s (%.6g + %.6g > %.6g)",
               ord->client_id, ord->filled_qty, exec->qty, ord->qty);
        ts_metrics_inc(o->metrics, TS_MET_RECON_BREAKS, 1);
    }
    if (new_filled > ord->qty) {
        new_filled = ord->qty;
    }
    if (ord->filled_qty > 0.0) {
        ord->avg_fill_price =
            (ord->avg_fill_price * ord->filled_qty + exec->price * exec->qty) /
            new_filled;
    } else {
        ord->avg_fill_price = exec->price;
    }
    ord->filled_qty = new_filled;

    ts_order_state to = new_filled >= ord->qty - 1e-9
                            ? TS_ORD_FILLED
                            : TS_ORD_PARTIAL;
    ord->updated_ns = now_ns(o);
    persist_snap(o, ord);

    if (o->journal) {
        journal_append_exec(o->journal, exec);
    }
    ts_metrics_inc(o->metrics, TS_MET_EXECUTIONS, 1);
    return set_state(o, ord, to, NULL);
}

ts_status oms_on_reject(oms_t *o, const char *client_id, const char *reason)
{
    ts_order *ord = find_by_client(o, client_id);
    if (!ord) {
        return TS_ERR_NOT_FOUND;
    }
    ts_metrics_inc(o->metrics, TS_MET_VENUE_REJECTS, 1);
    return set_state(o, ord, TS_ORD_REJECTED, reason);
}

ts_status oms_on_cancel(oms_t *o, const char *client_id)
{
    ts_order *ord = find_by_client(o, client_id);
    if (!ord) {
        return TS_ERR_NOT_FOUND;
    }
    ts_metrics_inc(o->metrics, TS_MET_CANCELS, 1);
    return set_state(o, ord, TS_ORD_CANCELED, NULL);
}

ts_status oms_on_amend(oms_t *o, const char *client_id, double new_price,
                       double new_qty)
{
    ts_order *ord = find_by_client(o, client_id);
    if (!ord) {
        return TS_ERR_NOT_FOUND;
    }
    if (ts_order_state_is_terminal(ord->state) ||
        ord->state == TS_ORD_UNKNOWN) {
        return TS_ERR_BAD_STATE;
    }
    if (new_price > 0.0) {
        ord->price = new_price;
    }
    if (new_qty > 0.0) {
        if (new_qty < ord->filled_qty) {
            return TS_ERR_INVALID_ARG; /* can't amend below filled qty */
        }
        ord->qty = new_qty;
    }
    ord->updated_ns = now_ns(o);
    persist_snap(o, ord);
    return TS_OK;
}

ts_status oms_on_expire(oms_t *o, const char *client_id, const char *reason)
{
    ts_order *ord = find_by_client(o, client_id);
    if (!ord) {
        return TS_ERR_NOT_FOUND;
    }
    return set_state(o, ord, TS_ORD_EXPIRED, reason);
}

ts_status oms_on_unknown(oms_t *o, const char *client_id, const char *reason)
{
    ts_order *ord = find_by_client(o, client_id);
    if (!ord) {
        return TS_ERR_NOT_FOUND;
    }
    ts_metrics_inc(o->metrics, TS_MET_RECON_BREAKS, 1);
    return set_state(o, ord, TS_ORD_UNKNOWN, reason);
}

ts_status oms_restore_order(oms_t *o, const ts_order *snapshot)
{
    if (!o || !snapshot || snapshot->client_id[0] == '\0') {
        return TS_ERR_INVALID_ARG;
    }
    ts_order *existing = find_by_client(o, snapshot->client_id);
    if (existing) {
        /* Snapshots are replayed in journal order; later wins. */
        *existing = *snapshot;
        if (snapshot->venue_order_id[0]) {
            vid_hash_insert(o, (size_t)(existing - o->orders));
        }
        return TS_OK;
    }
    if (o->count >= o->cap) {
        return TS_ERR_FULL;
    }
    o->orders[o->count] = *snapshot;
    id_hash_insert(o, o->count);
    if (snapshot->venue_order_id[0]) {
        vid_hash_insert(o, o->count);
    }
    o->count++;
    return TS_OK;
}

bool oms_would_self_trade(const oms_t *o, const ts_order_request *req)
{
    if (!o || !req) {
        return false;
    }
    for (size_t i = 0; i < o->count; ++i) {
        const ts_order *w = &o->orders[i];
        if (strcmp(w->instrument, req->instrument) != 0 ||
            w->side == req->side ||
            ts_order_state_is_terminal(w->state)) {
            continue;
        }
        /* opposite side, still open */
        if (req->type == TS_ORD_MARKET || w->type == TS_ORD_MARKET) {
            return true; /* can't prove non-crossing; be conservative */
        }
        if (req->side == TS_SIDE_BUY && req->price >= w->price) {
            return true;
        }
        if (req->side == TS_SIDE_SELL && req->price <= w->price) {
            return true;
        }
    }
    return false;
}

size_t oms_orders_in_state(const oms_t *o, ts_order_state state,
                           ts_order *out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < o->count && n < cap; ++i) {
        if (o->orders[i].state == state) {
            out[n++] = o->orders[i];
        }
    }
    return n;
}
