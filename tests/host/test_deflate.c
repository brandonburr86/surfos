/* Host-side test of libsurf's CRC-32, inflate and deflate (11/01/26): user/libsurf/crc32.c, inflate.c and deflate.c as
   they are. Covered: CRC-32 against its check values; inflate on hand-made streams, stored, fixed and dynamic, every kind
   of stream it must refuse, each cut short at every byte, with one byte of input or of room at a time, stopping exactly
   at the end of the data; deflate at every level over a corpus (empty, tiny, runs, periods, random, a mix, matches at the
   longest distances, a block of the worst symbols), its output inflated again in chunks of many sizes; mutated and
   random streams, which must end and stay inside their buffers (run under AddressSanitizer and UBSan where the host
   compiler has them, by `make unittest`). Interoperation with the host's gzip is tests/host/test_gztool.c's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inflate.h>
#include <deflate.h>
#include <crc32.h>

static int checks, failures;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static struct inflate *iz;
static struct deflate *dz;
static unsigned int rng = 12345;

static unsigned int rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/**** a bit writer for hand-made streams ****/

struct bw { unsigned char b[70000]; size_t n; unsigned int acc; int cnt; };

static void bw_start(struct bw *w) {
    memset(w, 0, sizeof(*w));
}

static void bw_put(struct bw *w, unsigned int v, int n) {
    while(n--) {
        w->acc |= (v & 1) << w->cnt;
        v >>= 1;
        if(++w->cnt == 8) {
            w->b[w->n++] = (unsigned char)w->acc;
            w->acc = 0;
            w->cnt = 0;
        }
    }
}

/* a Huffman code: most significant bit first */
static void bw_code(struct bw *w, unsigned int code, int n) {
    while(n--) bw_put(w, (code >> n) & 1, 1);
}

static void bw_end(struct bw *w) {
    if(w->cnt) bw_put(w, 0, 8 - w->cnt);
}

/* a fixed-code symbol */
static void bw_fixed(struct bw *w, unsigned int s) {
    if(s < 144) bw_code(w, 0x30 + s, 8);
    else if(s < 256) bw_code(w, 0x190 + s - 144, 9);
    else if(s < 280) bw_code(w, s - 256, 7);
    else bw_code(w, 0xC0 + s - 280, 8);
}

/**** running inflate ****/

/* the whole stream at once, into a buffer of exactly cap bytes: the result, *made bytes; *rest input left */
static int run_all(const unsigned char *in, size_t n, unsigned char *out, size_t cap, size_t *made, size_t *rest) {
    int r;
    inflate_init(iz);
    iz->in = in;
    iz->in_n = n;
    iz->out = out;
    iz->out_n = cap;
    r = inflate_run(iz);
    if(made) *made = cap - iz->out_n;
    if(rest) *rest = iz->in_n;
    return r;
}

static int refused(const unsigned char *in, size_t n) {
    static unsigned char out[1 << 17];
    return run_all(in, n, out, sizeof(out), NULL, NULL) == INFLATE_BAD;
}

/* one input chunk size and one room size, until END or nothing moves */
static int run_chunks(const unsigned char *in, size_t n, size_t ichunk, size_t ochunk, unsigned char *out, size_t cap,
                      size_t *made, size_t *rest) {
    size_t got = 0, fed = 0;
    int r;
    inflate_init(iz);
    for(;;) {
        size_t i = n - fed < ichunk ? n - fed : ichunk, o = cap - got < ochunk ? cap - got : ochunk;
        iz->in = in + fed;
        iz->in_n = i;
        iz->out = out + got;
        iz->out_n = o;
        r = inflate_run(iz);
        fed += i - iz->in_n;
        got += o - iz->out_n;
        if(r != INFLATE_OK) break;
        if(i == iz->in_n && o == iz->out_n) break;      /* nothing used, nothing made */
    }
    *made = got;
    *rest = n - fed;
    return r;
}

/**** CRC-32 ****/

