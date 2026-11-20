#include "trading/venue.h"
#include "trading/kraken_transport.h"

#include "trading/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Kraken spot adapter — REST mode (no WebSocket yet).
 *
 * Order entry:   POST /0/private/AddOrder|CancelOrder|CancelAllOrders
 * Reconciliation: GET /0/private/OpenOrders
 * Executions:    GET /0/private/TradesHistory (diffed by trade id)
 * Market data:   GET /0/public/Ticker via venue_kraken_get_ticker()
 *                (wired into the live feed loop separately)
 *
 * Auth: API-Key header + API-Sign = base64(HMAC-SHA256(secret,
 * urlpath + hex(sha256(nonce+postdata)))). Credentials are supplied at
 * create time by the app from a secrets source — never from config
 * files or the journal.
 *
 * Kraken has NO amend endpoint: venue_ops.amend stays NULL and EMS
 * returns TS_ERR_BAD_STATE; use cancel + resubmit instead.
 *
 * Instrument mapping: "BTCUSDT" -> "XBTUSD"; all other codes pass
 * through unchanged (configure pair names to match venue listing).
 */

#define KRAKEN_MAX_ORDERS 256
#define KRAKEN_MAX_EVENTS 1024
#define KRAKEN_MAX_TRADES_SEEN 512
#define KRAKEN_POLL_INTERVAL_NS 250000000LL /* 250ms */

typedef struct {
    char client_id[TS_MAX_ID];
    char txid[TS_MAX_ID];       /* Kraken order txid */
    char instrument[TS_MAX_CODE];
    ts_side side;
    double qty;
    double price;
    double filled_qty;
    bool terminal;
} kraken_order;

typedef struct {
    ts_venue_event ev;
} kraken_slot;

struct kraken_impl {
    char api_key[256];
    char api_secret[256];
    bool has_creds;
    bool connected;
    bool last_http_ok;
    kraken_order orders[KRAKEN_MAX_ORDERS];
    size_t order_count;
    kraken_slot events[KRAKEN_MAX_EVENTS];
    size_t ev_head, ev_tail, ev_count;
    /* seen trade ids for TradesHistory diffing */
    char seen_trades[KRAKEN_MAX_TRADES_SEEN][TS_MAX_ID];
    size_t seen_count, seen_next;
    ts_ts_ns last_poll_ns;
    ts_ts_ns last_ticker_ns;
    ts_metrics *metrics;
    ts_clock clock;
};

static ts_ts_ns now_ns(struct kraken_impl *k)
{
    return k->clock.now_ns(k->clock.ctx);
}

/* ---------- pair mapping ---------- */

static void kraken_pair(const char *instrument, char *out, size_t cap)
{
    if (strcmp(instrument, "BTCUSDT") == 0) {
        snprintf(out, cap, "XBTUSD");
    } else if (strcmp(instrument, "ETHUSDT") == 0) {
        snprintf(out, cap, "ETHUSD");
    } else {
        snprintf(out, cap, "%s", instrument);
    }
}

static void internal_pair(const char *pair, char *out, size_t cap)
{
    if (strcmp(pair, "XBTUSD") == 0) {
        snprintf(out, cap, "BTCUSDT");
    } else if (strcmp(pair, "ETHUSD") == 0) {
        snprintf(out, cap, "ETHUSDT");
    } else {
        snprintf(out, cap, "%s", pair);
    }
}

/* ---------- minimal JSON extraction ---------- */

static const char *json_find(const char *obj_start, const char *obj_end,
                             const char *key)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = obj_start;
    size_t plen = strlen(pat);
    while (p && p < obj_end) {
        p = strstr(p, pat);
        if (!p || p >= obj_end) return NULL;
        const char *colon = strchr(p + plen, ':');
        if (!colon || colon >= obj_end) return NULL;
        return colon + 1;
    }
    return NULL;
}

