/*
SurfOS user library: inflate
--------------------
File: inflate.c Date: 11/01/26
--------------------
RFC 1951 as a state machine: inflate_run() stops wherever the input or the room runs out and the
next call goes on in the state it left.

Methods: table decoding with a fast loop, which is zlib's method and follows its published design;
no zlib text is included.

A Huffman code is a table. make_table() takes section 3.2.2's three steps as they stand: count the
codes of each length, find the first code of each length, give each symbol the next code of its
length. A code is sent first bit first, so its bits are reversed to index the table, and every entry
whose low bits are that code holds it. Nine bits take most symbols in one lookup. A longer code goes
on through a second table under those nine bits, as wide as the longest code there; those tables
share one array. Its size is a bound: a second table 2^k wide means a code k bits past the first
nine, and in a whole code that takes at least k + 1 symbols under those nine bits. Entries per symbol
are highest at k = 6, 64 for 7, so 286 symbols come to 2592 entries at most.

Bytes are taken as the next code needs them, so nothing past the last block is read and no more than
7 unused bits are held between codes. run_fast() takes that apart for speed where there is plenty
of input and room: it takes two bytes at a time and no longer asks whether it can, and it gives back
any whole byte it did not use before it returns. Everything it leaves undecided (an end of input, a
code that is no code) is left to the states, which say what is wrong.

Every byte made goes into the window and into out, and copies come from the window alone; so out
need not hold any history, and a distance is checked against what the window really has.
*/

#include <string.h>
#include <inflate.h>

#define WMASK      (INFLATE_WINDOW - 1)
#define ROOT       INFLATE_ROOT
#define ROOT_SIZE  (1u << ROOT)
#define ROOT_MASK  (ROOT_SIZE - 1)
#define MAX_BITS   15
#define LENGTH_SYMBOLS 19

/* A table entry. A code: its length << 9 | its symbol. 0: no code. Under the first nine bits, a second table: */
#define LINK       0x8000u              /* with its width in bits above this */
#define LINK_SHIFT 12
#define LINK_MASK  0x0fffu              /* and where it starts in more[] below */

#define NEED (-1)                       /* read_symbol(): input ran out first */
#define NONE (-2)                       /* no code starts with these bits */

enum { BLOCK, STORED_LEN, STORED, TABLE, ORDER, LENGTHS, REPEAT, SYMBOL, LITERAL, LENGTH_EXTRA, DIST, DIST_EXTRA, COPY,
       DONE, BAD };

static const unsigned short length_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                                67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const unsigned char length_more[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
                                               5, 5, 5, 5, 0 };
static const unsigned short dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
                                              769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const unsigned char dist_more[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
                                             11, 11, 12, 12, 13, 13 };
static const unsigned char length_order[LENGTH_SYMBOLS] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

static int fail(struct inflate *z, const char *why) {
    z->msg = why;
    z->state = BAD;
    return INFLATE_BAD;
}

/* one byte more in bits: 0 when in has none */
static int pull(struct inflate *z) {
    if(!z->in_n) return 0;
    z->bits |= (unsigned int)*z->in++ << z->nbits;
    z->in_n--;
    z->nbits += 8;
    return 1;
}

static int need(struct inflate *z, int n) {
    while(z->nbits < n) if(!pull(z)) return 0;
    return 1;
}

static unsigned int take(struct inflate *z, int n) {
    unsigned int v = z->bits & ((1u << n) - 1);
    z->bits >>= n;
    z->nbits -= n;
    return v;
}

static void put(struct inflate *z, unsigned char c) {
    z->window[z->wpos] = c;
    z->wpos = (z->wpos + 1) & WMASK;
    if(z->whave < INFLATE_WINDOW) z->whave++;
    *z->out++ = c;
    z->out_n--;
}

/* a code's bits turned round: the order in which they arrive */
static unsigned int reversed(unsigned int code, int bits) {
    code = (code & 0x5555) << 1 | (code >> 1 & 0x5555);
    code = (code & 0x3333) << 2 | (code >> 2 & 0x3333);
    code = (code & 0x0f0f) << 4 | (code >> 4 & 0x0f0f);
    code = (code & 0x00ff) << 8 | (code >> 8 & 0x00ff);
    return code >> (16 - bits);
}

/* The table of the code that gives the n symbols these lengths (0: not used). 0, or -1 for lengths no code can have: more
   codes than they have room for, or fewer. Fewer is allowed where section 3.2.7 needs it: in the two codes of a block,
   not in the code of the code lengths, one symbol of one bit, or none at all. */