static void test_crc(void) {
    unsigned char m[1000];
    check(crc32(0, "", 0) == 0, "the CRC of nothing is 0");
    check(crc32(0, "123456789", 9) == 0xCBF43926u, "the CRC check value");
    check(crc32(crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u, "a CRC in two pieces");
    check(crc32(0, "The quick brown fox jumps over the lazy dog", 43) == 0x414FA339u, "the fox");
    memset(m, 0, sizeof(m));
    check(crc32(0, m, 32) == 0x190A55ADu, "32 zero bytes");
    memset(m, 0xff, sizeof(m));
    check(crc32(0, m, 32) == 0xFF6CAB0Bu, "32 bytes of ones");
}

/**** streams by hand ****/

static void test_known(void) {
    static const unsigned char stored[] = { 0x01, 5, 0, 0xfa, 0xff, 'h', 'e', 'l', 'l', 'o', 'X', 'Y' };
    static const unsigned char fixed[] = { 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00, 'X' };
    static const unsigned char empty[] = { 0x03, 0x00 };
    static const unsigned char empty_stored[] = { 0x01, 0, 0, 0xff, 0xff };
    unsigned char out[64];
    size_t made, rest;
    check(run_all(stored, sizeof(stored), out, sizeof(out), &made, &rest) == INFLATE_END && made == 5 &&
          !memcmp(out, "hello", 5) && rest == 2, "a stored block, and the two bytes after it left alone");
    check(run_all(fixed, sizeof(fixed), out, sizeof(out), &made, &rest) == INFLATE_END && made == 5 &&
          !memcmp(out, "hello", 5) && rest == 1, "a fixed block: 'hello' as zlib makes it");
    check(run_all(empty, sizeof(empty), out, sizeof(out), &made, &rest) == INFLATE_END && !made && !rest, "an empty fixed block");
    check(run_all(empty_stored, sizeof(empty_stored), out, sizeof(out), &made, &rest) == INFLATE_END && !made && !rest,
          "an empty stored block");
    check(run_all(empty, 1, out, sizeof(out), &made, &rest) == INFLATE_OK && !made, "cut short: OK, more wanted, not END");
    check(run_all(empty, 0, out, sizeof(out), &made, &rest) == INFLATE_OK && !made, "no input at all: more wanted");
    /* END also with no room left over: nothing more is wanted of the caller */
    check(run_all(fixed, sizeof(fixed), out, 5, &made, &rest) == INFLATE_END && made == 5 && rest == 1,
          "END when the room is exactly the output");
    check(run_all(fixed, sizeof(fixed), out, 4, &made, &rest) == INFLATE_OK && made == 4, "a byte short of room: OK, and four made");
    {
        size_t k;
        static const unsigned char two[] = { 0x00, 2, 0, 0xfd, 0xff, 'a', 'b', 0x4b, 0x04, 0x00 };   /* stored, then fixed 'a' */
        check(run_all(two, sizeof(two), out, sizeof(out), &made, &rest) == INFLATE_END && made == 3 && !memcmp(out, "aba", 3) && !rest,
              "a stored block, then a fixed one");
        for(k = 1; k < sizeof(two); k++) {
            check(run_chunks(two, k, 1, 1, out, sizeof(out), &made, &rest) == INFLATE_OK, "cut at each byte: never END");
        }
    }
}

/* A dynamic block's header: nlit + ndist lengths, each sent as itself. The code of the code lengths gives the sixteen
   lengths 0 to 15 four bits each, and the three repeats none, which is a whole code and all a test of a refusal needs. */
static void bw_dynamic_head(struct bw *w, int last, int nlit, int ndist, const unsigned char *lens) {
    static const unsigned char order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int i, n = nlit + ndist;
    bw_put(w, (unsigned int)last, 1);
    bw_put(w, 2, 2);
    bw_put(w, (unsigned int)(nlit - 257), 5);
    bw_put(w, (unsigned int)(ndist - 1), 5);
    bw_put(w, 19 - 4, 4);
    for(i = 0; i < 19; i++) bw_put(w, order[i] <= 15 ? 4 : 0, 3);       /* lengths 0-15: 16 codes of 4 bits */
    for(i = 0; i < n; i++) bw_code(w, lens[i], 4);
}

static void test_dynamic(void) {
    static struct bw w;
    static unsigned char lens[320], out[70000];
    size_t made, rest;
    int i;

    /* a block with literals 'a' and 'b', the end and a length code, two bits each, and one distance code of one bit */
    memset(lens, 0, sizeof(lens));
    lens['a'] = 2;
    lens['b'] = 2;
    lens[256] = 2;
    lens[257] = 2;                      /* length 3 */
    lens[286] = 1;                      /* distance symbol 0: one code, and one left unused */
    bw_start(&w);
    bw_dynamic_head(&w, 1, 286, 1, lens);
    bw_code(&w, 0, 2);                  /* 'a': the codes go by value: a=00 b=01 end=10 len3=11 */
    bw_code(&w, 1, 2);                  /* 'b' */
    bw_code(&w, 3, 2);                  /* length 3 */
    bw_code(&w, 0, 1);                  /* distance 1 */
    bw_code(&w, 2, 2);                  /* the end */
    bw_end(&w);
    check(run_all(w.b, w.n, out, sizeof(out), &made, &rest) == INFLATE_END && made == 5 && !memcmp(out, "abbbb", 5) && !rest,
          "a dynamic block with one distance code of one bit");

    /* the distance code's unused half */
    bw_start(&w);
    bw_dynamic_head(&w, 1, 286, 1, lens);
    bw_code(&w, 0, 2);
    bw_code(&w, 3, 2);
    bw_code(&w, 1, 1);
    for(i = 0; i < 40; i++) bw_code(&w, 1, 1);
    bw_end(&w);
    check(refused(w.b, w.n), "the unused half of a one-code distance code is no code");

    /* and that needs no more bits than the one: nothing that could follow would make it a code */
    bw_start(&w);
    bw_dynamic_head(&w, 1, 286, 1, lens);
    bw_code(&w, 0, 2);
    bw_code(&w, 3, 2);
    bw_code(&w, 1, 1);
    bw_end(&w);
    check(refused(w.b, w.n), "the unused half of a one-code code is refused at its first bit");

    /* no distance codes at all: literals are fine, a length is not */
    lens[286] = 0;
    bw_start(&w);
    bw_dynamic_head(&w, 1, 286, 1, lens);
    bw_code(&w, 0, 2);
    bw_code(&w, 2, 2);
    bw_end(&w);
    check(run_all(w.b, w.n, out, sizeof(out), &made, &rest) == INFLATE_END && made == 1 && out[0] == 'a',
          "no distance codes: a block of literals");
    bw_start(&w);
    bw_dynamic_head(&w, 1, 286, 1, lens);
    bw_code(&w, 0, 2);
    bw_code(&w, 3, 2);
    for(i = 0; i < 40; i++) bw_code(&w, 0, 1);
    bw_end(&w);
    check(refused(w.b, w.n), "no distance codes: a match has no distance");

    /* only the end, on one bit */
    memset(lens, 0, sizeof(lens));
    lens[256] = 1;
    bw_start(&w);
    bw_dynamic_head(&w, 1, 257, 1, lens);
    bw_code(&w, 0, 1);
    bw_end(&w);
    check(run_all(w.b, w.n, out, sizeof(out), &made, &rest) == INFLATE_END && !made, "a literal code of one symbol, the end");

    /* the refusals */
    memset(lens, 0, sizeof(lens));
    lens['a'] = 1;
    lens['b'] = 2;
    lens[256] = 2;
    bw_start(&w);
    bw_dynamic_head(&w, 1, 257, 1, lens);
    bw_end(&w);
    check(run_all(w.b, w.n, out, sizeof(out), &made, &rest) == INFLATE_OK, "a valid head: nothing wrong yet, the data is missing");
    lens['c'] = 1;
    bw_start(&w);
    bw_dynamic_head(&w, 1, 257, 1, lens);
    bw_end(&w);
    check(refused(w.b, w.n), "an over-full literal code");
    memset(lens, 0, sizeof(lens));
    lens['a'] = 2;
    lens[256] = 2;
    bw_start(&w);
    bw_dynamic_head(&w, 1, 257, 1, lens);
    bw_end(&w);
    check(refused(w.b, w.n), "an incomplete literal code");
    memset(lens, 0, sizeof(lens));
    lens['a'] = 1;
    lens['b'] = 1;
    bw_start(&w);
    bw_dynamic_head(&w, 1, 257, 1, lens);
    bw_end(&w);
    check(refused(w.b, w.n), "a literal code with no end");
    memset(lens, 0, sizeof(lens));
    lens['a'] = 1;
    lens[256] = 1;
    lens[257] = 1;                      /* distance symbol 0 */
    lens[258] = 1;
    lens[259] = 1;
    bw_start(&w);
    bw_dynamic_head(&w, 1, 257, 3, lens);
    bw_end(&w);
    check(refused(w.b, w.n), "an over-full distance code");
    memset(lens, 0, sizeof(lens));
    lens['a'] = 1;
    lens[256] = 1;
    lens[257] = 2;
    lens[258] = 2;
    bw_start(&w);
    bw_dynamic_head(&w, 1, 257, 2, lens);
    bw_end(&w);
    check(refused(w.b, w.n), "an incomplete distance code of two lengths");

    bw_start(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 2, 2);
    bw_put(&w, 30, 5);                  /* 287 literal codes */
    bw_put(&w, 0, 5);
    bw_put(&w, 0, 4);
    bw_end(&w);
    check(refused(w.b, w.n), "287 literal and length codes");
    bw_start(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 2, 2);
    bw_put(&w, 0, 5);
    bw_put(&w, 30, 5);                  /* 31 distance codes */
    bw_put(&w, 0, 4);
    bw_end(&w);
    check(refused(w.b, w.n), "31 distance codes");
    bw_start(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 2, 2);
    bw_put(&w, 0, 5);
    bw_put(&w, 0, 5);
    bw_put(&w, 15, 4);
    for(i = 0; i < 19; i++) bw_put(&w, 7, 3);
    bw_end(&w);
    check(refused(w.b, w.n), "an over-full code of the code lengths");
    bw_start(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 2, 2);
    bw_put(&w, 0, 5);
    bw_put(&w, 0, 5);
    bw_put(&w, 15, 4);
    for(i = 0; i < 19; i++) bw_put(&w, i == 0 ? 1 : 0, 3);
    bw_end(&w);
    check(refused(w.b, w.n), "an incomplete code of the code lengths: the one code that must be whole");
    bw_start(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 2, 2);
    bw_put(&w, 0, 5);
    bw_put(&w, 0, 5);
    bw_put(&w, 15, 4);
    for(i = 0; i < 19; i++) bw_put(&w, i == 4 ? 1 : 0, 3);              /* length 8 alone: with no more input, nothing else could refuse it */
    bw_end(&w);
    check(refused(w.b, w.n), "... and it is refused as soon as it is read, with nothing after it");
    bw_start(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 2, 2);
    bw_put(&w, 0, 5);
    bw_put(&w, 0, 5);
    bw_put(&w, 15, 4);
    for(i = 0; i < 19; i++) bw_put(&w, 0, 3);
    bw_end(&w);
    check(refused(w.b, w.n), "no code of the code lengths at all");

    /* repeats: 16, 17 and 18 only in the code of the code lengths, on two bits with 0 (four symbols) */
    {
        static const unsigned char order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
        /* symbols 0, 1, 16, 18 of 2 bits: by value 0=00 1=01 16=10 18=11 */
        bw_start(&w);
        bw_put(&w, 1, 1);
        bw_put(&w, 2, 2);
        bw_put(&w, 0, 5);
        bw_put(&w, 0, 5);
        bw_put(&w, 15, 4);
        for(i = 0; i < 19; i++) bw_put(&w, order[i] == 0 || order[i] == 1 || order[i] == 16 || order[i] == 18 ? 2 : 0, 3);
        bw_code(&w, 2, 2);              /* a repeat of what? */
        bw_put(&w, 0, 2);
        bw_end(&w);
        check(refused(w.b, w.n), "a repeat of the length before the first");
        bw_start(&w);
        bw_put(&w, 1, 1);
        bw_put(&w, 2, 2);
        bw_put(&w, 0, 5);
        bw_put(&w, 0, 5);
        bw_put(&w, 15, 4);
        for(i = 0; i < 19; i++) bw_put(&w, order[i] == 0 || order[i] == 1 || order[i] == 16 || order[i] == 18 ? 2 : 0, 3);
        bw_code(&w, 3, 2);              /* 138 zeros */
        bw_put(&w, 127, 7);
        bw_code(&w, 3, 2);              /* and 138 more */
        bw_put(&w, 127, 7);
        bw_code(&w, 3, 2);              /* past 258 */
        bw_put(&w, 0, 7);
        bw_end(&w);
        check(refused(w.b, w.n), "a repeat that runs past the code lengths' end");
    }

    /* fixed-code refusals */
    bw_start(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 1, 2);
    bw_fixed(&w, 286);
    bw_end(&w);
    check(refused(w.b, w.n), "fixed: length symbol 286");
    bw_start(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 1, 2);
    bw_fixed(&w, 287);
    bw_end(&w);
    check(refused(w.b, w.n), "fixed: length symbol 287");
    bw_start(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 1, 2);
    bw_fixed(&w, 'a');
    bw_fixed(&w, 257);
    bw_code(&w, 30, 5);
    bw_end(&w);
    check(refused(w.b, w.n), "fixed: distance symbol 30");
    bw_start(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 1, 2);
    bw_fixed(&w, 'a');
    bw_fixed(&w, 257);
    bw_code(&w, 31, 5);
    bw_end(&w);
    check(refused(w.b, w.n), "fixed: distance symbol 31");
    bw_start(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 1, 2);
    bw_fixed(&w, 257);
    bw_code(&w, 0, 5);
    bw_end(&w);
    check(refused(w.b, w.n), "a match before anything was made");
    bw_start(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 1, 2);
    bw_fixed(&w, 'a');
    bw_fixed(&w, 'b');
    bw_fixed(&w, 257);
    bw_code(&w, 2, 5);                  /* distance 3, with 2 bytes made */
    bw_end(&w);
    check(refused(w.b, w.n), "a distance one past the start");
    bw_start(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 1, 2);
    bw_fixed(&w, 'a');
    bw_fixed(&w, 'b');
    bw_fixed(&w, 257);
    bw_code(&w, 1, 5);                  /* distance 2, with 2 bytes made: fine */
    bw_fixed(&w, 256);
    bw_end(&w);
    check(run_all(w.b, w.n, out, sizeof(out), &made, &rest) == INFLATE_END && made == 5 && !memcmp(out, "ababa", 5),
          "a distance back to the very start, overlapping itself");

    bw_start(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 3, 2);
    bw_end(&w);
    check(refused(w.b, w.n), "a block of type 3");
    {
        static const unsigned char bad[] = { 0x01, 5, 0, 0xfa, 0xfe, 'h', 'e', 'l', 'l', 'o' };
        check(refused(bad, sizeof(bad)), "a stored block whose lengths disagree");
        check(iz->msg != NULL, "a refusal says why");
        check(inflate_run(iz) == INFLATE_BAD, "and goes on refusing");
    }

    /* the longest length and distance: both spellings of 258 */
    bw_start(&w);
    bw_put(&w, 0, 1);
    bw_put(&w, 0, 2);
    bw_end(&w);
    {
        unsigned int n = 32768;
        size_t k;
        bw_put(&w, n & 0xff, 8);
        bw_put(&w, n >> 8, 8);
        bw_put(&w, ~n & 0xff, 8);
        bw_put(&w, (~n >> 8) & 0xff, 8);
        for(k = 0; k < n; k++) w.b[w.n++] = (unsigned char)(k * 7);
    }
    bw_put(&w, 1, 1);
    bw_put(&w, 1, 2);
    bw_fixed(&w, 285);                  /* length 258, no extra bits */
    bw_code(&w, 29, 5);                 /* distance 24577 + 8191 = 32768, the farthest */
    bw_put(&w, 8191, 13);
    bw_fixed(&w, 284);                  /* length 227 + 31 = 258, the other way */
    bw_put(&w, 31, 5);
    bw_code(&w, 29, 5);
    bw_put(&w, 8191, 13);
    bw_fixed(&w, 256);
    bw_end(&w);
    {
        unsigned int k, good = 1;
        check(run_all(w.b, w.n, out, sizeof(out), &made, &rest) == INFLATE_END && made == 32768 + 516 && !rest,
              "distance 32768, the window's oldest byte, with both spellings of length 258");
        for(k = 0; k < 516; k++) if(out[32768 + k] != (unsigned char)(k * 7)) good = 0;
        check(good, "... and they copy what was there");
    }
}

/* The canonical code of section 3.2.2 of RFC 1951, worked out here, independently of inflate.c */
static void rfc_codes(const unsigned char *lens, int n, unsigned short *code) {
    unsigned short bl_count[16], next_code[17];
    unsigned int c = 0;
    int bits, i;
    memset(bl_count, 0, sizeof(bl_count));
    for(i = 0; i < n; i++) bl_count[lens[i]]++;
    bl_count[0] = 0;
    for(bits = 1; bits <= 15; bits++) {
        c = (c + bl_count[bits - 1]) << 1;
        next_code[bits] = (unsigned short)c;
    }
    for(i = 0; i < n; i++) if(lens[i]) code[i] = next_code[lens[i]]++;
}

/* The most a code can ask of the second level of inflate's table: 40 first-bits with codes 6 bits longer than the first
   level takes, which is all that 286 symbols can pay for, and every code of them used. */
static void test_largest_table(void) {
    static struct bw w;
    static unsigned char lens[320], out[4096], want[4096];
    static unsigned short code[286];
    static const unsigned char deep[7] = { 10, 11, 12, 13, 14, 15, 15 };
    size_t made, rest, n = 0;
    int s = 4, i, k;
    memset(lens, 0, sizeof(lens));
    lens[256] = 1;
    lens[0] = 2;
    lens[1] = 3;
    lens[2] = 5;
    lens[3] = 6;
    for(i = 0; i < 40; i++) {
        for(k = 0; k < 7; k++) {
            if(s == 256) s++;
            lens[s++] = deep[k];
        }
    }
    check(s == 285, "the table's test uses 285 symbols");
    lens[286] = 1;                                      /* one distance code */
    rfc_codes(lens, 286, code);
    bw_start(&w);
    bw_dynamic_head(&w, 1, 286, 1, lens);
    for(i = 0; i < 256; i++) {
        bw_code(&w, code[i], lens[i]);
        want[n++] = (unsigned char)i;
    }
    for(i = 257; i < 285; i++) {                        /* each length symbol, as a match of the last byte */
        int extra = i < 265 ? 0 : i < 285 ? (i - 261) / 4 : 0, base, q;
        static const unsigned short bases[28] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131,
                                                  163, 195, 227 };
        base = bases[i - 257];
        bw_code(&w, code[i], lens[i]);
        bw_put(&w, 0, extra);
        bw_code(&w, 0, 1);
        for(q = 0; q < base; q++) {
            want[n] = want[n - 1];
            n++;
        }
    }
    bw_code(&w, code[256], lens[256]);
    bw_end(&w);
    check(run_all(w.b, w.n, out, sizeof(out), &made, &rest) == INFLATE_END && made == n && !memcmp(out, want, n) && !rest,
          "the largest table a stream can ask for: every code of it decodes");
    check(run_chunks(w.b, w.n, 1, 1, out, sizeof(out), &made, &rest) == INFLATE_END && made == n && !memcmp(out, want, n), "and a bit at a time, as it were");
}