static bool json_str(const char *start, const char *end, const char *key,
                     char *out, size_t cap)
{
    const char *v = json_find(start, end, key);
    if (!v) return false;
    while (*v == ' ' || *v == '\t') v++;
    if (*v != '"') return false;
    v++;
    size_t n = 0;
    while (*v && *v != '"' && v < end && n + 1 < cap) {
        out[n++] = *v++;
    }
    out[n] = '\0';
    return true;
}

static bool json_num(const char *start, const char *end, const char *key,
                     double *out)
{
    const char *v = json_find(start, end, key);
    if (!v) return false;
    while (*v == ' ' || *v == '\t') v++;
    char *ep = NULL;
    double d = strtod(v, &ep);
    if (ep == v) return false;
    *out = d;
    return true;
}

/* ---------- signed request ---------- */

static char *signed_post(struct kraken_impl *k, const char *path,
                         const char *post_data, int *status)
{
    if (!k->has_creds) return NULL;
    char nonce[32];
    snprintf(nonce, sizeof(nonce), "%lld",
             (long long)(now_ns(k) / 1000000LL));

    /* post_data must already contain nonce=...&... ; we build it here */
    char *full_post = malloc(strlen(post_data) + strlen(nonce) + 32);
    if (!full_post) return NULL;
    snprintf(full_post, strlen(post_data) + strlen(nonce) + 32,
             "nonce=%s&%s", nonce, post_data);

    char *sign = kraken_api_sign(path, nonce, full_post, k->api_secret);
    if (!sign) { free(full_post); return NULL; }

    char headers[1024];
    snprintf(headers, sizeof(headers),
             "API-Key: %s\r\nAPI-Sign: %s\r\n", k->api_key, sign);
    free(sign);

    char *body = kraken_http_post(path, full_post, headers, status);
    free(full_post);
    k->last_http_ok = (body != NULL && *status >= 200 && *status < 300);
    k->connected = k->last_http_ok;
    return body;
}

static char *public_get(struct kraken_impl *k, const char *path_query,
                        int *status)
{
    char *body = kraken_http_get(path_query, status);
    k->last_http_ok = (body != NULL && *status >= 200 && *status < 300);
    k->connected = k->last_http_ok || k->has_creds; /* public may fail independently */
    return body;
}

/* ---------- internal event queue ---------- */

static bool push_event(struct kraken_impl *k, const ts_venue_event *ev)
{
    if (k->ev_count >= KRAKEN_MAX_EVENTS) {
        ts_metrics_inc(k->metrics, TS_MET_RECON_BREAKS, 1);
        return false;
    }
    k->events[k->ev_tail].ev = *ev;
    k->ev_tail = (k->ev_tail + 1) % KRAKEN_MAX_EVENTS;
    k->ev_count++;
    return true;
}

static void make_ev(struct kraken_impl *k, ts_venue_event *ev,
                    ts_venue_event_type type, const char *client_id,
                    const char *instr)
{
    memset(ev, 0, sizeof(*ev));
    ev->type = type;
    ev->ts_ns = now_ns(k);
    if (client_id) ts_copy_str(ev->client_id, sizeof(ev->client_id), client_id);
    if (instr) ts_copy_str(ev->instrument, sizeof(ev->instrument), instr);
}

/* ---------- order table ---------- */

static kraken_order *find_order(struct kraken_impl *k, const char *client_id)
{
    for (size_t i = 0; i < k->order_count; ++i) {
        if (strcmp(k->orders[i].client_id, client_id) == 0) {
            return &k->orders[i];
        }
    }
    return NULL;
}

static kraken_order *find_by_txid(struct kraken_impl *k, const char *txid)
{
    for (size_t i = 0; i < k->order_count; ++i) {
        if (strcmp(k->orders[i].txid, txid) == 0) {
            return &k->orders[i];
        }
    }
    return NULL;
}

/* ---------- error array check ---------- */

