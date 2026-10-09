/*
SurfOS crypto: PBKDF2-HMAC-SHA1 and the IEEE 802.11 PRF
--------------------
File: kdf.c     Date: 10/9/26 (Wi-Fi: WPA2)
--------------------
The two key derivations of WPA2-Personal: the passphrase and SSID to the PMK with
PBKDF2 (RFC 8018, 4096 iterations of HMAC-SHA1: 802.11-2016 J.4), and the PMK to the
PTK with the PRF of 802.11-2016 12.7.1.2. Both key HMAC once and copy the keyed
context for every block, so an iteration costs two SHA-1 blocks. Intermediate values
are wiped. tests/host/test_crypto.c checks them against RFC 6070 and Annex J.
*/

#include <surfos/types.h>
#include <crypto/crypto.h>
#include <blibc_common.h>

int pbkdf2_sha1(const u8 *pass, size_t pass_len, const u8 *salt, size_t salt_len, u_int iterations,
                u8 *out, size_t out_len) {
    struct hmac_sha1_ctx key, c;
    u8 u[SHA1_LEN], t[SHA1_LEN], cnt[4];
    u32 block;
    u_int i, j;
    size_t n;
    if(!iterations) return -1;
    hmac_sha1_init(&key, pass, pass_len);
    for(block = 1; out_len; block++) {
        /* T_block = U_1 ^ ... ^ U_c, U_1 = HMAC(P, S || INT(block)), U_i = HMAC(P, U_i-1) */
        cnt[0] = (u8)(block >> 24); cnt[1] = (u8)(block >> 16); cnt[2] = (u8)(block >> 8); cnt[3] = (u8)block;
        c = key;
        hmac_sha1_update(&c, salt, salt_len);
        hmac_sha1_update(&c, cnt, sizeof(cnt));
        hmac_sha1_final(&c, u);
        memcpy(t, u, sizeof(t));
        for(i = 1; i < iterations; i++) {
            c = key;
            hmac_sha1_update(&c, u, sizeof(u));
            hmac_sha1_final(&c, u);
            for(j = 0; j < SHA1_LEN; j++) t[j] ^= u[j];
        }
        n = out_len < SHA1_LEN ? out_len : SHA1_LEN;
        memcpy(out, t, n);
        out += n;
        out_len -= n;
    }
    crypto_wipe(&key, sizeof(key));
    crypto_wipe(&c, sizeof(c));
    crypto_wipe(u, sizeof(u));
    crypto_wipe(t, sizeof(t));
    return 0;
}

/* PRF-n(K, A, B) = the first n bits of H(K, A, B, 0) || H(K, A, B, 1) || ...,
   H(K, A, B, i) = HMAC-SHA1(K, A || 0 || B || i) with i a single byte */
void ieee80211_prf_sha1(const u8 *key, size_t key_len, const char *label, const u8 *data, size_t data_len,
                        u8 *out, size_t out_len) {
    struct hmac_sha1_ctx k, c;
    u8 h[SHA1_LEN], zero = 0, i;
    size_t label_len = strlen(label), n;
    if(out_len > 255 * SHA1_LEN) out_len = 255 * SHA1_LEN;
    hmac_sha1_init(&k, key, key_len);
    for(i = 0; out_len; i++) {
        c = k;
        hmac_sha1_update(&c, label, label_len);
        hmac_sha1_update(&c, &zero, 1);
        hmac_sha1_update(&c, data, data_len);
        hmac_sha1_update(&c, &i, 1);
        hmac_sha1_final(&c, h);
        n = out_len < SHA1_LEN ? out_len : SHA1_LEN;
        memcpy(out, h, n);
        out += n;
        out_len -= n;
    }
    crypto_wipe(&k, sizeof(k));
    crypto_wipe(&c, sizeof(c));
    crypto_wipe(h, sizeof(h));
}