/* A hostile stream too short for inflate's fast loop never meets the fast loop's own checks: these have the input and room it
   wants, so the same refusals are made there, and what it takes ahead is given back. */
static void test_fast_loop(void) {
    static struct bw w;
    static unsigned char out[4096];
    size_t made, rest;
    int i;

    bw_start(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 1, 2);
    bw_fixed(&w, 'a');
    bw_fixed(&w, 'b');
    bw_fixed(&w, 257);
    bw_code(&w, 2, 5);                  /* distance 3, with 2 bytes made */
    for(i = 0; i < 60; i++) bw_fixed(&w, 'c');
    bw_fixed(&w, 256);
    bw_end(&w);
    check(run_all(w.b, w.n, out, sizeof(out), &made, &rest) == INFLATE_BAD && made == 2, "a distance past the start, in the middle of a long stream");
    check(iz->msg != NULL && inflate_run(iz) == INFLATE_BAD, "... refused, and it goes on refusing");
    bw_start(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 1, 2);
    bw_fixed(&w, 'a');
    bw_fixed(&w, 257);
    bw_code(&w, 30, 5);                 /* a distance symbol that does not exist */
    for(i = 0; i < 60; i++) bw_fixed(&w, 'c');
    bw_fixed(&w, 256);
    bw_end(&w);
    check(run_all(w.b, w.n, out, sizeof(out), &made, &rest) == INFLATE_BAD && made == 1, "distance symbol 30, in the middle of a long stream");
    bw_start(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 1, 2);
    bw_fixed(&w, 'a');
    bw_fixed(&w, 286);                  /* a length symbol that does not exist */
    for(i = 0; i < 60; i++) bw_fixed(&w, 'c');
    bw_fixed(&w, 256);
    bw_end(&w);
    check(run_all(w.b, w.n, out, sizeof(out), &made, &rest) == INFLATE_BAD && made == 1, "length symbol 286, in the middle of a long stream");

    /* the last block's end, met with input to spare: what follows is left, to the byte */
    bw_start(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 1, 2);
    for(i = 0; i < 60; i++) bw_fixed(&w, 'c');
    bw_fixed(&w, 256);
    bw_end(&w);
    for(i = 0; i < 64; i++) w.b[w.n++] = 'T';
    check(run_all(w.b, w.n, out, sizeof(out), &made, &rest) == INFLATE_END && made == 60 && rest == 64, "with 64 bytes after the stream, not one of them is read");
    /* a block's end, and a stored block after it, whose lengths must be found exactly on the next byte */
    bw_start(&w);
    bw_put(&w, 0, 1);
    bw_put(&w, 1, 2);
    for(i = 0; i < 60; i++) bw_fixed(&w, 'c');
    bw_fixed(&w, 256);
    bw_put(&w, 1, 1);
    bw_put(&w, 0, 2);
    bw_end(&w);
    bw_put(&w, 20, 8);
    bw_put(&w, 0, 8);
    bw_put(&w, 235, 8);
    bw_put(&w, 255, 8);
    for(i = 0; i < 20; i++) bw_put(&w, 'd', 8);
    for(i = 0; i < 64; i++) w.b[w.n++] = 'T';
    check(run_all(w.b, w.n, out, sizeof(out), &made, &rest) == INFLATE_END && made == 80 && out[59] == 'c' && out[60] == 'd' && rest == 64,
          "a stored block after a block the fast loop ended");
}