static bool kraken_error_empty(const char *body)
{
    const char *e = strstr(body, "\"error\"");
    if (!e) return false;
    const char *b = strchr(e, '[');
    if (!b) return false;
    const char *end = strchr(b, ']');
    if (!end) return false;
    /* empty array => only whitespace between [ and ] */
    for (const char *p = b + 1; p < end; ++p) {
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
            return false;
        }
    }
    return true;
}

static void kraken_error_text(const char *body, char *out, size_t cap)
{
    const char *e = strstr(body, "\"error\"");
    if (!e) { snprintf(out, cap, "unknown kraken error"); return; }
    const char *b = strchr(e, '[');
    const char *end = b ? strchr(b, ']') : NULL;
    if (!b || !end || end - b > (ptrdiff_t)cap - 1) {
        snprintf(out, cap, "kraken error");
        return;
    }
    size_t n = (size_t)(end - b);
    memcpy(out, b, n);
    out[n] = '\0';
}

/* ---------- venue_ops ---------- */

static ts_status kraken_submit(venue_t *v, const ts_order *ord)
{
    struct kraken_impl *k = v->impl;
    if (!k->connected && !k->has_creds) return TS_ERR_BAD_STATE;
    if (!k->has_creds) return TS_ERR_BAD_STATE;
    if (find_order(k, ord->client_id)) return TS_OK; /* idempotent */

    char pair[64];
    kraken_pair(ord->instrument, pair, sizeof(pair));

    char post[1024];
    const char *otype = ord->type == TS_ORD_MARKET ? "market" : "limit";
    const char *side = ord->side == TS_SIDE_BUY ? "buy" : "sell";
    if (ord->type == TS_ORD_MARKET) {
        snprintf(post, sizeof(post),
                 "pair=%s&type=%s&ordertype=%s&volume=%.8f",
                 pair, side, otype, ord->qty);
    } else {
        snprintf(post, sizeof(post),
                 "pair=%s&type=%s&ordertype=%s&price=%.8f&volume=%.8f",
                 pair, side, otype, ord->price, ord->qty);
    }

    int status = 0;
    char *body = signed_post(k, "/0/private/AddOrder", post, &status);
    if (!body) {
        /* transport failure: uncertain outcome -> UNKNOWN, never retry */
        ts_venue_event ev;
        make_ev(k, &ev, TS_VEV_UNKNOWN, ord->client_id, ord->instrument);
        ts_copy_str(ev.reason, sizeof(ev.reason),
                    "addorder transport failure");
        push_event(k, &ev);
        return TS_OK;
    }
    ts_status ret = TS_OK;
    if (!kraken_error_empty(body)) {
        ts_venue_event ev;
        make_ev(k, &ev, TS_VEV_REJECT, ord->client_id, ord->instrument);
        kraken_error_text(body, ev.reason, sizeof(ev.reason));
        push_event(k, &ev);
        free(body);
        return ret;
    }
    /* extract txid */
    const char *r = strstr(body, "\"result\"");
    const char *end = body + strlen(body);
    char txid[TS_MAX_ID] = "";
    if (r && json_str(r, end, "txid", txid, sizeof(txid))) {
        /* txid may be array ["..."] — json_str stops at first quote char;
         * extract first quoted token inside brackets instead */
        const char *lb = strchr(r, '[');
        const char *q1 = lb ? strchr(lb, '"') : NULL;
        const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        if (q1 && q2 && q2 > q1 + 1) {
            size_t n = (size_t)(q2 - q1 - 1);
            if (n >= sizeof(txid)) n = sizeof(txid) - 1;
            memcpy(txid, q1 + 1, n);
            txid[n] = '\0';
        }
    }
    free(body);

    if (k->order_count < KRAKEN_MAX_ORDERS) {
        kraken_order *ko = &k->orders[k->order_count++];
        memset(ko, 0, sizeof(*ko));
        ts_copy_str(ko->client_id, sizeof(ko->client_id), ord->client_id);
        ts_copy_str(ko->txid, sizeof(ko->txid), txid);
        ts_copy_str(ko->instrument, sizeof(ko->instrument), ord->instrument);
        ko->side = ord->side;
        ko->qty = ord->qty;
        ko->price = ord->price;
    }

    ts_venue_event ev;
    make_ev(k, &ev, TS_VEV_ACK, ord->client_id, ord->instrument);
    ts_copy_str(ev.venue_order_id, sizeof(ev.venue_order_id), txid);
    push_event(k, &ev);
    return TS_OK;
}

