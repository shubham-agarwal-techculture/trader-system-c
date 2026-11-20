#include "trading/kraken_transport.h"

#include <string.h>

/* Compact SHA-256 + HMAC + base64 for Kraken API-Sign.
 * Public-domain-style implementation, dependency-free. */

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_block(uint32_t st[8], const unsigned char *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=st[0],b=st[1],c=st[2],d=st[3],e=st[4],f=st[5],g=st[6],h=st[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e,6)^rotr(e,11)^rotr(e,25);
        uint32_t ch = (e&f)^((~e)&g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a,2)^rotr(a,13)^rotr(a,22);
        uint32_t mj = (a&b)^(a&c)^(b&c);
        uint32_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    st[0]+=a; st[1]+=b; st[2]+=c; st[3]+=d;
    st[4]+=e; st[5]+=f; st[6]+=g; st[7]+=h;
}

void kraken_sha256(const unsigned char *data, size_t len,
                   unsigned char out[32])
{
    uint32_t st[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                      0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    size_t i = 0;
    while (i + 64 <= len) {
        sha256_block(st, data + i);
        i += 64;
    }
    unsigned char tail[128];
    size_t rem = len - i;
    memcpy(tail, data + i, rem);
    tail[rem++] = 0x80;
    size_t total = (rem <= 56) ? 64 : 128;
    memset(tail + rem, 0, total - rem - 8);
    uint64_t bits = (uint64_t)len * 8;
    for (int j = 0; j < 8; ++j) {
        tail[total - 1 - j] = (unsigned char)(bits >> (8 * j));
    }
    sha256_block(st, tail);
    if (total == 128) sha256_block(st, tail + 64);
    for (int j = 0; j < 8; ++j) {
        out[j*4]   = (unsigned char)(st[j] >> 24);
        out[j*4+1] = (unsigned char)(st[j] >> 16);
        out[j*4+2] = (unsigned char)(st[j] >> 8);
        out[j*4+3] = (unsigned char)(st[j]);
    }
}

void kraken_hmac_sha256(const unsigned char *key, size_t key_len,
                        const unsigned char *data, size_t data_len,
                        unsigned char out[32])
{
    unsigned char k[64];
    memset(k, 0, sizeof(k));
    if (key_len > 64) {
        kraken_sha256(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }
    unsigned char ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    unsigned char inner[32];
    /* inner = sha256(ipad || data) — do it streaming manually */
    {
        /* concatenate ipad+data */
        size_t total = 64 + data_len;
        unsigned char *buf = malloc(total);
        if (!buf) { memset(out, 0, 32); return; }
        memcpy(buf, ipad, 64);
        memcpy(buf + 64, data, data_len);
        kraken_sha256(buf, total, inner);
        free(buf);
    }
    {
        unsigned char buf[96];
        memcpy(buf, opad, 64);
        memcpy(buf + 64, inner, 32);
        kraken_sha256(buf, 96, out);
    }
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *kraken_base64_encode(const unsigned char *data, size_t len)
{
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = malloc(out_len + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i+1] << 8;
        if (i + 2 < len) v |= data[i+2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? B64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? B64[v & 63] : '=';
    }
    out[o] = '\0';
    return out;
}

static int b64val(char c)
{
    const char *p = strchr(B64, c);
    return p ? (int)(p - B64) : -1;
}

int kraken_base64_decode(const char *b64, unsigned char *out, size_t outcap)
{
    size_t len = strlen(b64);
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; ++i) {
        if (b64[i] == '=') break;
        int v = b64val(b64[i]);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o < outcap) out[o++] = (unsigned char)(acc >> bits);
            acc &= (1u << bits) - 1;
        }
    }
    return (int)o;
}

char *kraken_api_sign(const char *urlpath, const char *nonce,
                      const char *post_data, const char *api_secret_b64)
{
    /* message = urlpath + sha256(nonce + postdata) (hex) */
    char *nonce_post = malloc(strlen(nonce) + strlen(post_data) + 1);
    if (!nonce_post) return NULL;
    strcpy(nonce_post, nonce);
    strcat(nonce_post, post_data);
    unsigned char sha[32];
    kraken_sha256((const unsigned char *)nonce_post,
                  strlen(nonce_post), sha);
    free(nonce_post);

    char hex[65];
    for (int i = 0; i < 32; ++i) {
        snprintf(hex + i * 2, 3, "%02x", sha[i]);
    }

    char *msg = malloc(strlen(urlpath) + 64 + 1);
    if (!msg) return NULL;
    strcpy(msg, urlpath);
    strcat(msg, hex);

    unsigned char secret[128];
    int slen = kraken_base64_decode(api_secret_b64, secret, sizeof(secret));
    if (slen <= 0) { free(msg); return NULL; }

    unsigned char mac[32];
    kraken_hmac_sha256(secret, (size_t)slen,
                       (const unsigned char *)msg, strlen(msg), mac);
    free(msg);
    return kraken_base64_encode(mac, 32);
}