/* every way of cutting input and room over a stream with stored, fixed and dynamic blocks */
static void test_chunks(void) {
    static unsigned char plain[60000], packed[70000], back[60000], with_tail[70003];
    size_t i, n, made, rest;
    size_t sizes[] = { 1, 2, 3, 7, 255, 4096, 65536 };
    unsigned a, b;
    int r;
    for(i = 0; i < sizeof(plain); i++) plain[i] = (i < 20000) ? (unsigned char)("the quick brown fox "[i % 20]) : (i < 40000) ? (unsigned char)rnd() : (unsigned char)(i >> 6);
    deflate_init(dz, 6);
    dz->in = plain;
    dz->in_n = sizeof(plain);
    dz->out = packed;
    dz->out_n = sizeof(packed);
    r = deflate_run(dz, 1);
    check(r == DEFLATE_END, "the stream for the chunk tests is made");
    n = sizeof(packed) - dz->out_n;
    memcpy(with_tail, packed, n);
    memcpy(with_tail + n, "END", 3);
    for(a = 0; a < sizeof(sizes) / sizeof(sizes[0]); a++) {
        for(b = 0; b < sizeof(sizes) / sizeof(sizes[0]); b++) {
            memset(back, 0, sizeof(back));
            r = run_chunks(with_tail, n + 3, sizes[a], sizes[b], back, sizeof(back), &made, &rest);
            check(r == INFLATE_END && made == sizeof(plain) && rest == 3 && !memcmp(back, plain, sizeof(plain)),
                  "chunks of input and room: the same bytes, and the three after the stream unread");
        }
    }
    /* cut short: at each length (from the sparse sample of a long stream) it wants more, and it never says END */
    for(i = 0; i < n; i += 1 + i / 50) {
        r = run_chunks(packed, i, 4096, 65536, back, sizeof(back), &made, &rest);
        if(r != INFLATE_OK) {
            check(0, "a stream cut short must want more");
            break;
        }
        if(memcmp(back, plain, made)) {
            check(0, "what was made of a stream cut short is the data's beginning");
            break;
        }
    }
    check(1, "cut short at each length: more is wanted, and what came out is the data's beginning");
}

