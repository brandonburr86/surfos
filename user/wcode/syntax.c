/*
SurfOS user program: wcode
--------------------
File: syntax.c  Date: 11/05/26
--------------------
The colouring's states along the text (syntax.h), and what uses them: which language a file is,
the state a line starts in, and the bracket that matches another.

The states live in the text index's marks (text.h), each with whether it is still right. A change
makes the states after it stale, but most changes make no difference to them (typing inside a
line rarely opens a comment), so the first look after a change tokenizes from the mark before it
to the first good mark after it and compares: when the state there is the same, everything after
it stands; otherwise the marks after it are stale and are worked out again when they are needed,
or a little at a time while the window is idle. A screenful is drawn from the mark before it,
never from the top: after Ctrl+End in a 16 MB file only that much is tokenized, within a budget,
and what the budget does not reach is drawn with the stale state, which is nearly always right,
until the idle time has caught up.
*/
#include <string.h>
#include "syntax.h"

#define SYNC_MAX (64u << 10)            /* a change's region worth comparing at the next good mark */
#define BLOCK 256                       /* lines a backward search for a bracket takes at a time */

static const char *const names[WC_LANGS] = {
    "Plain text", "C", "Assembly (NASM)", "Assembly (GNU as)", "Python", "Shell", "Makefile", "Markdown", "JSON",
};

const char *wc_lang_name(int lang) {
    return lang >= 0 && lang < WC_LANGS ? names[lang] : names[0];
}

const char *wc_lang_comment(int lang) {
    switch(lang) {
    case WC_LANG_C: return "//";
    case WC_LANG_NASM: return ";";
    case WC_LANG_GAS: case WC_LANG_PY: case WC_LANG_SH: case WC_LANG_MAKE: return "#";
    }
    return NULL;
}

int wc_lang_braces(int lang) {
    return lang == WC_LANG_C || lang == WC_LANG_JSON || lang == WC_LANG_SH;
}