static ts_status kraken_cancel(venue_t *v, const char *client_id)
{
    struct kraken_impl *k = v->impl;
    kraken_order *ko = find_order(k, client_id);
    if (!ko) return TS_ERR_NOT_FOUND;
    if (ko->terminal) return TS_OK;

    char post[256];
    snprintf(post, sizeof(post), "txid=%s", ko->txid);
    int status = 0;
    char *body = signed_post(k, "/0/private/CancelOrder", post, &status);
    if (!body) {
        ts_venue_event ev;
        make_ev(k, &ev, TS_VEV_UNKNOWN, client_id, ko->instrument);
        ts_copy_str(ev.reason, sizeof(ev.reason),
                    "cancelorder transport failure");
        push_event(k, &ev);
        return TS_OK;
    }
    if (!kraken_error_empty(body)) {
        char reason[128];
        kraken_error_text(body, reason, sizeof(reason));
        /* EOrder:Unknown order is benign -> treat as already gone */
        if (strstr(reason, "Unknown order") == NULL) {
            ts_venue_event ev;
            make_ev(k, &ev, TS_VEV_REJECT, client_id, ko->instrument);
            ts_copy_str(ev.reason, sizeof(ev.reason), reason);
            push_event(k, &ev);
        } else {
            ts_venue_event ev;
            make_ev(k, &ev, TS_VEV_CANCEL_ACK, client_id, ko->instrument);
            push_event(k, &ev);
            ko->terminal = true;
        }
        free(body);
        return TS_OK;
    }
    free(body);
    ts_venue_event ev;
    make_ev(k, &ev, TS_VEV_CANCEL_ACK, client_id, ko->instrument);
    push_event(k, &ev);
    ko->terminal = true;
    return TS_OK;
}

static ts_status kraken_cancel_all(venue_t *v)
{
    struct kraken_impl *k = v->impl;
    int status = 0;
    char *body = signed_post(k, "/0/private/CancelAllOrders", "", &status);
    if (!body) return TS_ERR_BAD_STATE;
    int count = 0;
    const char *r = strstr(body, "\"result\"");
    const char *c = r ? json_find(r, body + strlen(body), "count") : NULL;
    if (c) count = (int)strtol(c, NULL, 10);
    free(body);
    /* Per-order CANCEL_ACKs arrive via OpenOrders diff in poll; mark all
     * local orders pending-cancel and let reconciliation resolve. */
    for (size_t i = 0; i < k->order_count; ++i) {
        if (!k->orders[i].terminal) {
            ts_venue_event ev;
            make_ev(k, &ev, TS_VEV_CANCEL_ACK, k->orders[i].client_id,
                    k->orders[i].instrument);
            push_event(k, &ev);
            k->orders[i].terminal = true;
        }
    }
    (void)count;
    return TS_OK;
}

static void kraken_on_market_event(venue_t *v, const ts_mkt_event *ev)
{
    (void)v; (void)ev; /* Kraken adapter is a market-data SOURCE; inbound
                          normalized events are not used. */
}

/* Signed GET for private endpoints. api_path is the Kraken path WITHOUT
 * the "/0" prefix used by kraken_http_get (e.g. "/private/TradesHistory");
 * signing uses "/0" + api_path. Optional query params may be appended. */
