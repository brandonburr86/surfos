/* Host-side unit tests for lib/crypto/ (10/9/26, track C of the Wi-Fi brief), against published
   test vectors: FIPS 180-4 and RFC 6234 (SHA-1, SHA-256), RFC 2202 and RFC 4231 (HMAC), RFC 6070
   (PBKDF2-HMAC-SHA1; all but the 16777216-iteration case, which takes minutes under emulation) and
   IEEE 802.11-2016 Annex J.3 and J.4 (the PRF; the passphrase to the PSK); and the constant-time
   compare and the wiping. Built like the kernel objects and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <surfos/types.h>
#include <crypto/crypto.h>

void exit(int status);

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

static u_int unhex(const char *s, u8 *out) {
    u_int n = 0;
    int hi = -1, v;
    for(; *s; s++) {
        if(*s >= '0' && *s <= '9') v = *s - '0';
        else if(*s >= 'a' && *s <= 'f') v = *s - 'a' + 10;
        else if(*s >= 'A' && *s <= 'F') v = *s - 'A' + 10;
        else continue;
        if(hi < 0) hi = v;
        else { out[n++] = (u8)(hi << 4 | v); hi = -1; }
    }
    return n;
}

static void print_hex(const char *label, const u8 *p, u_int n) {
    u_int i;
    printf("      %s ", label);
    for(i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
}

/* got (n bytes) against a hex string of the same length */
static void check_hex(const u8 *got, u_int n, const char *hex, const char *what) {
    u8 want[1024];
    u_int m = unhex(hex, want);
    int ok = m == n && memcmp(got, want, n) == 0;
    check(ok, what);
    if(!ok) { print_hex("got ", got, n); print_hex("want", want, m); }
}

/**** SHA-1 and SHA-256 ****/

static const char *msg_448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
static const char *msg_test4 = "0123456701234567012345670123456701234567012345670123456701234567";

static void sha1_million(u_int chunk, u8 out[SHA1_LEN]) {
    static u8 a[1000];
    struct sha1_ctx c;
    u_int left = 1000000, n;
    memset(a, 'a', sizeof(a));
    sha1_init(&c);
    for(; left; left -= n) {
        n = left < chunk ? left : chunk;
        sha1_update(&c, a, n);
    }
    sha1_final(&c, out);
}

static void sha256_million(u_int chunk, u8 out[SHA256_LEN]) {
    static u8 a[1000];
    struct sha256_ctx c;
    u_int left = 1000000, n;
    memset(a, 'a', sizeof(a));
    sha256_init(&c);
    for(; left; left -= n) {
        n = left < chunk ? left : chunk;
        sha256_update(&c, a, n);
    }
    sha256_final(&c, out);
}