/**** deflate ****/

/* deflate in chunks, inflate in chunks, compare; the packed size */
static size_t round_trip(const unsigned char *plain, size_t n, int level, size_t ichunk, size_t ochunk) {
    static unsigned char *packed, *back;
    size_t fed = 0, got = 0, cap = n + n / 8 + 4096, made, rest;
    int r;
    packed = realloc(packed, cap);
    back = realloc(back, n + 1);
    if(!packed || !back) exit(2);
    deflate_init(dz, level);
    for(;;) {
        size_t i = n - fed < ichunk ? n - fed : ichunk, o = cap - got < ochunk ? cap - got : ochunk;
        dz->in = plain + fed;
        dz->in_n = i;
        dz->out = packed + got;
        dz->out_n = o;
        r = deflate_run(dz, fed + i == n);
        fed += i - dz->in_n;
        got += o - dz->out_n;
        if(r == DEFLATE_END) break;
        if(got == cap) {
            check(0, "the packed form is larger than its bound");
            return 0;
        }
    }
    r = run_chunks(packed, got, ichunk ? 777 : 1, 1024, back, n + 1, &made, &rest);
    if(r != INFLATE_END || made != n || rest != 0 || (n && memcmp(back, plain, n))) {
        char msg[120];
        snprintf(msg, sizeof(msg), "round trip of %u bytes at level %d, chunks %u/%u: %d, %u made", (unsigned)n, level,
                 (unsigned)ichunk, (unsigned)ochunk, r, (unsigned)made);
        check(0, msg);
    }
    checks++;
    return got;
}