static int make_table(struct inflate_table *t, const unsigned char *lens, int n, int code_lengths) {
    unsigned short bl_count[MAX_BITS + 1], next_code[MAX_BITS + 1], rev[INFLATE_SYMBOLS];
    unsigned char width[ROOT_SIZE];
    unsigned int space = 1u << MAX_BITS, code = 0, at = 0;
    int bits, i;

    /* Step 1: how many codes of each length. A code of b bits takes 2^(15 - b) of the 2^15 places there are. */
    memset(bl_count, 0, sizeof(bl_count));
    for(i = 0; i < n; i++) bl_count[lens[i]]++;
    bl_count[0] = 0;
    for(bits = 1; bits <= MAX_BITS; bits++) {
        unsigned int takes = (unsigned int)bl_count[bits] << (MAX_BITS - bits);
        if(takes > space) return -1;
        space -= takes;
    }
    if(space) {
        int one_bit = bl_count[1] == 1 && space == 1u << (MAX_BITS - 1);
        if(code_lengths || !(one_bit || space == 1u << MAX_BITS)) return -1;
    }

    /* Step 2: the first code of each length. */
    for(bits = 1; bits <= MAX_BITS; bits++) {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = (unsigned short)code;
    }

    /* Step 3: each symbol the next code of its length; first to learn how wide each second table must be. */
    memset(width, 0, sizeof(width));
    for(i = 0; i < n; i++) {
        bits = lens[i];
        if(!bits) continue;
        rev[i] = (unsigned short)reversed(next_code[bits]++, bits);
        if(bits > ROOT && (unsigned int)(bits - ROOT) > width[rev[i] & ROOT_MASK]) width[rev[i] & ROOT_MASK] = (unsigned char)(bits - ROOT);
    }
    memset(t->first, 0, sizeof(t->first));
    for(i = 0; i < (int)ROOT_SIZE; i++) {
        if(!width[i]) continue;
        if(at + (1u << width[i]) > INFLATE_MORE) return -1;     /* the bound in this file's header says it cannot be */
        t->first[i] = (unsigned short)(LINK | (unsigned int)width[i] << LINK_SHIFT | at);
        at += 1u << width[i];
    }
    for(i = 0; i < n; i++) {
        unsigned int entry = (unsigned int)lens[i] << 9 | (unsigned int)i, slot;
        bits = lens[i];
        if(!bits) continue;
        if(bits <= ROOT) {
            for(slot = rev[i]; slot < ROOT_SIZE; slot += 1u << bits) t->first[slot] = (unsigned short)entry;
        } else {
            unsigned int link = t->first[rev[i] & ROOT_MASK], base = link & LINK_MASK, wide = 1u << (link >> LINK_SHIFT & 7);
            for(slot = (unsigned int)rev[i] >> ROOT; slot < wide; slot += 1u << (bits - ROOT)) t->more[base + slot] = (unsigned short)entry;
        }
    }
    return 0;
}

/* The entry for the code at the bottom of these bits. Bits that have not come read as 0, which cannot do harm: a code longer
   than what has come says so by its length, and a code that fits is the code. */
static unsigned int lookup(const struct inflate_table *t, unsigned int bits) {
    unsigned int e = t->first[bits & ROOT_MASK];
    if(e & LINK) e = t->more[(e & LINK_MASK) + (bits >> ROOT & ((1u << (e >> LINK_SHIFT & 7)) - 1))];
    return e;
}

/* The next symbol, its bits used; NEED or NONE. Nothing is used until the whole code has come. */
static int read_symbol(struct inflate *z, const struct inflate_table *t) {
    for(;;) {
        unsigned int e = lookup(t, z->bits);
        int len = (int)(e >> 9);
        if(e && len <= z->nbits) {
            take(z, len);
            return (int)(e & 511);
        }
        if(!e && z->nbits) return NONE;                 /* a code that is no code, whatever comes after it */
        if(!pull(z)) return NEED;
    }
}

/* Codes and copies, as long as a whole one is sure to fit: 8 bytes in and 258 out. Two bytes at a time go into bits whenever
   fewer than 16 are left, and a code takes at most 15, so nothing here counts them. It stops short, and leaves the state as
   it is, at anything the states must judge. */
