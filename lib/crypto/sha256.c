/*
SurfOS crypto: SHA-256 and HMAC-SHA256
--------------------
File: sha256.c  Date: 10/9/26 (Wi-Fi: WPA2)
--------------------
SHA-256 (FIPS 180-4) and HMAC over it (RFC 2104), for crypto_random()'s HMAC-DRBG (and
later WPA3). Like sha1.c: bytes in and out, big end first, no alignment needed, contexts
wiped when finished. See include/crypto/crypto.h; tests/host/test_crypto.c checks it
against FIPS 180 and RFC 4231.
*/

#include <surfos/types.h>
#include <crypto/crypto.h>
#include <blibc_common.h>

/* the first 32 bits of the fractional parts of the cube roots of the first 64 primes */
static const u32 K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static u32 ror(u32 x, int n) { return (x >> n) | (x << (32 - n)); }
static u32 get_be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static void put_be32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

static void sha256_block(u32 h[8], const u8 *p) {
    u32 w[16], s[8], t1, t2, s0, s1;
    int i;
    for(i = 0; i < 16; i++) w[i] = get_be32(p + 4 * i);
    for(i = 0; i < 8; i++) s[i] = h[i];
    for(i = 0; i < 64; i++) {
        if(i >= 16) {
            s0 = w[(i + 1) & 15];
            s1 = w[(i + 14) & 15];
            s0 = ror(s0, 7) ^ ror(s0, 18) ^ (s0 >> 3);
            s1 = ror(s1, 17) ^ ror(s1, 19) ^ (s1 >> 10);
            w[i & 15] += s0 + s1 + w[(i + 9) & 15];
        }
        t1 = s[7] + (ror(s[4], 6) ^ ror(s[4], 11) ^ ror(s[4], 25)) + ((s[4] & s[5]) ^ (~s[4] & s[6])) + K[i] + w[i & 15];
        t2 = (ror(s[0], 2) ^ ror(s[0], 13) ^ ror(s[0], 22)) + ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
        s[7] = s[6]; s[6] = s[5]; s[5] = s[4]; s[4] = s[3] + t1;
        s[3] = s[2]; s[2] = s[1]; s[1] = s[0]; s[0] = t1 + t2;
    }
    for(i = 0; i < 8; i++) h[i] += s[i];
}

void sha256_init(struct sha256_ctx *c) {
    c->h[0] = 0x6a09e667;
    c->h[1] = 0xbb67ae85;
    c->h[2] = 0x3c6ef372;
    c->h[3] = 0xa54ff53a;
    c->h[4] = 0x510e527f;
    c->h[5] = 0x9b05688c;
    c->h[6] = 0x1f83d9ab;
    c->h[7] = 0x5be0cd19;
    c->len = 0;
    c->used = 0;
}

void sha256_update(struct sha256_ctx *c, const void *data, size_t len) {
    const u8 *p = data;
    size_t n;
    if(!len) return;
    c->len += len;
    if(c->used) {
        n = 64 - c->used;
        if(n > len) n = len;
        memcpy(c->buf + c->used, p, n);
        c->used += n;
        p += n;
        len -= n;
        if(c->used < 64) return;
        sha256_block(c->h, c->buf);
        c->used = 0;
    }
    for(; len >= 64; p += 64, len -= 64) sha256_block(c->h, p);
    if(len) {
        memcpy(c->buf, p, len);
        c->used = len;
    }
}

void sha256_final(struct sha256_ctx *c, u8 out[SHA256_LEN]) {
    u64 bits = c->len << 3;
    int i;
    c->buf[c->used++] = 0x80;
    if(c->used > 56) {
        memset(c->buf + c->used, 0, 64 - c->used);
        sha256_block(c->h, c->buf);
        c->used = 0;
    }
    memset(c->buf + c->used, 0, 56 - c->used);
    put_be32(c->buf + 56, (u32)(bits >> 32));
    put_be32(c->buf + 60, (u32)bits);
    sha256_block(c->h, c->buf);
    for(i = 0; i < 8; i++) put_be32(out + 4 * i, c->h[i]);
    crypto_wipe(c, sizeof(*c));
}

void sha256(const void *data, size_t len, u8 out[SHA256_LEN]) {
    struct sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

void hmac_sha256_init(struct hmac_sha256_ctx *c, const u8 *key, size_t key_len) {
    u8 k[64], pad[64];
    int i;
    memset(k, 0, sizeof(k));
    if(key_len > sizeof(k)) sha256(key, key_len, k);
    else if(key_len) memcpy(k, key, key_len);
    for(i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&c->inner);
    sha256_update(&c->inner, pad, sizeof(pad));
    for(i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&c->outer);
    sha256_update(&c->outer, pad, sizeof(pad));
    crypto_wipe(k, sizeof(k));
    crypto_wipe(pad, sizeof(pad));
}

void hmac_sha256_update(struct hmac_sha256_ctx *c, const void *data, size_t len) {
    sha256_update(&c->inner, data, len);
}

void hmac_sha256_final(struct hmac_sha256_ctx *c, u8 out[SHA256_LEN]) {
    u8 ih[SHA256_LEN];
    sha256_final(&c->inner, ih);
    sha256_update(&c->outer, ih, sizeof(ih));
    sha256_final(&c->outer, out);
    crypto_wipe(ih, sizeof(ih));
}

void hmac_sha256(const u8 *key, size_t key_len, const void *data, size_t len, u8 out[SHA256_LEN]) {
    struct hmac_sha256_ctx c;
    hmac_sha256_init(&c, key, key_len);
    hmac_sha256_update(&c, data, len);
    hmac_sha256_final(&c, out);
}
