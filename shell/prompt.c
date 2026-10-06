/*
SurfOS shell prompt
--------------------
File: prompt.c  Date: 10/6/26
--------------------
The SurfOS prompt (include/surfos/prompt.h): the working directory as breadcrumbs, then
the wave and the state. Plain C without library calls: the kernel's shell links this
file, libsurf builds it again for the user shell (user/libsurf/prompt.c), and
tests/host/test_prompt.c runs it on the host.

How a long path is shortened: the names on the left give way first, replaced by one
"…" item, so the current directory and the ones just above it stay. At most max_depth
items are shown ("…" counts as one), and the prompt takes at most half the terminal's
width (at least 20 columns), which leaves the other half for typing. When even "… ▸ "
and the current directory do not fit, the directory's name keeps its last characters
after "…". Every character counts as one column, as on the SurfOS console.
*/

#include <surfos/prompt.h>

#define SEP_U    " \xE2\x96\xB8 "       /* " ▸ " (U+25B8) */
#define WAVE_U   "\xE2\x89\x8B"         /* ≋ (U+224B) */
#define MORE_U   "\xE2\x80\xA6"         /* … (U+2026) */
#define SEP_A    " > "
#define WAVE_A   "~"
#define MORE_A   "..."
#define ROOT     "SURF"
#define SEP_COLS 3

/* the SurfOS palette as ANSI SGR (the subset include/surfos/tty.h lists) */
#define C_PROMPT "\033[0;1;34m"         /* light blue: the prompt's colour since 2004 */
#define C_FAILED "\033[1;31m"           /* light red */
#define C_REMOTE "\033[1;35m"           /* light magenta */
#define C_ELEV   "\033[1;33m"           /* yellow */
#define C_DIAG   "\033[0;33m"           /* orange */
#define C_RESET  "\033[0m"

#define MAX_NAMES 130                   /* a 256-byte path has at most 128 names */

struct name {
    const char *s;                      /* bytes of the cwd or of the host name, not terminated */
    int len;
};

struct buf {
    char *p;
    int size, len, full;
};

static int slen(const char *s) {
    int n = 0;
    while(s && s[n]) n++;
    return n;
}

/* bytes, as far as they fit with room for the terminating NUL */
static void put(struct buf *b, const char *s, int n) {
    while(n-- > 0) {
        if(b->len + 1 >= b->size) { b->full = 1; return; }
        b->p[b->len++] = *s++;
    }
}

static void puts_(struct buf *b, const char *s) {
    put(b, s, slen(s));
}

/* a name as the terminal should see it: a control character would be obeyed, so it is a '?' */
static void put_name(struct buf *b, const char *s, int n) {
    for(; n > 0; n--, s++) {
        unsigned char ch = (unsigned char)*s;
        put(b, (ch < 0x20 || ch == 0x7F) ? "?" : s, 1);
    }
}

/* the columns of n bytes of UTF-8: every byte that starts a character */
static int cols(const char *s, int n) {
    int i, c = 0;
    for(i = 0; i < n; i++) if(((unsigned char)s[i] & 0xC0) != 0x80) c++;
    return c;
}

/* where the last k characters of n bytes begin */
static int tail_start(const char *s, int n, int k) {
    while(n > 0 && k > 0) {
        n--;
        if(((unsigned char)s[n] & 0xC0) != 0x80) k--;
    }
    return n;
}

enum prompt_state prompt_state(const struct prompt_ctx *c) {
    if(c->diagnostic) return PROMPT_DIAGNOSTIC;
    if(c->elevated) return PROMPT_ELEVATED;
    if(c->remote) return PROMPT_REMOTE;
    if(c->last_status) return PROMPT_FAILED;
    return PROMPT_NORMAL;
}

