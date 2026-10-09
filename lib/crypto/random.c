/*
SurfOS crypto: random bytes
--------------------
File: random.c  Date: 10/9/26 (Wi-Fi: WPA2)
--------------------
crypto_random(), for the supplicant's nonces: an HMAC_DRBG with SHA-256 (NIST SP 800-90A
Rev. 1, 10.1.2) seeded from an entropy source, given fresh bytes from it as additional
input on every call and reseeded from it every RANDOM_RESEED_CALLS calls, so its output
is never a fixed sequence and does not rest on one seed for long.

The kernel's source reads RDRAND when CPUID.01H:ECX bit 30 says the CPU has it (the
laptop's i3-1005G1 does): ten tries a read, as Intel's DRNG Software Implementation Guide
advises, and not at all on a CPU whose first eight reads fail or never change (Linux's
check: some AMD parts gave all ones after a resume). Next to every RDRAND word goes the
low half of the TSC read right after it (RDRAND's latency varies), and the tick count
goes into the first. QEMU's default CPUs and a 486 have no RDRAND: the TSC and the ticks
are all there is then, which is weaker but still differs from boot to boot and call to
call. crypto_random_source() replaces the source (tests/host/test_crypto.c: known-answer
tests with a fixed one, and the kernel's with RDRAND hidden).

A mutex guards the generator: task context only.
*/

#include <surfos/types.h>
#include <surfos/cpu.h>
#include <surfos/timer.h>
#include <surfos/sync.h>
#include <crypto/crypto.h>
#include <blibc_common.h>

#define CPUID_1_ECX_RDRAND  (1u << 30)

#define RANDOM_SEED_LEN     64      /* entropy input and nonce at a (re)seed: 32 bytes of RDRAND */
#define RANDOM_FRESH_LEN    16      /* additional input to every call */
#define RANDOM_RESEED_CALLS 16
#define RANDOM_CHUNK        1024    /* bytes per generate request */

#define DRBG_RESEED_INTERVAL 0x100000   /* requests between seeds (SP 800-90A allows 2^48) */
#define DRBG_MAX_REQUEST     65536      /* bytes (2^19 bits) */

/**** HMAC_DRBG ****/

/* HMAC_DRBG_Update with the provided data given in up to three pieces */
static void drbg_update(struct hmac_drbg *d, const u8 *a, size_t a_len, const u8 *b, size_t b_len,
                        const u8 *c, size_t c_len) {
    struct hmac_sha256_ctx h;
    u8 round;
    for(round = 0; round < 2; round++) {
        hmac_sha256_init(&h, d->k, SHA256_LEN);
        hmac_sha256_update(&h, d->v, SHA256_LEN);
        hmac_sha256_update(&h, &round, 1);
        hmac_sha256_update(&h, a, a_len);
        hmac_sha256_update(&h, b, b_len);
        hmac_sha256_update(&h, c, c_len);
        hmac_sha256_final(&h, d->k);
        hmac_sha256(d->k, SHA256_LEN, d->v, SHA256_LEN, d->v);
        if(!a_len && !b_len && !c_len) break;
    }
}

void hmac_drbg_instantiate(struct hmac_drbg *d, const u8 *entropy, size_t entropy_len, const u8 *nonce,
                           size_t nonce_len, const u8 *pers, size_t pers_len) {
    memset(d->k, 0x00, SHA256_LEN);
    memset(d->v, 0x01, SHA256_LEN);
    drbg_update(d, entropy, entropy_len, nonce, nonce_len, pers, pers_len);
    d->reseed_counter = 1;
}

void hmac_drbg_reseed(struct hmac_drbg *d, const u8 *entropy, size_t entropy_len, const u8 *add, size_t add_len) {
    drbg_update(d, entropy, entropy_len, add, add_len, NULL, 0);
    d->reseed_counter = 1;
}

int hmac_drbg_generate(struct hmac_drbg *d, const u8 *add, size_t add_len, u8 *out, size_t out_len) {
    size_t n;
    if(d->reseed_counter > DRBG_RESEED_INTERVAL || out_len > DRBG_MAX_REQUEST) return -1;
    if(add_len) drbg_update(d, add, add_len, NULL, 0, NULL, 0);
    while(out_len) {
        hmac_sha256(d->k, SHA256_LEN, d->v, SHA256_LEN, d->v);
        n = out_len < SHA256_LEN ? out_len : SHA256_LEN;
        memcpy(out, d->v, n);
        out += n;
        out_len -= n;
    }
    drbg_update(d, add, add_len, NULL, 0, NULL, 0);
    d->reseed_counter++;
    return 0;
}

