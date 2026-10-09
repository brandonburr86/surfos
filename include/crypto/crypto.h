/*
SurfOS crypto: what Wi-Fi needs
----------------------
File: crypto.h  Date: 10/9/26
----------------------
lib/crypto/: hashes, MACs and ciphers for the WPA2 supplicant (net/wlan/wpa.c), pure C
with no allocation and no I/O except crypto_random(), host-tested against published test
vectors (tests/host/test_crypto.c). Track C of docs/superpowers/plans/2026-10-09-wifi.md
owns this file and may add to it; the names below are the contract.

Everything takes and gives bytes: no alignment or byte order is assumed of a buffer. An
output may be the same buffer as an input unless a comment says otherwise. Contexts are
wiped when a hash or MAC is finished; keys and keyed state are the caller's to wipe
(crypto_wipe).
*/

#ifndef _SURFOS_CRYPTO_H
#define _SURFOS_CRYPTO_H

#include <surfos/types.h>

#define SHA1_LEN   20
#define SHA256_LEN 32
#define AES_BLOCK  16

void sha1(const void *data, size_t len, u8 out[SHA1_LEN]);
void sha256(const void *data, size_t len, u8 out[SHA256_LEN]);
void hmac_sha1(const u8 *key, size_t key_len, const void *data, size_t len, u8 out[SHA1_LEN]);
void hmac_sha256(const u8 *key, size_t key_len, const void *data, size_t len, u8 out[SHA256_LEN]);

/* the same a piece at a time (10/9/26): init, any number of updates, final (which wipes the
   context). A context may be copied, for instance after keying an HMAC once. */
struct sha1_ctx { u32 h[5]; u64 len; u8 buf[64]; u_int used; };
void sha1_init(struct sha1_ctx *c);
void sha1_update(struct sha1_ctx *c, const void *data, size_t len);
void sha1_final(struct sha1_ctx *c, u8 out[SHA1_LEN]);

struct sha256_ctx { u32 h[8]; u64 len; u8 buf[64]; u_int used; };
void sha256_init(struct sha256_ctx *c);
void sha256_update(struct sha256_ctx *c, const void *data, size_t len);
void sha256_final(struct sha256_ctx *c, u8 out[SHA256_LEN]);

struct hmac_sha1_ctx { struct sha1_ctx inner, outer; };
void hmac_sha1_init(struct hmac_sha1_ctx *c, const u8 *key, size_t key_len);
void hmac_sha1_update(struct hmac_sha1_ctx *c, const void *data, size_t len);
void hmac_sha1_final(struct hmac_sha1_ctx *c, u8 out[SHA1_LEN]);

struct hmac_sha256_ctx { struct sha256_ctx inner, outer; };
void hmac_sha256_init(struct hmac_sha256_ctx *c, const u8 *key, size_t key_len);
void hmac_sha256_update(struct hmac_sha256_ctx *c, const void *data, size_t len);
void hmac_sha256_final(struct hmac_sha256_ctx *c, u8 out[SHA256_LEN]);

/* PBKDF2 with HMAC-SHA1 (RFC 8018); 0, or -1 when iterations is 0 (10/9/26: out must not
   overlap salt) */
int pbkdf2_sha1(const u8 *pass, size_t pass_len, const u8 *salt, size_t salt_len, u_int iterations,
                u8 *out, size_t out_len);

/* the IEEE 802.11 PRF with HMAC-SHA1 (802.11-2016 12.7.1.2): label, a zero byte, data, a counter.
   (10/9/26) out_len is in bytes (PRF-384 is 48), at most 5100; out must not overlap data */
void ieee80211_prf_sha1(const u8 *key, size_t key_len, const char *label, const u8 *data, size_t data_len,
                        u8 *out, size_t out_len);

/* AES-128 single blocks; a context holds the expanded key */
struct aes_ctx { u32 rk[44]; u32 drk[44]; };
void aes128_setkey(struct aes_ctx *ctx, const u8 key[16]);
void aes128_encrypt(const struct aes_ctx *ctx, const u8 in[AES_BLOCK], u8 out[AES_BLOCK]);
void aes128_decrypt(const struct aes_ctx *ctx, const u8 in[AES_BLOCK], u8 out[AES_BLOCK]);

/* AES key wrap (RFC 3394) with a 128-bit KEK: n 64-bit blocks of plain, n + 1 of wrapped.
   aes_unwrap: 0, or -1 when the integrity check fails */
void aes_wrap(const u8 kek[16], const u8 *plain, size_t plain_len, u8 *wrapped);
int aes_unwrap(const u8 kek[16], const u8 *wrapped, size_t wrapped_len, u8 *plain);

/* random bytes for nonces: RDRAND when the CPU has it, mixed with the TSC and timer jitter into
   an HMAC-DRBG (SHA-256); never a fixed sequence */
void crypto_random(void *buf, size_t len);

bool crypto_equal(const void *a, const void *b, size_t len);    /* in constant time */
void crypto_wipe(void *p, size_t len);                         /* not optimised away */

#endif