/* the interpreter a #! line names: the last part of its first word, or the word after env */
static int shebang(const char *s, unsigned n) {
    char w[32];
    unsigned i = 2, a, k;
    int pass;
    if(n < 3 || s[0] != '#' || s[1] != '!') return WC_LANG_TEXT;
    for(pass = 0; pass < 2; pass++) {
        while(i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        for(a = i; i < n && s[i] != ' ' && s[i] != '\t'; i++)
            if(s[i] == '/') a = i + 1;
        k = i - a < sizeof(w) - 1 ? i - a : sizeof(w) - 1;
        memcpy(w, s + a, k);
        w[k] = 0;
        if(strcmp(w, "env")) break;
    }
    if(!strncmp(w, "python", 6)) return WC_LANG_PY;
    if(!strcmp(w, "sh") || !strcmp(w, "bash") || !strcmp(w, "dash") || !strcmp(w, "ash") || !strcmp(w, "ksh") ||
       !strcmp(w, "zsh") || !strcmp(w, "surfsh")) return WC_LANG_SH;
    if(!strcmp(w, "make")) return WC_LANG_MAKE;
    return WC_LANG_TEXT;
}

int wc_lang_guess(const char *name, const char *first, unsigned n) {
    static const struct { const char *ext; int lang; } exts[] = {
        { "c", WC_LANG_C }, { "h", WC_LANG_C }, { "asm", WC_LANG_NASM }, { "nasm", WC_LANG_NASM },
        { "inc", WC_LANG_NASM }, { "s", WC_LANG_GAS }, { "py", WC_LANG_PY }, { "sh", WC_LANG_SH },
        { "bash", WC_LANG_SH }, { "mk", WC_LANG_MAKE }, { "mak", WC_LANG_MAKE }, { "md", WC_LANG_MD },
        { "markdown", WC_LANG_MD }, { "json", WC_LANG_JSON },
    };
    const char *base = name ? strrchr(name, '/') : NULL, *dot;
    unsigned i;
    base = base ? base + 1 : name ? name : "";
    if(!strcmp(base, "Makefile") || !strcmp(base, "makefile") || !strcmp(base, "GNUmakefile")) return WC_LANG_MAKE;
    if(!strcmp(base, ".profile") || !strcmp(base, ".bashrc") || !strcmp(base, "surfshrc") || !strcmp(base, ".surfshrc"))
        return WC_LANG_SH;
    dot = strrchr(base, '.');
    if(dot && dot != base)                      /* FAT keeps a name's case but ignores it, so .C is C too */
        for(i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
            if(!strcasecmp(dot + 1, exts[i].ext)) return exts[i].lang;
    return first ? shebang(first, n) : WC_LANG_TEXT;
}

/**** the states ****/

static unsigned lex_line(struct wc_text *t, int lang, const struct wc_line *l, unsigned st, unsigned char *cls,
                         unsigned ncls) {
    return wc_lex(lang, wc_text_span(t, l->start, l->end - l->start), l->end - l->start, st, cls, ncls);
}

unsigned wc_syn_classes(struct wc_text *t, int lang, const struct wc_line *l, unsigned state, unsigned char *cls,
                        unsigned ncls) {
    return lex_line(t, lang, l, state, cls, ncls);
}

static void invalidate(struct wc_text *t, unsigned from) {
    for(; from < t->nm; from++) t->m[from].ok = 0;
}

void wc_syn_reset(struct wc_text *t) {
    invalidate(t, 1);
    t->m[0].state = 0;
    t->m[0].ok = 1;
    t->dirty_lo = 1;
    t->dirty_hi = 0;
}

/* from mark i (whose state is right) to the start of line, budget bytes at most: the state where it stopped (*reached
   that line). The marks it passes before line get their states, and new ones go where none were for WC_MARK_EVERY
   lines or WC_MARK_BYTES bytes; a mark at line itself is left to the caller. */
static unsigned walk(struct wc_text *t, int lang, unsigned i, unsigned line, unsigned long budget, unsigned *reached) {
    struct wc_line l;
    unsigned st = t->m[i].state, at = t->m[i].line, pos = t->m[i].pos, last = at, last_pos = pos, k = i + 1;
    unsigned long done = 0;
    while(at < line && done < budget) {
        wc_text_line(t, pos, &l);
        st = lex_line(t, lang, &l, st, NULL, 0);
        done += l.end - l.start + l.eol;
        pos = l.end + l.eol;
        at++;
        if(at >= line) break;
        if(k < t->nm && t->m[k].line == at) {
            t->m[k].state = st;
            t->m[k].ok = 1;
            k++;
            last = at;
            last_pos = pos;
        } else if(at - last >= WC_MARK_EVERY || pos - last_pos >= WC_MARK_BYTES) {
            if(wc_text_mark_add(t, k, pos, at) == 0) {
                t->m[k].state = st;
                t->m[k].ok = 1;
                k++;
            }
            last = at;
            last_pos = pos;
        }
    }
    *reached = at;
    return st;
}

/* after changes: are the states after them still right? */
static void sync(struct wc_text *t, int lang) {
    unsigned lo = t->dirty_lo, hi = t->dirty_hi, i, k, st, reached;
    if(lo > hi) return;
    t->dirty_lo = 1;
    t->dirty_hi = 0;
    if(lang == WC_LANG_TEXT) return;
    i = wc_text_mark_of(t, lo);
    for(k = i + 1; k < t->nm && t->m[k].pos <= hi; k++);
    if(!t->m[i].ok || k >= t->nm || !t->m[k].ok || t->m[k].pos - t->m[i].pos > SYNC_MAX) {
        invalidate(t, i + 1);
        return;
    }
    st = walk(t, lang, i, t->m[k].line, SYNC_MAX + 1, &reached);
    for(k = i + 1; k < t->nm && t->m[k].line < reached; k++);     /* walk may have added marks */
    if(reached == t->m[k].line && st == t->m[k].state) return;    /* the change made no difference past here */
    t->m[k].state = st;
    t->m[k].ok = reached == t->m[k].line;
    invalidate(t, k + 1);
}

unsigned wc_syn_state(struct wc_text *t, int lang, unsigned line, unsigned long budget, int *exact) {
    unsigned i, j, st, reached;
    if(exact) *exact = 1;
    if(lang == WC_LANG_TEXT) return 0;
    sync(t, lang);
    if(line > t->nl) line = t->nl;
    i = wc_text_mark_before(t, line);
    for(j = i; !t->m[j].ok; j--);               /* m[0] is always right */
    if(j == i && t->m[i].line == line) return t->m[i].state;
    st = walk(t, lang, j, line, budget, &reached);
    if(reached < line) {                        /* out of budget: the best guess there is */
        if(exact) *exact = 0;
        i = wc_text_mark_before(t, line);
        return t->m[i].line > reached ? t->m[i].state : st;
    }
    i = wc_text_mark_before(t, line);
    if(t->m[i].line == line) {
        t->m[i].state = st;
        t->m[i].ok = 1;
    }
    return st;
}

int wc_syn_idle(struct wc_text *t, int lang, unsigned long budget) {
    unsigned f, st, reached, last;
    if(lang == WC_LANG_TEXT) return 0;
    sync(t, lang);
    for(f = 1; f < t->nm && t->m[f].ok; f++);
    if(f >= t->nm) return 0;
    last = t->nm - 1;
    st = walk(t, lang, f - 1, t->m[last].line, budget, &reached);
    last = t->nm - 1;                           /* walk may have added marks */
    if(reached == t->m[last].line) {
        t->m[last].state = st;
        t->m[last].ok = 1;
    }
    for(f = 1; f < t->nm && t->m[f].ok; f++);
    return f < t->nm;
}

/**** brackets ****/

static int partner(int c) {
    switch(c) {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '{': return '}';
    case '}': return '{';
    }
    return 0;
}

static int in_code(int cls) {
    return cls != WC_C_COMMENT && cls != WC_C_STRING && cls != WC_C_CODE && cls != WC_C_KEY;
}

static unsigned char line_cls[WC_LEX_CLASSES];

/* the class of byte k of a line whose classes are in line_cls (past them: code) */
static int class_at(unsigned k) {
    return k < WC_LEX_CLASSES ? line_cls[k] : WC_C_TEXT;
}

int wc_syn_match(struct wc_text *t, int lang, unsigned pos, unsigned *at, unsigned *other, unsigned max_lines,
                 unsigned long max_bytes) {
    struct wc_line l;
    unsigned line, st, bpos = WC_NONE, k, lines = 0, starts[BLOCK], states[BLOCK], b0, b1, p;
    unsigned long bytes = 0;
    int c = 0, want, depth = 1, fwd, b;
    *at = *other = WC_NONE;
    if(pos > t->len) pos = t->len;
    line = wc_text_line_of(t, pos);
    wc_text_line(t, wc_text_line_start(t, line), &l);
    st = wc_syn_state(t, lang, line, max_bytes, NULL);
    st = lex_line(t, lang, &l, st, line_cls, sizeof(line_cls));
    if(pos < l.end && partner(wc_text_byte(t, pos)) && in_code(class_at(pos - l.start))) bpos = pos;
    else if(pos > l.start && pos <= l.end && partner(wc_text_byte(t, pos - 1)) && in_code(class_at(pos - 1 - l.start)))
        bpos = pos - 1;
    if(bpos == WC_NONE) return 0;
    *at = bpos;
    c = wc_text_byte(t, bpos);
    want = partner(c);
    fwd = c == '(' || c == '[' || c == '{';
    if(fwd) {
        for(k = bpos + 1;;) {
            for(; k < l.end; k++, bytes++) {
                b = wc_text_byte(t, k);
                if((b != c && b != want) || !in_code(class_at(k - l.start))) continue;
                depth += b == c ? 1 : -1;
                if(!depth) {
                    *other = k;
                    return 1;
                }
            }
            if(!l.eol || ++lines > max_lines || bytes > max_bytes) return 0;
            wc_text_line(t, l.end + l.eol, &l);
            st = lex_line(t, lang, &l, st, line_cls, sizeof(line_cls));
            k = l.start;
        }
    }
    /* backward: the line itself, then blocks of lines before it, each tokenized forward for its lines' states */
    for(k = bpos; k > l.start; bytes++) {
        b = wc_text_byte(t, --k);
        if((b != c && b != want) || !in_code(class_at(k - l.start))) continue;
        depth += b == c ? 1 : -1;
        if(!depth) {
            *other = k;
            return 1;
        }
    }
    for(b1 = line; b1 > 0 && lines <= max_lines && bytes <= max_bytes; b1 = b0) {
        b0 = b1 > BLOCK ? b1 - BLOCK : 0;
        st = wc_syn_state(t, lang, b0, max_bytes, NULL);
        for(p = wc_text_line_start(t, b0), k = b0; k < b1; k++) {
            starts[k - b0] = p;
            states[k - b0] = st;
            wc_text_line(t, p, &l);
            st = lex_line(t, lang, &l, st, NULL, 0);
            p = l.end + l.eol;
        }
        for(k = b1; k-- > b0;) {
            if(++lines > max_lines || bytes > max_bytes) return 0;
            wc_text_line(t, starts[k - b0], &l);
            lex_line(t, lang, &l, states[k - b0], line_cls, sizeof(line_cls));
            for(p = l.end; p > l.start; bytes++) {
                b = wc_text_byte(t, --p);
                if((b != c && b != want) || !in_code(class_at(p - l.start))) continue;
                depth += b == c ? 1 : -1;
                if(!depth) {
                    *other = p;
                    return 1;
                }
            }
        }
    }
    return 0;
}