static char *signed_get(struct kraken_impl *k, const char *api_path,
                        const char *query)
{
    if (!k->has_creds) return NULL;
    char sign_path[300];
    snprintf(sign_path, sizeof(sign_path), "/0%s", api_path);
    char nonce[32];
    snprintf(nonce, sizeof(nonce), "%lld",
             (long long)(now_ns(k) / 1000000LL));
    char *sign = kraken_api_sign(sign_path, nonce, "", k->api_secret);
    if (!sign) return NULL;
    char url[640];
    snprintf(url, sizeof(url), "%s?%snonce=%s", api_path,
             (query && query[0]) ? query : "", nonce);
    /* note: if query non-empty it must end with '&' */
    char headers[1024];
    snprintf(headers, sizeof(headers),
             "API-Key: %s\r\nAPI-Sign: %s\r\n", k->api_key, sign);
    free(sign);
    int st = 0;
    char *body = kraken_http_get(url, &st);
    k->last_http_ok = (body != NULL && st >= 200 && st < 300);
    if (k->last_http_ok) k->connected = true;
    return body;
}

static ts_status kraken_poll(venue_t *v, ts_venue_event *out)
{
    struct kraken_impl *k = v->impl;
    /* 1) drain queued events first */
    if (k->ev_count > 0) {
        *out = k->events[k->ev_head].ev;
        k->ev_head = (k->ev_head + 1) % KRAKEN_MAX_EVENTS;
        k->ev_count--;
        return TS_OK;
    }
    /* 2) throttle REST work */
    ts_ts_ns now = now_ns(k);
    if (now - k->last_poll_ns < KRAKEN_POLL_INTERVAL_NS) {
        return TS_ERR_NOT_FOUND;
    }
    k->last_poll_ns = now;
    if (!k->has_creds) return TS_ERR_NOT_FOUND;

    /* TradesHistory diff -> EXEC events (queued; returned next poll) */
    int status = 0;
    char *body = signed_get(k, "/private/TradesHistory", "");
    if (!body) return TS_ERR_NOT_FOUND;

    const char *r = strstr(body, "\"result\"");
    if (r) {
        const char *res_end = body + strlen(body);
        /* iterate trade objects: each starts with "<tradeid>":{ */
        const char *p = strchr(r, '{');
        while (p && p < res_end) {
            const char *obj_end = strchr(p, '}');
            if (!obj_end) break;
            char trade_id[TS_MAX_ID] = "";
            /* key precedes '{' : find last '"' before p */
            const char *kq = p;
            while (kq > r && *kq != '"') kq--;
            const char *kq2 = kq;
            while (kq2 > r && *(kq2 - 1) != '"') kq2--;
            if (kq2 > r && kq - kq2 < (ptrdiff_t)sizeof(trade_id)) {
                size_t n = (size_t)(kq - kq2);
                memcpy(trade_id, kq2, n);
                trade_id[n] = '\0';
            }
            if (trade_id[0]) {
                /* seen? */
                bool seen = false;
                for (size_t i = 0; i < k->seen_count; ++i) {
                    if (strcmp(k->seen_trades[i], trade_id) == 0) {
                        seen = true; break;
                    }
                }
                if (!seen) {
                    char txid[TS_MAX_ID] = "";
                    json_str(p, obj_end, "ordertxid", txid, sizeof(txid));
                    kraken_order *ko = find_by_txid(k, txid);
                    if (ko && !ko->terminal) {
                        ts_venue_event ev;
                        make_ev(k, &ev, TS_VEV_EXEC, ko->client_id,
                                ko->instrument);
                        ts_copy_str(ev.exec.client_id,
                                    sizeof(ev.exec.client_id), ko->client_id);
                        ts_copy_str(ev.exec.venue_order_id,
                                    sizeof(ev.exec.venue_order_id), txid);
                        ts_copy_str(ev.exec.venue_exec_id,
                                    sizeof(ev.exec.venue_exec_id), trade_id);
                        ts_copy_str(ev.exec.instrument,
                                    sizeof(ev.exec.instrument), ko->instrument);
                        ev.exec.side = ko->side;
                        json_num(p, obj_end, "vol", &ev.exec.qty);
                        json_num(p, obj_end, "price", &ev.exec.price);
                        json_num(p, obj_end, "fee", &ev.exec.fee);
                        ev.exec.ts_ns = now;
                        ko->filled_qty += ev.exec.qty;
                        if (ko->filled_qty >= ko->qty - 1e-12) {
                            ko->terminal = true;
                        }
                        push_event(k, &ev);
                    }
                    /* record seen */
                    if (k->seen_count < KRAKEN_MAX_TRADES_SEEN) {
                        ts_copy_str(k->seen_trades[k->seen_count],
                                    sizeof(k->seen_trades[0]), trade_id);
                        k->seen_count++;
                    }
                }
            }
            p = strchr(obj_end, '{');
        }
    }
    free(body);
    (void)status;
    /* return next queued event if the diff produced any */
    if (k->ev_count > 0) {
        *out = k->events[k->ev_head].ev;
        k->ev_head = (k->ev_head + 1) % KRAKEN_MAX_EVENTS;
        k->ev_count--;
        return TS_OK;
    }
    return TS_ERR_NOT_FOUND;
}

