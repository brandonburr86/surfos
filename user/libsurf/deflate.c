/*
SurfOS user library: deflate
--------------------
File: deflate.c Date: 11/01/26
--------------------
Input goes into a window of 64 KB, of which the lower half is history once the upper half has been
reached. Each position is put on a hash chain by its next 3 bytes, and a match is sought along that
chain, as far as the level says; from level 4 a match is held back for one byte in case the next
position has a longer one. Literals and matches are counted up as symbols until a block has
16383 of them; only then are codes made, because only then are the frequencies known.

The codes come from package-merge, which gives the best code of a limited length directly: no
tree, no fixing up of a code that came out too long. Its lists show which symbols go where without
any chain of pointers: in each list the leaves that are used are the first ones by frequency, so
it is enough to know how many leaves there are among a list's first items.

A block goes out in whichever of the three forms is smallest. It is written whole into pending,
which is drained into out, so the coding never has to stop for lack of room. Pending is big enough
because the form chosen is never larger than the fixed one, and a fixed symbol is at most 31 bits
(8 + 5 length bits, 5 + 13 distance bits), so a block of 16384 symbols is at most 63.5 KB; a stored
block is chosen only when that is smaller still.

A stored block needs its input still in the window. A block whose beginning has slid out is one
of many long matches, for which stored is the worst form anyway.
*/

#include <string.h>
#include <stdint.h>
#include <deflate.h>

#define WSIZE     DEFLATE_WSIZE
#define WMASK     (WSIZE - 1)
#define MIN_MATCH 3
#define MAX_MATCH 258
#define LOOKAHEAD (MAX_MATCH + 4)       /* a match is sought with this much ahead, or at the input's end */
#define MAX_DIST  (WSIZE - 1)           /* so that a distance fits in 16 bits, with 0 for a literal */
#define TOO_FAR   4096                  /* a 3-byte match further back costs more than three literals */
#define HASH_MUL  0x9E3779B1u
#define MAX_BITS  15
#define LENGTHS_LIMIT 7                 /* the code of the code lengths is limited to 7 bits */
#define LENGTH_SYMBOLS 19

static const struct { unsigned short chain, lazy, nice, good; } levels[10] = {
    { 0, 0, 0, 0 },                     /* no level 0: nothing here stores without trying */
    { 4, 0, 16, 4 },
    { 8, 0, 32, 4 },
    { 32, 0, 64, 8 },
    { 16, 4, 32, 4 },
    { 32, 8, 32, 8 },
    { 128, 16, 128, 8 },
    { 256, 32, 258, 16 },
    { 1024, 128, 258, 32 },
    { 4096, 258, 258, 32 },
};

static const unsigned short length_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                                67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const unsigned char length_more[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
                                               5, 5, 5, 5, 0 };
static const unsigned short dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
                                              769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const unsigned char dist_more[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
                                             11, 11, 12, 12, 13, 13 };
static const unsigned char length_order[LENGTH_SYMBOLS] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

/**** the bits ****/

static void put_bits(struct deflate *z, unsigned int v, int n) {
    z->bitbuf |= v << z->bitcnt;
    z->bitcnt += n;
    while(z->bitcnt >= 8) {
        z->pending[z->pend_n++] = (unsigned char)z->bitbuf;
        z->bitbuf >>= 8;
        z->bitcnt -= 8;
    }
}

/* the partial byte, filled with zeros */
static void align(struct deflate *z) {
    if(z->bitcnt) put_bits(z, 0, 8 - z->bitcnt);
}

static unsigned int reverse(unsigned int v, int n) {
    unsigned int r = 0;
    while(n--) {
        r = r << 1 | (v & 1);
        v >>= 1;
    }
    return r;
}