static void test_deflate(void) {
    static unsigned char d[600000];
    size_t i, sizes[6];
    int level, s;
    size_t last, small;
    /* the small sizes at every level */
    for(i = 0; i < sizeof(d); i++) d[i] = (unsigned char)"abcabcabd"[(i * i >> 3) % 9];
    for(level = 1; level <= 9; level++) {
        size_t n;
        for(n = 0; n < 40; n++) round_trip(d, n, level, 65536, 65536);
        round_trip(d, 300, level, 1, 1);
    }
    /* runs of one byte, around each size where something changes */
    memset(d, 'x', sizeof(d));
    for(level = 1; level <= 9; level++) {
        round_trip(d, 258, level, 65536, 65536);
        round_trip(d, 259, level, 65536, 65536);
        round_trip(d, 262, level, 65536, 65536);
        round_trip(d, 32768, level, 65536, 65536);
        round_trip(d, 65535, level, 65536, 65536);
        round_trip(d, 65536, level, 65536, 65536);
        round_trip(d, 65537, level, 65536, 65536);
    }
    small = round_trip(d, sizeof(d), 9, 65536, 65536);
    check(small < 1500, "600 KB of one byte packs to almost nothing");
    /* periods: each match has one distance */
    for(s = 1; s < 300; s += 7) {
        for(i = 0; i < 20000; i++) d[i] = (unsigned char)(i % (size_t)s * 31);
        round_trip(d, 20000, 6, 4096, 4096);
    }
    /* random bytes: stored blocks, and little growth */
    for(i = 0; i < sizeof(d); i++) d[i] = (unsigned char)rnd();
    for(level = 1; level <= 9; level++) {
        size_t got = round_trip(d, sizeof(d), level, 65536, 65536);
        check(got < sizeof(d) + sizeof(d) / 1000, "random bytes barely grow");
    }
    round_trip(d, sizeof(d), 6, 1000, 1000);
    /* a block that starts packable and turns random, and back, every few KB */
    for(i = 0; i < sizeof(d); i++) d[i] = ((i >> 13) & 1) ? (unsigned char)rnd() : (unsigned char)(i >> 4);
    for(level = 1; level <= 9; level++) round_trip(d, sizeof(d), level, 65536, 65536);
    /* the farthest matches: a random block repeated at 32760 bytes, so every match sits at the window's edge */
    for(i = 0; i < 32760; i++) d[i] = (unsigned char)rnd();
    for(i = 32760; i < sizeof(d); i++) d[i] = d[i - 32760];
    last = 0;
    for(level = 1; level <= 9; level++) {
        size_t got = round_trip(d, sizeof(d), level, 65536, 65536);
        if(level == 9) last = got;
    }
    check(last < 40000 + sizeof(d) / 20, "a 32760-byte period is found at the far edge of the window");
    /* the worst symbols: 3-byte matches at long distances, then long matches at long distances, to fill pending */
    for(i = 0; i < sizeof(d); i++) d[i] = (unsigned char)rnd();
    for(i = 40000; i + 300 < sizeof(d); i += 6) {
        size_t from = i - 4000 - rnd() % 28000;
        d[i] = d[from];
        d[i + 1] = d[from + 1];
        d[i + 2] = d[from + 2];
    }
    for(level = 1; level <= 9; level++) round_trip(d, sizeof(d), level, 65536, 65536);
    for(i = 40000; i + 300 < sizeof(d); i += 200) memcpy(d + i, d + i - 30000 - rnd() % 2000, 190);
    for(level = 1; level <= 9; level++) round_trip(d, sizeof(d), level, 65536, 65536);
    /* every level's size order: 9 no worse than 1 on text-like data */
    for(i = 0; i < sizeof(d); i++) d[i] = (unsigned char)("etaoin shrdlu cmfwyp "[rnd() % 21 % (1 + rnd() % 21)]);
    sizes[0] = round_trip(d, sizeof(d), 1, 65536, 65536);
    sizes[1] = round_trip(d, sizeof(d), 6, 65536, 65536);
    sizes[2] = round_trip(d, sizeof(d), 9, 65536, 65536);
    check(sizes[2] <= sizes[1] && sizes[1] <= sizes[0], "level 9 is no larger than 6, nor 6 than 1");
}