static void run_fast(struct inflate *z) {
    const unsigned char *in = z->in, *start = in, *in_stop = in + z->in_n - 7;
    unsigned char *out = z->out, *out_start = out, *out_stop = out + z->out_n - 257, *window = z->window;
    unsigned int bits = z->bits, wpos = z->wpos, whave = z->whave;
    int nbits = z->nbits;

#define TOP_UP() do { \
        if(nbits < 16) { \
            bits |= ((unsigned int)in[0] | (unsigned int)in[1] << 8) << nbits; \
            in += 2; \
            nbits += 16; \
        } \
    } while(0)

    while(in < in_stop && out < out_stop) {
        unsigned int e, s, length, dist, from, i;
        int n;
        TOP_UP();
        e = lookup(&z->lit, bits);
        if(!e) break;
        n = (int)(e >> 9);
        s = e & 511;
        if(s < 256) {
            bits >>= n;
            nbits -= n;
            *out++ = (unsigned char)s;
            window[wpos] = (unsigned char)s;
            wpos = (wpos + 1) & WMASK;
            if(whave < INFLATE_WINDOW) whave++;
            continue;
        }
        if(s > 285) break;
        bits >>= n;
        nbits -= n;
        if(s == 256) {
            z->state = BLOCK;
            break;
        }
        s -= 257;
        TOP_UP();
        n = length_more[s];
        length = length_base[s] + (bits & ((1u << n) - 1));
        bits >>= n;
        nbits -= n;
        TOP_UP();
        e = lookup(&z->dcode, bits);
        s = e & 511;
        if(!e || s > 29) {                              /* the states read this distance again, and say why */
            z->left = length;
            z->state = DIST;
            break;
        }
        n = (int)(e >> 9);
        bits >>= n;
        nbits -= n;
        TOP_UP();
        n = dist_more[s];
        dist = dist_base[s] + (bits & ((1u << n) - 1));
        bits >>= n;
        nbits -= n;
        if(dist > whave) {
            z->msg = "a distance back past the start";
            z->state = BAD;
            break;
        }
        from = (wpos - dist) & WMASK;
        for(i = 0; i < length; i++) {
            unsigned char c = window[from];
            from = (from + 1) & WMASK;
            window[wpos] = c;
            wpos = (wpos + 1) & WMASK;
            *out++ = c;
        }
        whave = whave + length < INFLATE_WINDOW ? whave + length : INFLATE_WINDOW;
    }
#undef TOP_UP

    /* Bytes read ahead and not used go back, or the next thing read would start in the wrong place. Any whole byte left here
       is this call's. An earlier call that stopped inside a code may have left up to 14 bits, but they are the start of that
       code, and a table with codes that long has no gaps, so the first lookup finds it and uses them all. The test of in is
       only a fence. */
    while(nbits >= 8 && in > start) {
        in--;
        nbits -= 8;
    }
    bits &= (1u << nbits) - 1;
    z->in_n -= (size_t)(in - start);
    z->in = in;
    z->out_n -= (size_t)(out - out_start);
    z->out = out;
    z->bits = bits;
    z->nbits = nbits;
    z->wpos = wpos;
    z->whave = whave;
}

static void load_fixed(struct inflate *z) {
    int i = 0;
    if(z->fixed_ready) return;
    for(; i < 144; i++) z->lens[i] = 8;
    for(; i < 256; i++) z->lens[i] = 9;
    for(; i < 280; i++) z->lens[i] = 7;
    for(; i < 288; i++) z->lens[i] = 8;
    make_table(&z->lit, z->lens, 288, 0);
    memset(z->lens, 5, 32);
    make_table(&z->dcode, z->lens, 32, 0);
    z->fixed_ready = 1;
}

void inflate_init(struct inflate *z) {
    z->msg = NULL;
    z->bits = 0;
    z->nbits = 0;
    z->state = BLOCK;
    z->last = 0;
    z->fixed_ready = 0;
    z->wpos = z->whave = 0;
}

