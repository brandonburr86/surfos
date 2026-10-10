/*
SurfOS crypto: SHA-1 and HMAC-SHA1
--------------------
File: sha1.c    Date: 10/9/26 (Wi-Fi: WPA2)
--------------------
SHA-1 (FIPS 180-4) and HMAC over it (RFC 2104), for PBKDF2, the IEEE 802.11 PRF and the
EAPOL-Key MIC of the WPA2 supplicant. Words are read and written a byte at a time, big
end first, so no buffer needs any alignment and the host's byte order does not matter.
A context is wiped when its hash is finished: for HMAC it holds keyed state.
See include/crypto/crypto.h; tests/host/test_crypto.c checks it against FIPS 180 and
RFC 2202.
*/

#include <surfos/types.h>
#include <crypto/crypto.h>
#include <blibc_common.h>

static u32 rol(u32 x, int n) { return (x << n) | (x >> (32 - n)); }
static u32 get_be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static void put_be32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

/* one 64-byte block; the message schedule is kept as a ring of 16 words */
static void sha1_block(u32 h[5], const u8 *p) {
    u32 w[16], a, b, c, d, e, t;
    int i;
    for(i = 0; i < 16; i++) w[i] = get_be32(p + 4 * i);
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
    for(i = 0; i < 80; i++) {
        if(i >= 16) w[i & 15] = rol(w[(i + 13) & 15] ^ w[(i + 8) & 15] ^ w[(i + 2) & 15] ^ w[i & 15], 1);
        if(i < 20) t = ((b & c) | (~b & d)) + 0x5a827999;
        else if(i < 40) t = (b ^ c ^ d) + 0x6ed9eba1;
        else if(i < 60) t = ((b & c) | (b & d) | (c & d)) + 0x8f1bbcdc;
        else t = (b ^ c ^ d) + 0xca62c1d6;
        t += rol(a, 5) + e + w[i & 15];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1_init(struct sha1_ctx *c) {
    c->h[0] = 0x67452301;
    c->h[1] = 0xefcdab89;
    c->h[2] = 0x98badcfe;
    c->h[3] = 0x10325476;
    c->h[4] = 0xc3d2e1f0;
    c->len = 0;
    c->used = 0;
}

void sha1_update(struct sha1_ctx *c, const void *data, size_t len) {
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
        sha1_block(c->h, c->buf);
        c->used = 0;
    }
    for(; len >= 64; p += 64, len -= 64) sha1_block(c->h, p);
    if(len) {
        memcpy(c->buf, p, len);
        c->used = len;
    }
}

void sha1_final(struct sha1_ctx *c, u8 out[SHA1_LEN]) {
    u64 bits = c->len << 3;
    int i;
    c->buf[c->used++] = 0x80;
    if(c->used > 56) {
        memset(c->buf + c->used, 0, 64 - c->used);
        sha1_block(c->h, c->buf);
        c->used = 0;
    }
    memset(c->buf + c->used, 0, 56 - c->used);
    put_be32(c->buf + 56, (u32)(bits >> 32));
    put_be32(c->buf + 60, (u32)bits);
    sha1_block(c->h, c->buf);
    for(i = 0; i < 5; i++) put_be32(out + 4 * i, c->h[i]);
    crypto_wipe(c, sizeof(*c));
}

void sha1(const void *data, size_t len, u8 out[SHA1_LEN]) {
    struct sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, data, len);
    sha1_final(&c, out);
}

/* RFC 2104: the inner and outer hashes start with the key, padded to a block, xor ipad and opad */
void hmac_sha1_init(struct hmac_sha1_ctx *c, const u8 *key, size_t key_len) {
    u8 k[64], pad[64];
    int i;
    memset(k, 0, sizeof(k));
    if(key_len > sizeof(k)) sha1(key, key_len, k);
    else if(key_len) memcpy(k, key, key_len);
    for(i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha1_init(&c->inner);
    sha1_update(&c->inner, pad, sizeof(pad));
    for(i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha1_init(&c->outer);
    sha1_update(&c->outer, pad, sizeof(pad));
    crypto_wipe(k, sizeof(k));
    crypto_wipe(pad, sizeof(pad));
}

void hmac_sha1_update(struct hmac_sha1_ctx *c, const void *data, size_t len) {
    sha1_update(&c->inner, data, len);
}

void hmac_sha1_final(struct hmac_sha1_ctx *c, u8 out[SHA1_LEN]) {
    u8 ih[SHA1_LEN];
    sha1_final(&c->inner, ih);
    sha1_update(&c->outer, ih, sizeof(ih));
    sha1_final(&c->outer, out);
    crypto_wipe(ih, sizeof(ih));
}

void hmac_sha1(const u8 *key, size_t key_len, const void *data, size_t len, u8 out[SHA1_LEN]) {
    struct hmac_sha1_ctx c;
    hmac_sha1_init(&c, key, key_len);
    hmac_sha1_update(&c, data, len);
    hmac_sha1_final(&c, out);
}