int prompt_render(const struct prompt_ctx *c, char *out, int size) {
    static const char state_char[] = { 0, '!', '@', '#', '?' };
    static const char *const state_colour[] = { "", C_FAILED, C_REMOTE, C_ELEV, C_DIAG };
    struct name name[MAX_NAMES + 1];
    struct buf b;
    const char *p, *sep, *wave, *more;
    enum prompt_state st = prompt_state(c);
    char ch = state_char[st];
    int n = 0, host = 0, hidden = 0, first, depth, width, budget, tail, morec, total, count, i, cut = -1;

    if(!out || size < 1) return -1;
    b.p = out;
    b.size = size;
    b.len = b.full = 0;
    sep = c->unicode ? SEP_U : SEP_A;
    wave = c->unicode ? WAVE_U : WAVE_A;
    more = c->unicode ? MORE_U : MORE_A;
    morec = c->unicode ? 1 : 3;

    /* the breadcrumbs: a remote session's host, then the path's names (the root's is SURF) */
    if(c->remote && c->hostname && c->hostname[0]) {
        name[0].s = c->hostname;
        name[0].len = slen(c->hostname);
        n = host = 1;
    }
    for(p = c->cwd ? c->cwd : "/"; *p; ) {
        const char *s;
        while(*p == '/') p++;
        if(!*p) break;
        for(s = p; *p && *p != '/'; p++);
        if(p - s == 1 && s[0] == '.') continue;
        if(n == MAX_NAMES + host) {             /* keep the last ones */
            for(i = host; i + 1 < n; i++) name[i] = name[i + 1];
            n--;
            hidden = 1;
        }
        name[n].s = s;
        name[n].len = (int)(p - s);
        n++;
    }
    if(n == host) {
        name[n].s = ROOT;
        name[n].len = slen(ROOT);
        n++;
    }

    /* how much is shown: max_depth items, half the terminal */
    depth = c->max_depth > 0 ? c->max_depth : 5;
    if(depth < 2) depth = 2;
    width = c->width > 0 ? c->width : 80;
    budget = width / 2;
    if(budget < 20) budget = 20;
    tail = 4 + (ch ? 1 : 0);                    /* " ≋" "!" "> " */
    first = host;
    if(n - host > depth) {
        first = n - (depth - 1);
        hidden = 1;
    }
    for(;;) {
        total = tail;
        count = 0;
        if(host) { total += cols(name[0].s, name[0].len); count++; }
        if(hidden) { total += morec; count++; }
        for(i = first; i < n; i++) { total += cols(name[i].s, name[i].len); count++; }
        total += SEP_COLS * (count - 1);
        if(total <= budget || first >= n - 1) break;
        first++;
        hidden = 1;
    }
    if(total > budget) {                        /* the current directory alone is too long */
        int k = budget - tail - morec - (host ? cols(name[0].s, name[0].len) + SEP_COLS : 0);
        if(k < 1) k = 1;
        cut = tail_start(name[n - 1].s, name[n - 1].len, k);
    }

    if(c->color) puts_(&b, C_PROMPT);
    if(host) {
        put_name(&b, name[0].s, name[0].len);
        puts_(&b, sep);
    }
    if(cut >= 0) {
        puts_(&b, more);
        put_name(&b, name[n - 1].s + cut, name[n - 1].len - cut);
    } else {
        if(hidden) {
            puts_(&b, more);
            puts_(&b, sep);
        }
        for(i = first; i < n; i++) {
            put_name(&b, name[i].s, name[i].len);
            if(i < n - 1) puts_(&b, sep);
        }
    }
    puts_(&b, " ");
    puts_(&b, wave);
    if(ch) {
        if(c->color) puts_(&b, state_colour[st]);
        put(&b, &ch, 1);
        if(c->color) puts_(&b, C_PROMPT);
    }
    puts_(&b, ">");
    if(c->color) puts_(&b, C_RESET);
    puts_(&b, " ");
    b.p[b.len] = 0;
    return b.full ? -1 : b.len;
}

int prompt_columns(const char *s) {
    int n = 0;
    while(s && *s) {
        if(*s == 0x1B) {                        /* ESC [ parameters final */
            s++;
            if(*s == '[') {
                s++;
                while(*s && !(*s >= 0x40 && *s <= 0x7E)) s++;
                if(*s) s++;
            }
            continue;
        }
        if(((unsigned char)*s & 0xC0) != 0x80) n++;
        s++;
    }
    return n;
}