static void test_sha(void) {
    static u8 big[640];
    static const u_int chunks[] = { 1, 3, 55, 56, 63, 64, 65, 1000 };
    u8 h[SHA256_LEN];
    struct sha1_ctx c1;
    struct sha256_ctx c2;
    u_int i;
    char what[80];

    sha1("", 0, h);
    check_hex(h, SHA1_LEN, "da39a3ee5e6b4b0d3255bfef95601890afd80709", "SHA-1 of the empty string");
    sha1("abc", 3, h);
    check_hex(h, SHA1_LEN, "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1 \"abc\" (FIPS 180, RFC 6234 TEST1)");
    sha1(msg_448, strlen(msg_448), h);
    check_hex(h, SHA1_LEN, "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "SHA-1 of the 448-bit message (TEST2_1)");
    for(i = 0; i < 10; i++) memcpy(big + 64 * i, msg_test4, 64);
    sha1(big, 640, h);
    check_hex(h, SHA1_LEN, "dea356a2cddd90c7a7ecedc5ebb563934f460452", "SHA-1 RFC 6234 TEST4 (64 bytes x 10)");
    for(i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        sha1_million(chunks[i], h);
        snprintf(what, sizeof(what), "SHA-1 of a million 'a' (TEST3), fed %u bytes at a time", chunks[i]);
        check_hex(h, SHA1_LEN, "34aa973cd4c4daa4f61eeb2bdbad27316534016f", what);
    }

    sha256("", 0, h);
    check_hex(h, SHA256_LEN, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of the empty string");
    sha256("abc", 3, h);
    check_hex(h, SHA256_LEN, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 \"abc\" (FIPS 180, TEST1)");
    sha256(msg_448, strlen(msg_448), h);
    check_hex(h, SHA256_LEN, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "SHA-256 of the 448-bit message");
    sha256(big, 640, h);
    check_hex(h, SHA256_LEN, "594847328451bdfa85056225462cc1d867d877fb388df0ce35f25ab5562bfbb5", "SHA-256 RFC 6234 TEST4");
    for(i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        sha256_million(chunks[i], h);
        snprintf(what, sizeof(what), "SHA-256 of a million 'a', fed %u bytes at a time", chunks[i]);
        check_hex(h, SHA256_LEN, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", what);
    }

    /* the length counts bytes past 2^32 bits: 2^29 + 64 bytes would take too long; check that a
       split at every point of a two-block message gives the one-shot result instead */
    for(i = 0; i <= 128; i++) {
        u8 m[128], one[SHA256_LEN];
        u_int j;
        for(j = 0; j < 128; j++) m[j] = (u8)(j * 13 + 1);
        sha1(m, 128, one);
        sha1_init(&c1);
        sha1_update(&c1, m, i);
        sha1_update(&c1, m + i, 128 - i);
        sha1_final(&c1, h);
        if(memcmp(h, one, SHA1_LEN)) break;
        sha256(m, 128, one);
        sha256_init(&c2);
        sha256_update(&c2, m, i);
        sha256_update(&c2, m + i, 128 - i);
        sha256_final(&c2, h);
        if(memcmp(h, one, SHA256_LEN)) break;
    }
    check(i == 129, "SHA-1 and SHA-256: a message split anywhere hashes as one piece");

    sha1_init(&c1);
    sha1_update(&c1, "abc", 3);
    sha1_final(&c1, h);
    for(i = 0; i < sizeof(c1) && !((u8 *)&c1)[i]; i++);
    check(i == sizeof(c1), "sha1_final wipes its context");
}

/**** HMAC ****/

struct hmac_case { const char *key; u_int key_rep; const char *data; u_int data_rep; const char *mac; };

/* key and data as hex (repeated key_rep / data_rep times) or, when rep is 0, as text */
static u_int material(const char *s, u_int rep, u8 *out) {
    u8 one[256];
    u_int n, i;
    if(!rep) {
        n = strlen(s);
        memcpy(out, s, n);
        return n;
    }
    n = unhex(s, one);
    for(i = 0; i < rep; i++) memcpy(out + i * n, one, n);
    return n * rep;
}

static void test_hmac(void) {
    static const struct hmac_case rfc2202[] = {
        { "0b", 20, "Hi There", 0, "b617318655057264e28bc0b6fb378c8ef146be00" },
        { "Jefe", 0, "what do ya want for nothing?", 0, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79" },
        { "aa", 20, "dd", 50, "125d7342b9ac11cd91a39af48aa17b4f63f175d3" },
        { "0102030405060708090a0b0c0d0e0f10111213141516171819", 1, "cd", 50, "4c9007f4026250c6bc8414f9bf50c86c2d7235da" },
        { "0c", 20, "Test With Truncation", 0, "4c1a03424b55e07fe7f27be1d58bb9324a9a5a04" },
        { "aa", 80, "Test Using Larger Than Block-Size Key - Hash Key First", 0, "aa4ae5e15272d00e95705637ce8a3b55ed402112" },
        { "aa", 80, "Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data", 0,
          "e8e99d0f45237d786d6bbaa7965c7808bbff1a91" },
    };
    static const struct hmac_case rfc4231[] = {
        { "0b", 20, "Hi There", 0, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7" },
        { "Jefe", 0, "what do ya want for nothing?", 0, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843" },
        { "aa", 20, "dd", 50, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe" },
        { "0102030405060708090a0b0c0d0e0f10111213141516171819", 1, "cd", 50,
          "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b" },
        { "0c", 20, "Test With Truncation", 0, "a3b6167473100ee06e0c796c2955552b" },
        { "aa", 131, "Test Using Larger Than Block-Size Key - Hash Key First", 0,
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54" },
        { "aa", 131, "This is a test using a larger than block-size key and a larger than block-size data. The key "
          "needs to be hashed before being used by the HMAC algorithm.", 0,
          "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2" },
    };
    u8 key[256], data[256], mac[SHA256_LEN], mac2[SHA256_LEN];
    u_int i, kl, dl;
    char what[64];
    struct hmac_sha256_ctx c;

    for(i = 0; i < 7; i++) {
        kl = material(rfc2202[i].key, rfc2202[i].key_rep, key);
        dl = material(rfc2202[i].data, rfc2202[i].data_rep, data);
        hmac_sha1(key, kl, data, dl, mac);
        snprintf(what, sizeof(what), "HMAC-SHA1 RFC 2202 test case %u", i + 1);
        check_hex(mac, SHA1_LEN, rfc2202[i].mac, what);
    }
    for(i = 0; i < 7; i++) {
        kl = material(rfc4231[i].key, rfc4231[i].key_rep, key);
        dl = material(rfc4231[i].data, rfc4231[i].data_rep, data);
        hmac_sha256(key, kl, data, dl, mac);
        snprintf(what, sizeof(what), "HMAC-SHA256 RFC 4231 test case %u%s", i + 1, i == 4 ? " (128 bits)" : "");
        check_hex(mac, i == 4 ? 16 : SHA256_LEN, rfc4231[i].mac, what);
    }

    /* in pieces, with the output over the key */
    kl = material("aa", 131, key);
    dl = material(rfc4231[6].data, 0, data);
    hmac_sha256(key, kl, data, dl, mac);
    hmac_sha256_init(&c, key, kl);
    hmac_sha256_update(&c, data, 1);
    hmac_sha256_update(&c, data + 1, 70);
    hmac_sha256_update(&c, data + 71, dl - 71);
    hmac_sha256_final(&c, mac2);
    check(memcmp(mac, mac2, SHA256_LEN) == 0, "HMAC-SHA256 in three pieces = in one");
    memcpy(mac2, "Jefe", 4);
    hmac_sha1(mac2, 4, "what do ya want for nothing?", 28, mac2);
    check_hex(mac2, SHA1_LEN, rfc2202[1].mac, "HMAC-SHA1 with the output buffer over the key");
}

/**** PBKDF2 and the 802.11 key derivations ****/

static void test_kdf(void) {
    static const char *case3 = "Test Using Larger Than Block-Size Key - Hash Key First";
    u8 out[64], key[80];

    pbkdf2_sha1((const u8 *)"password", 8, (const u8 *)"salt", 4, 1, out, 20);
    check_hex(out, 20, "0c60c80f961f0e71f3a9b524af6012062fe037a6", "PBKDF2-HMAC-SHA1 RFC 6070 case 1 (c = 1)");
    pbkdf2_sha1((const u8 *)"password", 8, (const u8 *)"salt", 4, 2, out, 20);
    check_hex(out, 20, "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957", "RFC 6070 case 2 (c = 2)");
    pbkdf2_sha1((const u8 *)"password", 8, (const u8 *)"salt", 4, 4096, out, 20);
    check_hex(out, 20, "4b007901b765489abead49d926f721d065a429c1", "RFC 6070 case 3 (c = 4096)");
    pbkdf2_sha1((const u8 *)"passwordPASSWORDpassword", 24, (const u8 *)"saltSALTsaltSALTsaltSALTsaltSALTsalt", 36,
                4096, out, 25);
    check_hex(out, 25, "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038", "RFC 6070 case 5 (25 bytes: two blocks)");
    pbkdf2_sha1((const u8 *)"pass\0word", 9, (const u8 *)"sa\0lt", 5, 4096, out, 16);
    check_hex(out, 16, "56fa6aa75548099dcc37d7f03425e0c3", "RFC 6070 case 6 (NUL bytes inside)");
    check(pbkdf2_sha1((const u8 *)"password", 8, (const u8 *)"salt", 4, 0, out, 20) == -1, "PBKDF2 with 0 iterations: -1");

    /* 802.11-2016 J.4.3: PSK = PBKDF2(passphrase, SSID, 4096, 256 bits) */
    pbkdf2_sha1((const u8 *)"password", 8, (const u8 *)"IEEE", 4, 4096, out, 32);
    check_hex(out, 32, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", "802.11 J.4 \"password\", SSID \"IEEE\"");
    pbkdf2_sha1((const u8 *)"ThisIsAPassword", 15, (const u8 *)"ThisIsASSID", 11, 4096, out, 32);
    check_hex(out, 32, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af", "802.11 J.4 \"ThisIsAPassword\"");
    pbkdf2_sha1((const u8 *)"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 32, (const u8 *)"ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ", 32, 4096, out, 32);
    check_hex(out, 32, "becb93866bb8c3832cb777c2f559807c8c59afcb6eae734885001300a981cc62", "802.11 J.4 32 'a', 32 'Z'");

    /* 802.11-2016 J.3.2: the PRF with HMAC-SHA1 */
    memset(key, 0x0b, 20);
    ieee80211_prf_sha1(key, 20, "prefix", (const u8 *)"Hi There", 8, out, 24);
    check_hex(out, 24, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606", "802.11 PRF J.3 test case 1 (192 bits)");
    ieee80211_prf_sha1((const u8 *)"Jefe", 4, "prefix-2", (const u8 *)"what do ya want for nothing?", 28, out, 32);
    check_hex(out, 32, "47c4908e30c947521ad20be9053450ecbea23d3aa604b77326d8b3825ff7475c", "802.11 PRF J.3 test case 2 (256 bits)");
    memset(key, 0xaa, 80);
    ieee80211_prf_sha1(key, 80, "prefix-3", (const u8 *)case3, strlen(case3), out, 48);
    check_hex(out, 48, "0ab6c33ccf70d0d736f4b04c8a7373255511abc5073713163bd0b8c9eeb7e1956fa066820a73ddee3f6d3bd407e0682a",
              "802.11 PRF J.3 test case 3 (384 bits, an 80-byte key)");
    memset(key, 0x0b, 20);
    ieee80211_prf_sha1(key, 20, "prefix-4", (const u8 *)"Hi There Again", 14, out, 64);
    check_hex(out, 64, "248cfbc532ab38ffa483c8a2e40bf170eb542a2e0916d7bf6d97da2c4c5ca877736c53a65b03fa4b3745ce7613f6ad68"
              "e0e4a798b7cf691c96176fd634a59a49", "802.11 PRF J.3 test case 4 (512 bits)");
    ieee80211_prf_sha1(key, 20, "prefix-4", (const u8 *)"Hi There Again", 14, out, 48);
    check_hex(out, 48, "248cfbc532ab38ffa483c8a2e40bf170eb542a2e0916d7bf6d97da2c4c5ca877736c53a65b03fa4b3745ce7613f6ad68",
              "PRF-384 is the first 48 bytes of PRF-512");
}

/**** compare and wipe ****/

static void test_util(void) {
    u8 a[40], b[40];
    u_int i;
    memset(a, 0x5a, sizeof(a));
    memcpy(b, a, sizeof(b));
    check(crypto_equal(a, b, 40) == true, "crypto_equal: equal buffers");
    b[0] ^= 1;
    check(crypto_equal(a, b, 40) == false, "... the first byte differs");
    b[0] ^= 1;
    b[39] ^= 0x80;
    check(crypto_equal(a, b, 40) == false, "... the last byte differs");
    check(crypto_equal(a, b, 39) == true, "... not compared past len");
    check(crypto_equal(a, b, 0) == true, "... nothing to compare: equal");
    crypto_wipe(a, 33);
    for(i = 0; i < 33 && !a[i]; i++);
    check(i == 33 && a[33] == 0x5a, "crypto_wipe clears what it is given and no more");
}

int main(void) {
    test_sha();
    test_hmac();
    test_kdf();
    test_util();
    printf("CRYPTO UNIT TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    exit(failures ? 1 : 0);
    return 0;
}