/* the last of the n ascending values in base that is at most v */
static int find_base(const unsigned short *base, int n, unsigned int v) {
    int lo = 0, hi = n - 1;
    while(lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if(base[mid] <= v) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

/**** the codes ****/

/* The lengths of the cheapest code of at most limit bits for n symbols (n at most DEFLATE_ALPHABET). Two symbols
   get codes even when fewer are used: a code with one symbol is left incomplete, which some decoders refuse, and
   the code of the code lengths must never be. */
static void code_lengths(const unsigned short *freq, int n, int limit, unsigned char *len) {
    unsigned short sorted[DEFLATE_ALPHABET];
    unsigned short leaves[MAX_BITS][2 * DEFLATE_ALPHABET + 1];  /* of a list's first t items, how many are leaves */
    uint32_t list[2 * DEFLATE_ALPHABET], last[2 * DEFLATE_ALPHABET];
    int m = 0, i, level, size, t;
    for(i = 0; i < n; i++) {
        len[i] = 0;
        if(freq[i]) {
            int k = m++;
            for(; k > 0 && freq[sorted[k - 1]] > freq[i]; k--) sorted[k] = sorted[k - 1];
            sorted[k] = (unsigned short)i;
        }
    }
    if(m < 2) {
        int a = m ? sorted[0] : 0;
        len[a] = 1;
        len[a ? 0 : 1] = 1;
        return;
    }
    size = m;
    for(i = 0; i < m; i++) {
        last[i] = freq[sorted[i]];
        leaves[0][i] = (unsigned short)i;
    }
    leaves[0][m] = (unsigned short)m;
    for(level = 1; level < limit; level++) {
        int packs = size / 2, a = 0, b = 0;
        t = 0;
        leaves[level][0] = 0;
        while(a < m || b < packs) {
            uint32_t leaf = a < m ? freq[sorted[a]] : 0, pack = b < packs ? last[2 * b] + last[2 * b + 1] : 0;
            if(b == packs || (a < m && leaf <= pack)) {
                list[t] = leaf;
                a++;
            } else {
                list[t] = pack;
                b++;
            }
            t++;
            leaves[level][t] = (unsigned short)a;
        }
        size = t;
        memcpy(last, list, (size_t)size * sizeof(last[0]));
    }
    t = 2 * m - 2;
    for(level = limit - 1; level >= 0; level--) {
        int c = leaves[level][t];
        for(i = 0; i < c; i++) len[sorted[i]]++;
        t = (t - c) * 2;
    }
}

/* the canonical code of these lengths, its bits reversed for put_bits() */
static void make_codes(const unsigned char *len, int n, unsigned short *code) {
    unsigned short count[MAX_BITS + 1], next[MAX_BITS + 2];
    unsigned int c = 0;
    int i;
    memset(count, 0, sizeof(count));
    for(i = 0; i < n; i++) count[len[i]]++;
    count[0] = 0;
    for(i = 1; i <= MAX_BITS; i++) {
        c = (c + count[i - 1]) << 1;
        next[i] = (unsigned short)c;
    }
    for(i = 0; i < n; i++) if(len[i]) code[i] = (unsigned short)reverse(next[len[i]]++, len[i]);
}

/* The code lengths of a block as the symbols of RFC 1951's section 3.2.7: their count. A run may go from the
   literal lengths into the distance lengths: the format says they are one sequence. */
static int run_lengths(const unsigned char *lens, int n, unsigned char *sym, unsigned char *more) {
    int i = 0, k = 0;
    while(i < n) {
        int v = lens[i], run = 1;
        while(i + run < n && lens[i + run] == v) run++;
        i += run;
        if(!v) {
            while(run >= 11) {
                int c = run < 138 ? run : 138;
                sym[k] = 18;
                more[k++] = (unsigned char)(c - 11);
                run -= c;
            }
            if(run >= 3) {
                sym[k] = 17;
                more[k++] = (unsigned char)(run - 3);
                run = 0;
            }
        } else {
            sym[k] = (unsigned char)v;
            more[k++] = 0;
            run--;
            while(run >= 3) {
                int c = run < 6 ? run : 6;
                sym[k] = 16;
                more[k++] = (unsigned char)(c - 3);
                run -= c;
            }
        }
        while(run-- > 0) {
            sym[k] = (unsigned char)v;
            more[k++] = 0;
        }
    }
    return k;
}

/**** a block ****/

/* the symbols' cost in bits with these lengths */
static unsigned int symbols_cost(const struct deflate *z, const unsigned char *lit, const unsigned char *dist) {
    unsigned int bits = 0;
    int i;
    for(i = 0; i < 256; i++) bits += (unsigned int)z->freq_lit[i] * lit[i];
    bits += lit[256];
    for(i = 0; i < 29; i++) bits += (unsigned int)z->freq_lit[257 + i] * (unsigned int)(lit[257 + i] + length_more[i]);
    for(i = 0; i < 30; i++) bits += (unsigned int)z->freq_dist[i] * (unsigned int)(dist[i] + dist_more[i]);
    return bits;
}

static void send_symbols(struct deflate *z) {
    int i;
    for(i = 0; i < z->nsym; i++) {
        unsigned int d = z->sym_dist[i], v = z->sym_val[i];
        if(!d) {
            put_bits(z, z->code_lit[v], z->len_lit[v]);
        } else {
            int l = find_base(length_base, 29, v + MIN_MATCH), s = find_base(dist_base, 30, d);
            put_bits(z, z->code_lit[257 + l], z->len_lit[257 + l]);
            if(length_more[l]) put_bits(z, v + MIN_MATCH - length_base[l], length_more[l]);
            put_bits(z, z->code_dist[s], z->len_dist[s]);
            if(dist_more[s]) put_bits(z, d - dist_base[s], dist_more[s]);
        }
    }
    put_bits(z, z->code_lit[256], z->len_lit[256]);
}

static void fixed_lengths(unsigned char *lit, unsigned char *dist) {
    int i = 0;
    for(; i < 144; i++) lit[i] = 8;
    for(; i < 256; i++) lit[i] = 9;
    for(; i < 280; i++) lit[i] = 7;
    for(; i < 288; i++) lit[i] = 8;
    memset(dist, 5, 30);
}

static void send_stored(struct deflate *z, int last) {
    unsigned int n = z->block_bytes;
    put_bits(z, (unsigned int)last, 3);
    align(z);
    put_bits(z, n & 0xff, 8);
    put_bits(z, n >> 8, 8);
    put_bits(z, ~n & 0xff, 8);
    put_bits(z, (~n >> 8) & 0xff, 8);
    memcpy(z->pending + z->pend_n, z->window + z->block_start, n);
    z->pend_n += n;
}

static void emit_block(struct deflate *z, int last) {
    unsigned char fixed_lit[288], fixed_dist[30], sym[DEFLATE_ALPHABET + 30], more[DEFLATE_ALPHABET + 30];
    unsigned char all[DEFLATE_ALPHABET + 30], len_lengths[LENGTH_SYMBOLS];
    unsigned short freq_lengths[LENGTH_SYMBOLS], code_lengths_code[LENGTH_SYMBOLS];
    unsigned int dynamic, fixed, stored = 0xffffffffu;
    int nlit = DEFLATE_ALPHABET, ndist = 30, nlen = LENGTH_SYMBOLS, nrun, i;
    z->freq_lit[256] = 1;
    code_lengths(z->freq_lit, DEFLATE_ALPHABET, MAX_BITS, z->len_lit);
    code_lengths(z->freq_dist, 30, MAX_BITS, z->len_dist);
    while(nlit > 257 && !z->len_lit[nlit - 1]) nlit--;
    while(ndist > 1 && !z->len_dist[ndist - 1]) ndist--;
    memcpy(all, z->len_lit, (size_t)nlit);
    memcpy(all + nlit, z->len_dist, (size_t)ndist);
    nrun = run_lengths(all, nlit + ndist, sym, more);
    memset(freq_lengths, 0, sizeof(freq_lengths));
    for(i = 0; i < nrun; i++) freq_lengths[sym[i]]++;
    code_lengths(freq_lengths, LENGTH_SYMBOLS, LENGTHS_LIMIT, len_lengths);
    make_codes(len_lengths, LENGTH_SYMBOLS, code_lengths_code);
    while(nlen > 4 && !len_lengths[length_order[nlen - 1]]) nlen--;

    dynamic = 3 + 14 + 3 * (unsigned int)nlen + symbols_cost(z, z->len_lit, z->len_dist);
    for(i = 0; i < nrun; i++) {
        dynamic += len_lengths[sym[i]];
        dynamic += sym[i] == 16 ? 2u : sym[i] == 17 ? 3u : sym[i] == 18 ? 7u : 0u;
    }
    fixed_lengths(fixed_lit, fixed_dist);
    fixed = 3 + symbols_cost(z, fixed_lit, fixed_dist);
    if(z->block_start >= 0 && z->block_bytes <= 65535)
        stored = (unsigned int)(((z->bitcnt + 3 + 7) & ~7) - z->bitcnt) + 32 + 8 * z->block_bytes;

    if(stored < fixed && stored < dynamic) {
        send_stored(z, last);
    } else if(fixed <= dynamic) {
        put_bits(z, 2u | (unsigned int)last, 3);        /* the type's low bit is the second bit sent */
        memcpy(z->len_lit, fixed_lit, 288);
        memcpy(z->len_dist, fixed_dist, 30);
        make_codes(z->len_lit, 288, z->code_lit);
        make_codes(z->len_dist, 30, z->code_dist);
        send_symbols(z);
    } else {
        put_bits(z, 4u | (unsigned int)last, 3);
        put_bits(z, (unsigned int)(nlit - 257), 5);
        put_bits(z, (unsigned int)(ndist - 1), 5);
        put_bits(z, (unsigned int)(nlen - 4), 4);
        for(i = 0; i < nlen; i++) put_bits(z, len_lengths[length_order[i]], 3);
        for(i = 0; i < nrun; i++) {
            put_bits(z, code_lengths_code[sym[i]], len_lengths[sym[i]]);
            if(sym[i] == 16) put_bits(z, more[i], 2);
            else if(sym[i] == 17) put_bits(z, more[i], 3);
            else if(sym[i] == 18) put_bits(z, more[i], 7);
        }
        make_codes(z->len_lit, nlit, z->code_lit);
        make_codes(z->len_dist, ndist, z->code_dist);
        send_symbols(z);
    }
    if(last) align(z);
    z->block_start += (int)z->block_bytes;
    z->block_bytes = 0;
    z->nsym = 0;
    memset(z->freq_lit, 0, sizeof(z->freq_lit));
    memset(z->freq_dist, 0, sizeof(z->freq_dist));
}

/**** the matches ****/

static void tally_literal(struct deflate *z, unsigned char c) {
    z->sym_dist[z->nsym] = 0;
    z->sym_val[z->nsym++] = c;
    z->freq_lit[c]++;
    z->block_bytes++;
}

static void tally_match(struct deflate *z, unsigned int len, unsigned int dist) {
    z->sym_dist[z->nsym] = (unsigned short)dist;
    z->sym_val[z->nsym++] = (unsigned char)(len - MIN_MATCH);
    z->freq_lit[257 + find_base(length_base, 29, len)]++;
    z->freq_dist[find_base(dist_base, 30, dist)]++;
    z->block_bytes += len;
}

/* position at on its chain: the position before it there, 0 for none. Position 0 is therefore never matched, which
   costs one byte's worth of a match, once */
static unsigned int insert(struct deflate *z, unsigned int at) {
    const unsigned char *p = z->window + at;
    unsigned int h = (((unsigned int)p[0] | (unsigned int)p[1] << 8 | (unsigned int)p[2] << 16) * HASH_MUL) >> 17;
    unsigned int old = z->head[h];
    z->prev[at & WMASK] = (unsigned short)old;
    z->head[h] = (unsigned short)at;
    return old;
}

static unsigned int oldest(const struct deflate *z) {
    return z->pos > MAX_DIST ? z->pos - MAX_DIST : 0;
}

/* The longest match at pos along cand's chain, longer than best, with ahead bytes after pos: its length (best when
   there is none) and in *dist its distance. Nothing past pos + ahead is compared. */
static unsigned int longest_match(struct deflate *z, unsigned int cand, unsigned int best, unsigned int ahead,
                                  unsigned int *dist) {
    const unsigned char *w = z->window, *here = w + z->pos;
    unsigned int limit = oldest(z), most = ahead < MAX_MATCH ? ahead : MAX_MATCH;
    unsigned int nice = (unsigned int)z->nice < most ? (unsigned int)z->nice : most;
    int chain = z->chain;
    if(best >= most) return best;
    if(best >= (unsigned int)z->good) chain >>= 2;
    do {
        const unsigned char *p = w + cand;
        if(p[best] == here[best] && p[0] == here[0] && p[1] == here[1]) {
            unsigned int n = 2;
            while(n < most && p[n] == here[n]) n++;
            if(n > best) {
                best = n;
                *dist = z->pos - cand;
                if(n >= nice) break;
            }
        }
        cand = z->prev[cand & WMASK];
    } while(cand > limit && --chain > 0);
    return best;
}

/* the bytes after a match's first, which nothing else puts on the chains */
static void insert_rest(struct deflate *z, unsigned int from, unsigned int end) {
    for(; from < end && z->fill - from >= MIN_MATCH; from++) insert(z, from);
}

static void step_greedy(struct deflate *z, unsigned int ahead) {
    unsigned int len = MIN_MATCH - 1, dist = 0, cand;
    if(ahead >= MIN_MATCH && (cand = insert(z, z->pos)) > oldest(z)) len = longest_match(z, cand, len, ahead, &dist);
    if(len >= MIN_MATCH && !(len == MIN_MATCH && dist > TOO_FAR)) {
        tally_match(z, len, dist);
        insert_rest(z, z->pos + 1, z->pos + len);
        z->pos += len;
    } else {
        tally_literal(z, z->window[z->pos]);
        z->pos++;
    }
}

static void step_lazy(struct deflate *z, unsigned int ahead) {
    unsigned int len = MIN_MATCH - 1, dist = 0, cand;
    if(ahead >= MIN_MATCH) {
        cand = insert(z, z->pos);
        if(cand > oldest(z) && z->prev_len < (unsigned int)z->lazy) len = longest_match(z, cand, z->prev_len, ahead, &dist);
        if(len == MIN_MATCH && dist > TOO_FAR) len = MIN_MATCH - 1;
    }
    if(z->prev_len >= MIN_MATCH && len <= z->prev_len) {        /* the match held back stands: it began at pos - 1 */
        unsigned int end = z->pos - 1 + z->prev_len;
        tally_match(z, z->prev_len, z->prev_dist);
        insert_rest(z, z->pos + 1, end);
        z->pos = end;
        z->have_literal = 0;
        z->prev_len = MIN_MATCH - 1;
    } else {
        if(z->have_literal) tally_literal(z, z->window[z->pos - 1]);
        z->have_literal = 1;
        z->prev_len = len;
        z->prev_dist = dist;
        z->pos++;
    }
}

/* Symbols from the window, until its input runs short (end: there will be no more, so to its last byte) or the block
   has all it can take. One symbol is always left free for the literal held back at the input's end. */
static void code_input(struct deflate *z, int end) {
    for(;;) {
        unsigned int ahead = z->fill - z->pos;
        if(z->nsym >= DEFLATE_SYMBOLS - 1) return;
        if(ahead < LOOKAHEAD && !end) return;
        if(!ahead) break;
        if(z->lazy) step_lazy(z, ahead);
        else step_greedy(z, ahead);
    }
    if(z->have_literal) {
        tally_literal(z, z->window[z->pos - 1]);
        z->have_literal = 0;
    }
}

/* The upper half of the window becomes the lower one. Only when there is no room left and too little ahead, so that
   as much history as possible stays. */
static void take_input(struct deflate *z) {
    size_t n;
    if(z->fill == 2 * WSIZE && z->fill - z->pos < LOOKAHEAD) {
        int i;
        memcpy(z->window, z->window + WSIZE, WSIZE);
        z->pos -= WSIZE;
        z->fill -= WSIZE;
        z->block_start -= WSIZE;
        for(i = 0; i < DEFLATE_HASH; i++) z->head[i] = z->head[i] >= WSIZE ? (unsigned short)(z->head[i] - WSIZE) : 0;
        for(i = 0; i < WSIZE; i++) z->prev[i] = z->prev[i] >= WSIZE ? (unsigned short)(z->prev[i] - WSIZE) : 0;
    }
    n = 2 * WSIZE - z->fill;
    if(n > z->in_n) n = z->in_n;
    if(!n) return;
    memcpy(z->window + z->fill, z->in, n);
    z->in += n;
    z->in_n -= n;
    z->fill += (unsigned int)n;
}

void deflate_init(struct deflate *z, int level) {
    if(level < 1) level = 1;
    if(level > 9) level = 9;
    z->chain = levels[level].chain;
    z->lazy = levels[level].lazy;
    z->nice = levels[level].nice;
    z->good = levels[level].good;
    z->pos = z->fill = 0;
    z->block_start = 0;
    z->block_bytes = 0;
    z->have_literal = 0;
    z->prev_len = MIN_MATCH - 1;
    z->prev_dist = 0;
    z->nsym = 0;
    z->done = 0;
    z->pend_at = z->pend_n = 0;
    z->bitbuf = 0;
    z->bitcnt = 0;
    memset(z->freq_lit, 0, sizeof(z->freq_lit));
    memset(z->freq_dist, 0, sizeof(z->freq_dist));
    memset(z->head, 0, sizeof(z->head));
}

int deflate_run(struct deflate *z, int finish) {
    for(;;) {
        size_t n = z->pend_n - z->pend_at;
        if(n > z->out_n) n = z->out_n;
        if(n) {
            memcpy(z->out, z->pending + z->pend_at, n);
            z->out += n;
            z->out_n -= n;
            z->pend_at += (unsigned int)n;
        }
        if(z->pend_at < z->pend_n) return DEFLATE_OK;
        z->pend_at = z->pend_n = 0;
        if(z->done) return DEFLATE_END;
        take_input(z);
        code_input(z, finish && !z->in_n);
        if(z->nsym >= DEFLATE_SYMBOLS - 1) {
            emit_block(z, 0);
        } else if(finish && !z->in_n) {
            emit_block(z, 1);
            z->done = 1;
        } else if(!z->in_n) {
            return DEFLATE_OK;
        }
    }
}
