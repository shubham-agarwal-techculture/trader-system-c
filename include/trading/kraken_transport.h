#ifndef TRADING_KRAKEN_TRANSPORT_H
#define TRADING_KRAKEN_TRANSPORT_H

#include "trading/core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal HTTPS transport for the Kraken adapter (WinHTTP on Windows).
 * Synchronous request/response; the venue's poll() drives throttling. */

/* GET https://api.kraken.com<path> ; returns malloc'd body or NULL. */
char *kraken_http_get(const char *path_with_query, int *http_status);

/* POST form-encoded body to https://api.kraken.com<path>.
 * extra_headers: optional "Name: value\r\n" block (may be NULL).
 * Returns malloc'd body or NULL; caller frees. */
char *kraken_http_post(const char *path, const char *post_data,
                       const char *extra_headers, int *http_status);

/* Crypto helpers for API-Sign (HMAC-SHA256). */
void kraken_sha256(const unsigned char *data, size_t len,
                   unsigned char out[32]);
void kraken_hmac_sha256(const unsigned char *key, size_t key_len,
                        const unsigned char *data, size_t data_len,
                        unsigned char out[32]);
char *kraken_base64_encode(const unsigned char *data, size_t len);
int kraken_base64_decode(const char *b64, unsigned char *out, size_t outcap);

/* Build the Kraken API-Sign header value:
 *   sign = base64( HMAC-SHA256( base64decode(api_secret),
 *             urlpath + sha256(nonce + postdata) ) )
 * urlpath includes leading '/', e.g. "/0/private/AddOrder". */
char *kraken_api_sign(const char *urlpath, const char *nonce,
                      const char *post_data, const char *api_secret_b64);

#ifdef __cplusplus
}
#endif

#endif /* TRADING_KRAKEN_TRANSPORT_H */
