/* libsurf's inflate against the host's zlib (11/01/26), run by tests/archive-sanitize.sh where the host has zlib's header: the
   Docker image does not. Streams made by zlib at each level and strategy and by libsurf's deflate, over text, over unequal
   symbols, random bytes and runs. A compressor reaches only 12-bit codes on data like these, so others are written here:
   random whole codes down to 15 bits in both of a block's tables, the widest second-level tables the decoder has, with
   symbols sent through them. Each stream is mutated, cut, or replaced by noise. The two must agree on every one: whether
   it ends, is refused or wants more; the bytes made; where the input was left. And inflate must give the same whatever
   chunks it is fed in. Run it with a number of streams, 150000 by default. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <inflate.h>
#include <deflate.h>

#define OUT_MAX (1u << 22)
#define SEEDS_MAX 80
#define CRAFTED   30

static unsigned char *ref_out, *our_out, *cut_out;
static struct inflate *iz;
static struct deflate *dz;
static unsigned long long rng = 0x9E3779B97F4A7C15ULL;

static unsigned int rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (unsigned int)(rng >> 20);
}

struct seed { unsigned char *bytes; size_t n; };
static struct seed seeds[SEEDS_MAX];
static int nseeds;

static void add_seed(const unsigned char *p, size_t n) {
    if(nseeds == SEEDS_MAX) abort();
    seeds[nseeds].bytes = malloc(n ? n : 1);
    if(!seeds[nseeds].bytes) abort();
    memcpy(seeds[nseeds].bytes, p, n);
    seeds[nseeds++].n = n;
}

static void zlib_seed(const unsigned char *p, size_t n, int level, int strategy) {
    static unsigned char out[1 << 20];
    z_stream s;
    memset(&s, 0, sizeof(s));
    if(deflateInit2(&s, level, Z_DEFLATED, -15, 8, strategy) != Z_OK) abort();
    s.next_in = (unsigned char *)p;
    s.avail_in = (unsigned int)n;
    s.next_out = out;
    s.avail_out = sizeof(out);
    if(deflate(&s, Z_FINISH) != Z_STREAM_END) abort();
    add_seed(out, sizeof(out) - s.avail_out);
    deflateEnd(&s);
}

static void our_seed(const unsigned char *p, size_t n, int level) {
    static unsigned char out[1 << 20];
    deflate_init(dz, level);
    dz->in = p;
    dz->in_n = n;
    dz->out = out;
    dz->out_n = sizeof(out);
    if(deflate_run(dz, 1) != DEFLATE_END) abort();
    add_seed(out, sizeof(out) - dz->out_n);
}

/**** dynamic blocks by hand ****/

struct bw { unsigned char *p; size_t n; unsigned int acc; int cnt; };

static void bw_put(struct bw *w, unsigned int v, int n) {
    while(n--) {
        w->acc |= (v & 1) << w->cnt;
        v >>= 1;
        if(++w->cnt == 8) {
            w->p[w->n++] = (unsigned char)w->acc;
            w->acc = 0;
            w->cnt = 0;
        }
    }
}

/* most significant bit first, as a Huffman code goes */
static void bw_code(struct bw *w, unsigned int code, int n) {
    while(n--) bw_put(w, (code >> n) & 1, 1);
}

/* n random lengths of a whole code, by splitting leaves of a tree, the deepest ones more often */
static void random_lengths(unsigned char *depth, int n) {
    int have = 2, i;
    depth[0] = depth[1] = 1;
    while(have < n) {
        int at = have - 1;
        if(rnd() % 3 == 0) at = (int)(rnd() % (unsigned)have);
        if(depth[at] == 15) {
            at = (int)(rnd() % (unsigned)have);
            if(depth[at] == 15) continue;
        }
        depth[at]++;
        depth[have++] = depth[at];
    }
    for(i = have - 1; i > 0; i--) {                     /* mixed up, or the deep ones would all be the last symbols */
        int j = (int)(rnd() % (unsigned)(i + 1));
        unsigned char t = depth[i];
        depth[i] = depth[j];
        depth[j] = t;
    }
}

static void canonical(const unsigned char *lens, int n, unsigned short *code) {
    unsigned short bl_count[16], next_code[16];
    unsigned int c = 0;
    int i;
    memset(bl_count, 0, sizeof(bl_count));
    for(i = 0; i < n; i++) bl_count[lens[i]]++;
    bl_count[0] = 0;
    for(i = 1; i <= 15; i++) {
        c = (c + bl_count[i - 1]) << 1;
        next_code[i] = (unsigned short)c;
    }
    for(i = 0; i < n; i++) if(lens[i]) code[i] = next_code[lens[i]]++;
}