static bool kraken_connected(venue_t *v)
{
    struct kraken_impl *k = v->impl;
    return k->connected;
}

static ts_status kraken_open_orders(venue_t *v, ts_venue_open_order *out,
                                    size_t cap, size_t *count)
{
    struct kraken_impl *k = v->impl;
    if (!k->has_creds) return TS_ERR_BAD_STATE;
    int status = 0;
    char *body = signed_get(k, "/private/OpenOrders", "");
    if (!body) return TS_ERR_BAD_STATE;
    size_t n = 0;
    const char *r = strstr(body, "\"result\"");
    const char *res_end = body + strlen(body);
    if (r) {
        const char *p = strchr(r, '{');
        while (p && p < res_end && n < cap) {
            const char *obj_end = strchr(p, '}');
            if (!obj_end) break;
            char descr[TS_MAX_ID] = "";
            json_str(p, obj_end, "descr", descr, sizeof(descr));
            /* descr is nested {"order":"buy ..."} — extract order txid
             * from "order":"O..." inside descr */
            const char *dord = strstr(descr, "\"order\":\"");
            char txid[TS_MAX_ID] = "";
            if (dord) {
                const char *s = dord + 9;
                const char *e = strchr(s, '"');
                if (e && e - s < (ptrdiff_t)sizeof(txid)) {
                    memcpy(txid, s, (size_t)(e - s));
                    txid[e - s] = '\0';
                }
            }
            if (txid[0]) {
                kraken_order *ko = find_by_txid(k, txid);
                ts_venue_open_order *oo = &out[n++];
                memset(oo, 0, sizeof(*oo));
                if (ko) {
                    ts_copy_str(oo->client_id, sizeof(oo->client_id),
                                ko->client_id);
                }
                ts_copy_str(oo->venue_order_id, sizeof(oo->venue_order_id),
                            txid);
                ts_copy_str(oo->instrument, sizeof(oo->instrument),
                            ko ? ko->instrument : "");
                oo->side = ko ? ko->side : TS_SIDE_BUY;
                oo->qty = ko ? ko->qty : 0.0;
                oo->filled_qty = ko ? ko->filled_qty : 0.0;
                oo->price = ko ? ko->price : 0.0;
            }
            p = strchr(obj_end, '{');
        }
    }
    free(body);
    if (count) *count = n;
    return TS_OK;
}

static void kraken_destroy(venue_t *v)
{
    if (!v) return;
    /* never log credentials */
    memset(v->impl, 0, sizeof(struct kraken_impl));
    free(v->impl);
    free(v);
}