/**** hostile streams ****/

/* one run over bytes that are not to be believed: it must end, and stay inside its buffers */
static void fuzz_one(const unsigned char *in, size_t n, size_t room) {
    unsigned char *copy = malloc(n ? n : 1), *out = malloc(room);
    size_t fed = 0, total = 0;
    int r, turns = 0;
    if(!copy || !out) exit(2);
    memcpy(copy, in, n);
    inflate_init(iz);
    for(;;) {
        size_t i = n - fed < 700 ? n - fed : 700, before;
        iz->in = copy + fed;
        iz->in_n = i;
        iz->out = out;
        iz->out_n = room;
        r = inflate_run(iz);
        before = iz->in_n;
        fed += i - before;
        total += room - iz->out_n;
        if(r != INFLATE_OK) break;
        if(i == before && iz->out_n == room) break;
        if(total > (size_t)64 << 20) break;             /* a bomb: the caller stops it, as here */
        if(++turns > 100000) {
            check(0, "inflate does not end");
            break;
        }
    }
    free(copy);
    free(out);
}

static void test_hostile(void) {
    static unsigned char plain[20000], packed[32768], m[32768];
    size_t n;
    int round, k;
    for(n = 0; n < sizeof(plain); n++) plain[n] = (n >> 7) & 1 ? (unsigned char)rnd() : (unsigned char)"to be or not to be "[n % 19];
    for(k = 1; k <= 9; k += 4) {
        deflate_init(dz, k);
        dz->in = plain;
        dz->in_n = sizeof(plain);
        dz->out = packed;
        dz->out_n = sizeof(packed);
        deflate_run(dz, 1);
        n = sizeof(packed) - dz->out_n;
        for(round = 0; round < 3000; round++) {
            int flips = 1 + (int)(rnd() % 8), f;
            memcpy(m, packed, n);
            for(f = 0; f < flips; f++) m[rnd() % n] ^= (unsigned char)(1u << (rnd() % 8));
            fuzz_one(m, n, 1 + rnd() % 4096);
        }
    }
    for(round = 0; round < 3000; round++) {             /* no structure at all */
        size_t len = 1 + rnd() % sizeof(m), i;
        for(i = 0; i < len; i++) m[i] = (unsigned char)rnd();
        m[0] = (unsigned char)(m[0] & ~7u) | (unsigned char)(round & 7);        /* each block type gets its turn */
        fuzz_one(m, len, 1 + rnd() % 4096);
    }
    check(1, "9000 mutated and 3000 random streams ended, inside their buffers");
    /* a bomb: fixed blocks of matches, a huge output from little input, and out holds the line */
    {
        static struct bw w;
        size_t i, made, rest;
        static unsigned char room[4096];
        int r;
        bw_start(&w);
        bw_put(&w, 1, 1);
        bw_put(&w, 1, 2);
        bw_fixed(&w, 'x');
        for(i = 0; i < 40000; i++) {
            bw_fixed(&w, 285);
            bw_code(&w, 0, 5);
        }
        bw_fixed(&w, 256);
        bw_end(&w);
        r = run_all(w.b, w.n, room, sizeof(room), &made, &rest);
        check(r == INFLATE_OK && made == sizeof(room) && rest > 0, "a stream of 10 MB from 26 KB stops where out ends");
    }
}

int main(void) {
    iz = malloc(sizeof(*iz));
    dz = malloc(sizeof(*dz));
    if(!iz || !dz) return 2;
    test_crc();
    test_known();
    test_dynamic();
    test_largest_table();
    test_fast_loop();
    test_chunks();
    test_deflate();
    test_hostile();
    free(iz);
    free(dz);
    printf("test_deflate: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