int inflate_run(struct inflate *z) {
    for(;;) switch(z->state) {
    case BLOCK:
        if(z->last) {
            take(z, z->nbits & 7);              /* the rest of the last byte */
            z->state = DONE;
            return INFLATE_END;
        }
        if(!need(z, 3)) return INFLATE_OK;
        z->last = (int)take(z, 1);
        switch(take(z, 2)) {
        case 0:
            take(z, z->nbits & 7);
            z->state = STORED_LEN;
            break;
        case 1:
            load_fixed(z);
            z->state = SYMBOL;
            break;
        case 2:
            z->state = TABLE;
            break;
        default:
            return fail(z, "a block of a type that does not exist");
        }
        break;

    case STORED_LEN: {
        unsigned int n;
        /* LEN then its complement: the two 16-bit values are taken together, so nothing has to remember the first */
        while(z->nbits < 32) if(!pull(z)) return INFLATE_OK;
        n = z->bits & 0xffff;
        if((n ^ (z->bits >> 16)) != 0xffff) return fail(z, "a stored block whose lengths disagree");
        z->bits = 0;
        z->nbits = 0;
        z->left = n;
        z->state = STORED;
        break;
    }

    case STORED: {
        size_t n = z->left;
        size_t i;
        if(n > z->in_n) n = z->in_n;
        if(n > z->out_n) n = z->out_n;
        for(i = 0; i < n; i++) put(z, z->in[i]);
        z->in += n;
        z->in_n -= n;
        z->left -= (unsigned int)n;
        if(z->left) return INFLATE_OK;
        z->state = BLOCK;
        break;
    }

    case TABLE:
        if(!need(z, 14)) return INFLATE_OK;
        z->nlit = (int)take(z, 5) + 257;
        z->ndist = (int)take(z, 5) + 1;
        z->nlen = (int)take(z, 4) + 4;
        z->fixed_ready = 0;
        if(z->nlit > 286 || z->ndist > 30) return fail(z, "more length or distance codes than there are");
        z->have = 0;
        memset(z->lens, 0, LENGTH_SYMBOLS);
        z->state = ORDER;
        break;

    case ORDER:
        while(z->have < z->nlen) {
            if(!need(z, 3)) return INFLATE_OK;
            z->lens[length_order[z->have++]] = (unsigned char)take(z, 3);
        }
        if(make_table(&z->lit, z->lens, LENGTH_SYMBOLS, 1)) return fail(z, "code lengths no code can have");
        z->have = 0;
        z->state = LENGTHS;
        break;

    case LENGTHS:
        while(z->have < z->nlit + z->ndist) {
            int s = read_symbol(z, &z->lit);
            if(s == NEED) return INFLATE_OK;
            if(s == NONE) return fail(z, "a code length that is no code");
            if(s < 16) {
                z->lens[z->have++] = (unsigned char)s;
                continue;
            }
            z->repeat = s;
            z->state = REPEAT;
            break;
        }
        if(z->state == REPEAT) break;
        if(!z->lens[256]) return fail(z, "a block with no end");
        if(make_table(&z->lit, z->lens, z->nlit, 0)) return fail(z, "literal lengths no code can have");
        if(make_table(&z->dcode, z->lens + z->nlit, z->ndist, 0)) return fail(z, "distance lengths no code can have");
        z->state = SYMBOL;
        break;

    case REPEAT: {
        int n, bits = z->repeat == 16 ? 2 : z->repeat == 17 ? 3 : 7, base = z->repeat == 18 ? 11 : 3, v = 0;
        if(!need(z, bits)) return INFLATE_OK;
        n = base + (int)take(z, bits);
        if(z->repeat == 16) {
            if(!z->have) return fail(z, "a repeat with nothing before it");
            v = z->lens[z->have - 1];
        }
        if(z->have + n > z->nlit + z->ndist) return fail(z, "code lengths that run past the end");
        while(n--) z->lens[z->have++] = (unsigned char)v;
        z->state = LENGTHS;
        break;
    }

    case SYMBOL: {
        int s;
        if(z->in_n >= 8 && z->out_n >= 258) {
            run_fast(z);
            if(z->state != SYMBOL) break;
        }
        s = read_symbol(z, &z->lit);
        if(s == NEED) return INFLATE_OK;
        if(s == NONE) return fail(z, "a code that is no code");
        if(s < 256) {
            if(!z->out_n) {
                z->literal = (unsigned char)s;
                z->state = LITERAL;
                return INFLATE_OK;
            }
            put(z, (unsigned char)s);
            break;
        }
        if(s == 256) {
            z->state = BLOCK;
            break;
        }
        s -= 257;
        if(s >= 29) return fail(z, "a length code that does not exist");
        z->left = length_base[s];
        z->extra = length_more[s];
        z->state = LENGTH_EXTRA;
        break;
    }

    case LITERAL:
        if(!z->out_n) return INFLATE_OK;
        put(z, z->literal);
        z->state = SYMBOL;
        break;

    case LENGTH_EXTRA:
        if(!need(z, z->extra)) return INFLATE_OK;
        z->left += take(z, z->extra);
        z->state = DIST;
        break;

    case DIST: {
        int s = read_symbol(z, &z->dcode);
        if(s == NEED) return INFLATE_OK;
        if(s == NONE) return fail(z, "a distance code that is no code");
        if(s >= 30) return fail(z, "a distance code that does not exist");
        z->dist = dist_base[s];
        z->extra = dist_more[s];
        z->state = DIST_EXTRA;
        break;
    }

    case DIST_EXTRA:
        if(!need(z, z->extra)) return INFLATE_OK;
        z->dist += take(z, z->extra);
        if(z->dist > z->whave) return fail(z, "a distance back past the start");
        z->state = COPY;
        break;

    case COPY: {
        size_t n = z->left;
        unsigned int from = (z->wpos - z->dist) & WMASK;
        if(n > z->out_n) n = z->out_n;
        z->left -= (unsigned int)n;
        while(n--) {
            put(z, z->window[from]);
            from = (from + 1) & WMASK;
        }
        if(z->left) return INFLATE_OK;
        z->state = SYMBOL;
        break;
    }

    case DONE:
        return INFLATE_END;

    default:
        return INFLATE_BAD;
    }
}