static const venue_ops_t g_kraken_ops = {
    .name = "kraken",
    .submit = kraken_submit,
    .cancel = kraken_cancel,
    .on_market_event = kraken_on_market_event,
    .poll = kraken_poll,
    .connected = kraken_connected,
    .cancel_all = kraken_cancel_all,
    .open_orders = kraken_open_orders,
    .destroy = kraken_destroy,
};

venue_t *venue_kraken_create(const char *api_key, const char *api_secret,
                             ts_metrics *metrics, ts_clock clock)
{
    venue_t *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    struct kraken_impl *k = calloc(1, sizeof(*k));
    if (!k) { free(v); return NULL; }
    if (api_key && api_secret && api_key[0] && api_secret[0]) {
        ts_copy_str(k->api_key, sizeof(k->api_key), api_key);
        ts_copy_str(k->api_secret, sizeof(k->api_secret), api_secret);
        k->has_creds = true;
        k->connected = true; /* optimistic until first failure */
    } else {
        k->connected = false;
    }
    k->metrics = metrics;
    k->clock = clock;
    v->ops = &g_kraken_ops;
    v->impl = k;
    return v;
}

/* Public market-data helper: fetch Ticker and normalize to QUOTE.
 * Returns TS_OK and fills *out, or error. Caller drives cadence. */
ts_status venue_kraken_get_ticker(venue_t *v, const char *instrument,
                                  ts_mkt_event *out, ts_clock clock)
{
    struct kraken_impl *k = v->impl;
    char pair[64];
    kraken_pair(instrument, pair, sizeof(pair));
    char path[128];
    snprintf(path, sizeof(path), "/public/Ticker?pair=%s", pair);
    /* kraken_http_get prepends /0 */
    int status = 0;
    char *body = kraken_http_get(path, &status);
    if (!body || status != 200) { free(body); return TS_ERR_BAD_STATE; }

    const char *r = strstr(body, "\"result\"");
    if (!r) { free(body); return TS_ERR_BAD_STATE; }
    const char *end = body + strlen(body);
    double bid = 0, ask = 0, last = 0;
    /* "b":[["px","vol"]], "a":[[...]], "c":[["last","lot"]] */
    const char *bp = json_find(r, end, "b");
    if (bp) { const char *q1 = strchr(bp, '"'); if (q1) { const char *q2 = strchr(q1+1,'"'); if(q2){ char tmp[32]; size_t n=q2-q1-1; if(n<31){memcpy(tmp,q1+1,n);tmp[n]=0;bid=strtod(tmp,NULL);} } } }
    const char *ap = json_find(r, end, "a");
    if (ap) { const char *q1 = strchr(ap, '"'); if (q1) { const char *q2 = strchr(q1+1,'"'); if(q2){ char tmp[32]; size_t n=q2-q1-1; if(n<31){memcpy(tmp,q1+1,n);tmp[n]=0;ask=strtod(tmp,NULL);} } } }
    const char *cp = json_find(r, end, "c");
    if (cp) { const char *q1 = strchr(cp, '"'); if (q1) { const char *q2 = strchr(q1+1,'"'); if(q2){ char tmp[32]; size_t n=q2-q1-1; if(n<31){memcpy(tmp,q1+1,n);tmp[n]=0;last=strtod(tmp,NULL);} } } }
    free(body);
    if (bid <= 0 && ask <= 0 && last <= 0) return TS_ERR_BAD_STATE;

    memset(out, 0, sizeof(*out));
    out->type = TS_MKT_QUOTE;
    ts_copy_str(out->instrument, sizeof(out->instrument), instrument);
    out->bid = bid;
    out->ask = ask;
    out->last = last;
    out->ts_ns = clock.now_ns(clock.ctx);
    k->last_ticker_ns = out->ts_ns;
    return TS_OK;
}