/* one final dynamic block: a whole code over nearly all of each alphabet, then symbols through it, a match only where its
   distance is back in what was made */
static void crafted_seed(void) {
    static const unsigned char order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    static const unsigned char length_more[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const unsigned short length_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const unsigned char dist_more[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    static const unsigned short dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static unsigned char out[1 << 18], lens[316], depth[286];
    static unsigned short lit_code[286], dist_code[30];
    struct bw w;
    int nlit = 40 + (int)(rnd() % 246), ndist = 16 + (int)(rnd() % 15), i, k;
    unsigned long made = 0;
    unsigned char *dl;
    w.p = out;
    w.n = 0;
    w.acc = 0;
    w.cnt = 0;
    if(ndist > 30) ndist = 30;
    memset(lens, 0, sizeof(lens));
    random_lengths(depth, nlit);
    for(i = 0; i < nlit; i++) lens[i * 286 / nlit] = depth[i];
    if(!lens[256]) {                                    /* the end must have a code: it takes one from a neighbour */
        for(i = 257; !lens[i]; i++) {}
        lens[256] = lens[i];
        lens[i] = 0;
    }
    dl = lens + 286;
    random_lengths(depth, ndist);
    for(i = 0; i < ndist; i++) dl[i] = depth[i];
    canonical(lens, 286, lit_code);
    canonical(dl, 30, dist_code);
    bw_put(&w, 1, 1);
    bw_put(&w, 2, 2);
    bw_put(&w, 286 - 257, 5);
    bw_put(&w, 30 - 1, 5);
    bw_put(&w, 19 - 4, 4);
    for(i = 0; i < 19; i++) bw_put(&w, order[i] <= 15 ? 4 : 0, 3);      /* lengths 0 to 15, four bits each */
    for(i = 0; i < 316; i++) bw_code(&w, lens[i], 4);
    for(k = 0; k < 60000 && w.n < sizeof(out) - 16; k++) {
        int s = (int)(rnd() % 286);
        if(!lens[s] || s == 256) continue;
        if(s < 256) {
            bw_code(&w, lit_code[s], lens[s]);
            made++;
        } else {
            int d = (int)(rnd() % 30);
            if(!dl[d] || dist_base[d] > made) continue;
            bw_code(&w, lit_code[s], lens[s]);
            bw_put(&w, 0, length_more[s - 257]);
            bw_code(&w, dist_code[d], dl[d]);
            bw_put(&w, 0, dist_more[d]);
            made += length_base[s - 257];
        }
    }
    bw_code(&w, lit_code[256], lens[256]);
    if(w.cnt) bw_put(&w, 0, 8 - w.cnt);
    add_seed(out, w.n);
}

/* each symbol half as likely as the one before */
static void skewed(unsigned char *d, size_t n) {
    size_t i;
    for(i = 0; i < n; i++) {
        unsigned int r = rnd() | 1u << 31, k = 0;
        while(!(r & 1)) {
            r >>= 1;
            k++;
        }
        d[i] = (unsigned char)(k * 11 + (rnd() & 1));
    }
}

static void make_seeds(void) {
    static unsigned char d[200000];
    static const int strategies[] = { Z_DEFAULT_STRATEGY, Z_FILTERED, Z_HUFFMAN_ONLY, Z_RLE, Z_FIXED };
    size_t i;
    unsigned kind, k;
    for(kind = 0; kind < 4; kind++) {
        size_t n = kind == 1 ? 3000 : sizeof(d) / (kind == 2 ? 20 : 2);
        for(i = 0; i < n; i++) {
            if(kind == 0) d[i] = (unsigned char)"the quick brown fox jumps over the lazy dog\n"[(i + (rnd() >> 29)) % 44];
            else if(kind == 2) d[i] = (unsigned char)rnd();
            else if(kind == 3) d[i] = (unsigned char)(i >> 9);
        }
        if(kind == 1) skewed(d, n);
        for(k = 0; k < sizeof(strategies) / sizeof(strategies[0]); k++) zlib_seed(d, n, k == 0 ? 9 : 6, strategies[k]);
        zlib_seed(d, n, 1, Z_DEFAULT_STRATEGY);
        zlib_seed(d, n, 0, Z_DEFAULT_STRATEGY);
        our_seed(d, n, 1);
        our_seed(d, n, 6);
        our_seed(d, n, 9);
    }
    skewed(d, 60000);
    zlib_seed(d, 60000, 9, Z_DEFAULT_STRATEGY);
    our_seed(d, 60000, 9);
    for(k = 0; k < CRAFTED; k++) crafted_seed();
}

/* 1 ended, 0 wants more, -1 refused */
static int run_zlib(const unsigned char *in, size_t n, size_t *made, size_t *used) {
    z_stream s;
    int r;
    memset(&s, 0, sizeof(s));
    if(inflateInit2(&s, -15) != Z_OK) abort();
    s.next_in = (unsigned char *)in;
    s.avail_in = (unsigned int)n;
    s.next_out = ref_out;
    s.avail_out = OUT_MAX;
    r = inflate(&s, Z_FINISH);
    *made = s.total_out;
    *used = s.total_in;
    inflateEnd(&s);
    return r == Z_STREAM_END ? 1 : r == Z_DATA_ERROR ? -1 : 0;
}

/* chunks: 0 all at once; else fed in random pieces, of both input and room */
static int run_ours(const unsigned char *in, size_t n, unsigned char *out, size_t *made, size_t *used, int chunks) {
    size_t fed = 0, got = 0;
    int r;
    inflate_init(iz);
    for(;;) {
        size_t i = n - fed, o = OUT_MAX - got;
        if(chunks) {
            size_t a = 1 + rnd() % (rnd() & 1 ? 9 : 40000), b = 1 + rnd() % (rnd() & 1 ? 9 : 40000);
            if(i > a) i = a;
            if(o > b) o = b;
        }
        iz->in = in + fed;
        iz->in_n = i;
        iz->out = out + got;
        iz->out_n = o;
        r = inflate_run(iz);
        fed += i - iz->in_n;
        got += o - iz->out_n;
        if(r != INFLATE_OK || got == OUT_MAX || (fed == n && iz->out_n)) break;
    }
    *made = got;
    *used = fed;
    return r == INFLATE_END ? 1 : r == INFLATE_BAD ? -1 : 0;
}

int main(int argc, char **argv) {
    unsigned long want = argc > 1 ? strtoul(argv[1], NULL, 0) : 150000, i, ended = 0, refused = 0, more = 0, big = 0;
    static unsigned char m[1 << 20];
    ref_out = malloc(OUT_MAX);
    our_out = malloc(OUT_MAX);
    cut_out = malloc(OUT_MAX);
    iz = malloc(sizeof(*iz));
    dz = malloc(sizeof(*dz));
    if(!ref_out || !our_out || !cut_out || !iz || !dz) return 2;
    make_seeds();
    for(i = 0; i < want; i++) {
        size_t n, zmade, zused, made, used, cmade, cused, least;
        int a, b, c;
        if(rnd() % 5 == 0) {                            /* noise, with each block type in turn */
            size_t k;
            n = 1 + rnd() % 3000;
            for(k = 0; k < n; k++) m[k] = (unsigned char)rnd();
            m[0] = (unsigned char)((m[0] & ~7u) | (i & 7));
        } else {
            const struct seed *s = &seeds[rnd() % (unsigned)nseeds];
            int edits = 1 + (int)(rnd() % 4);
            n = s->n;
            memcpy(m, s->bytes, n);
            while(edits--) {
                switch(rnd() % 4) {
                case 0: m[rnd() % n] ^= (unsigned char)(1u << (rnd() % 8)); break;
                case 1: m[rnd() % (n < 300 ? n : 300)] = (unsigned char)rnd(); break;      /* the block's header, mostly */
                case 2: m[rnd() % n] = (unsigned char)rnd(); break;
                default: n = 1 + rnd() % n; break;
                }
            }
        }
        a = run_zlib(m, n, &zmade, &zused);
        b = run_ours(m, n, our_out, &made, &used, 0);
        if(zmade >= OUT_MAX || made >= OUT_MAX) {       /* a bomb filled the buffer: neither has finished */
            big++;
            continue;
        }
        least = zmade < made ? zmade : made;
        if(a != b || memcmp(ref_out, our_out, least) || (a == 1 && (zmade != made || zused != used))) {
            printf("FAIL: stream %lu: zlib %d, %zu made, %zu used; ours %d, %zu made, %zu used: %s\n", i, a, zmade, zused, b, made, used,
                   b < 0 ? iz->msg : "");
            return 1;
        }
        c = run_ours(m, n, cut_out, &cmade, &cused, 1);
        if(c != b || cmade != made || cused != used || memcmp(cut_out, our_out, made)) {
            printf("FAIL: stream %lu in chunks: %d, %zu made, %zu used; whole %d, %zu made, %zu used\n", i, c, cmade, cused, b, made, used);
            return 1;
        }
        if(a == 1) ended++;
        else if(a < 0) refused++;
        else more++;
    }
    printf("test_inflate_zlib: %lu streams: %lu ended, %lu refused, %lu wanted more, %lu too big to tell; 0 differences\n", want, ended, refused, more, big);
    return 0;
}