/**** the kernel's entropy source ****/

static bool rdrand_checked, rdrand_ok;

/* RDRAND r32 is 0f c7 /6; written as bytes so no assembler option is needed. The carry flag
   says whether a value came; Intel advises ten tries before calling the unit broken. */
static bool rdrand32(u32 *v) {
    u32 x;
    u8 ok;
    int i;
    for(i = 0; i < 10; i++) {
        asm volatile(".byte 0x0f, 0xc7, 0xf0\n\t"      /* rdrand %eax */
                     "setc %1" : "=a"(x), "=qm"(ok) : : "cc");
        if(ok) {
            *v = x;
            return true;
        }
    }
    return false;
}

static bool have_rdrand(void) {
    u32 v, prev = 0;
    int i, changed = 0;
    if(rdrand_checked) return rdrand_ok;
    rdrand_checked = true;
    rdrand_ok = false;
    if(!cpu.has_cpuid || !(cpu.features_ecx & CPUID_1_ECX_RDRAND)) return false;
    for(i = 0; i < 8; i++) {
        if(!rdrand32(&v)) return false;
        if(i && v != prev) changed++;
        prev = v;
    }
    rdrand_ok = changed > 0;
    return rdrand_ok;
}

static void put32(u8 *p, u32 v, u_int n) {
    u_int i;
    for(i = 0; i < n && i < 4; i++) p[i] = (u8)(v >> (8 * i));
}

/* records of 8 bytes: an RDRAND word (0 without it), then the TSC's low half (the ticks too in
   the first record) */
static void kernel_entropy(u8 *buf, u_int len) {
    bool rd = have_rdrand();
    u32 w, t;
    u_int i;
    for(i = 0; i < len; i += 8) {
        w = 0;
        if(rd && !rdrand32(&w)) w = 0;
        t = cpu.tsc ? (u32)rdtsc() : 0;
        if(!i) t ^= (u32)getticks();
        put32(buf + i, w, len - i);
        if(len - i > 4) put32(buf + i + 4, t, len - i - 4);
    }
}

/**** crypto_random() ****/

static mutex_t random_lock = MUTEX_INIT("random");
static struct hmac_drbg drbg;
static crypto_entropy_fn source;            /* NULL: the kernel's */
static bool seeded;
static u_int calls_since_seed;

static const char personalization[] = "SurfOS crypto_random";

static void gather(u8 *buf, u_int len) {
    if(source) source(buf, len);
    else kernel_entropy(buf, len);
}

void crypto_random(void *buf, size_t len) {
    u8 seed[RANDOM_SEED_LEN], fresh[RANDOM_FRESH_LEN], *out = buf;
    size_t n;
    mutex_lock(&random_lock);
    if(!seeded || calls_since_seed >= RANDOM_RESEED_CALLS) {
        gather(seed, sizeof(seed));
        if(!seeded)
            hmac_drbg_instantiate(&drbg, seed, 48, seed + 48, 16, (const u8 *)personalization,
                                  sizeof(personalization) - 1);
        else hmac_drbg_reseed(&drbg, seed, sizeof(seed), NULL, 0);
        seeded = true;
        calls_since_seed = 0;
    }
    gather(fresh, sizeof(fresh));
    calls_since_seed++;
    for(; len; out += n, len -= n) {
        n = len < RANDOM_CHUNK ? len : RANDOM_CHUNK;
        hmac_drbg_generate(&drbg, fresh, sizeof(fresh), out, n);   /* never refuses: reseeded long before */
    }
    mutex_unlock(&random_lock);
    crypto_wipe(seed, sizeof(seed));
    crypto_wipe(fresh, sizeof(fresh));
}

void crypto_random_source(crypto_entropy_fn fn) {
    mutex_lock(&random_lock);
    source = fn;
    seeded = false;
    calls_since_seed = 0;
    rdrand_checked = false;                 /* the kernel's source looks at CPUID again */
    crypto_wipe(&drbg, sizeof(drbg));
    mutex_unlock(&random_lock);
}
