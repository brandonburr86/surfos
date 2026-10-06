/*
SurfOS shell: surfsh
--------------------
File: complete.c  Date: 10/6/26
--------------------
Tab completion of the word before the cursor. The line up to the cursor is read
with the parser's rules (quotes, backslashes, ; & < >) to find that word, its
characters and whether it names a command: the first word of a command, unless
it follows < or >.

A command name, without a /, completes against surfsh's own commands, the
kernel's (SYS_kcmdinfo; none on a kernel without the bridge) and the programs in
/initrd/bin. Anything else, and a name with a /, is a path: the part up to the
last / is the directory (relative to the current one), and its entries that start
with the rest are the candidates. ".", ".." and names that start with a dot are
offered only when the typed part starts with a dot.

One candidate is completed, with "/" after a directory and a space after anything
else. Several are completed as far as they agree; when they agree on nothing more,
the editor lists them on a second Tab. A name with a blank or a special character
in it is completed inside double quotes (left open after a directory, so the next
Tab can go on), as is any word that was begun with a quote.
*/

#include "surfsh.h"

struct word {
    int start;                  /* where the word begins in the line */
    int cmd;                    /* it names a command */
    int quoted;                 /* it began with a quote, or one is open at the cursor */
    int comment;                /* the cursor is in a comment: nothing to complete */
    int len;
    char text[LINE_LIMIT + 1];  /* the word up to the cursor, without quotes and backslashes */
};

static void add_char(struct word *w, char c) {
    if(w->len < LINE_LIMIT) w->text[w->len++] = c;
}

/* the word that ends at the cursor (pos), read as parse.c reads a line */
static void word_at(const char *line, int pos, struct word *w) {
    int i, in_word = 0, quote = 0, words = 0, target = 0, is_target = 0;
    w->start = pos;
    w->len = w->quoted = w->comment = 0;
    for(i = 0; i < pos; i++) {
        char c = line[i];
        if(quote == '\'') {
            if(c == '\'') quote = 0;
            else add_char(w, c);
            continue;
        }
        if(quote == '"') {
            if(c == '"') quote = 0;
            else if(c == '\\' && i + 1 < pos && (line[i + 1] == '"' || line[i + 1] == '\\')) add_char(w, line[++i]);
            else add_char(w, c);
            continue;
        }
        if(c == ' ' || c == '\t' || c == ';' || c == '&' || c == '|' || c == '<' || c == '>') {
            if(in_word) {
                if(is_target) is_target = 0;
                else words++;
                in_word = 0;
            }
            if(c == ';' || c == '&' || c == '|') words = target = 0;
            else if(c == '<' || c == '>') target = 1;
            continue;
        }
        if(!in_word) {
            if(c == '#') {
                w->comment = 1;
                return;
            }
            in_word = 1;
            w->start = i;
            w->len = w->quoted = 0;
            is_target = target;
            target = 0;
        }
        if(c == '\\') {
            if(i + 1 < pos) add_char(w, line[++i]);
        } else if(c == '"' || c == '\'') {
            quote = c;
            if(i == w->start) w->quoted = 1;
        } else {
            add_char(w, c);
        }
    }
    if(!in_word) {
        w->start = pos;
        w->len = 0;
        is_target = target;
    }
    if(quote) w->quoted = 1;
    w->cmd = !words && !is_target;
    w->text[w->len] = 0;
}

/**** candidates ****/

static void add(struct cands *c, const char *name, int dir) {
    char *copy;
    if(c->n == c->cap) {
        struct cand *v = realloc(c->v, (size_t)(c->cap ? c->cap * 2 : 32) * sizeof(*v));
        if(!v) return;
        c->v = v;
        c->cap = c->cap ? c->cap * 2 : 32;
    }
    if(!(copy = strdup(name))) return;
    c->v[c->n].name = copy;
    c->v[c->n].dir = dir;
    c->n++;
}

void cands_free(struct cands *c) {
    int i;
    for(i = 0; i < c->n; i++) free(c->v[i].name);
    free(c->v);
    c->v = NULL;
    c->n = c->cap = 0;
}

static int by_name(const void *a, const void *b) {
    return strcmp(((const struct cand *)a)->name, ((const struct cand *)b)->name);
}

/* sorted, each name once (a command can be surfsh's, the kernel's and a program) */
static void sort_unique(struct cands *c) {
    int i, n = 0;
    qsort(c->v, (size_t)c->n, sizeof(struct cand), by_name);
    for(i = 0; i < c->n; i++) {
        if(n && !strcmp(c->v[n - 1].name, c->v[i].name)) {
            c->v[n - 1].dir |= c->v[i].dir;
            free(c->v[i].name);
            continue;
        }
        c->v[n++] = c->v[i];
    }
    c->n = n;
}

static void command_cands(const char *prefix, struct cands *c) {
    struct cands progs = { NULL, 0, 0 };
    size_t n = strlen(prefix);
    int i;
    for(i = 0; i < nbuiltins; i++) if(!strncmp(builtins[i].name, prefix, n)) add(c, builtins[i].name, 0);
    for(i = 0; i < kcmd_count(); i++) if(!strncmp(kcmd_entry(i)->name, prefix, n)) add(c, kcmd_entry(i)->name, 0);
    program_names(&progs);
    for(i = 0; i < progs.n; i++) if(!strncmp(progs.v[i].name, prefix, n)) add(c, progs.v[i].name, 0);
    cands_free(&progs);
    sort_unique(c);
}

/* the entries of the typed directory that start with the typed rest; *dirlen: the directory part */
static void path_cands(const char *text, struct cands *c, int *dirlen) {
    const char *slash = strrchr(text, '/'), *base = slash ? slash + 1 : text;
    char dir[PATH_MAX];
    size_t blen = strlen(base);
    struct dirent *de;
    DIR *d;
    *dirlen = slash ? (int)(slash - text) + 1 : 0;
    if(*dirlen >= (int)sizeof(dir)) return;
    if(slash) {
        memcpy(dir, text, (size_t)*dirlen);
        dir[*dirlen] = 0;
    } else {
        strcpy(dir, ".");
    }
    if(base[0] == '.') {                        /* the directory entries the kernel does not list */
        if(!strncmp(".", base, blen)) add(c, ".", 1);
        if(!strncmp("..", base, blen)) add(c, "..", 1);
    }
    if((d = opendir(dir))) {
        while((de = readdir(d))) {
            if(de->d_name[0] == '.' && base[0] != '.') continue;
            if(!strncmp(de->d_name, base, blen)) add(c, de->d_name, de->d_type == DT_DIR);
        }
        closedir(d);
    }
    sort_unique(c);
}

/**** the completed word ****/

/* value as a word: in double quotes when quote, then suffix (inside them); close: the closing quote */
static int write_word(char *out, int size, const char *value, const char *suffix, int quote, int close) {
    int n = 0;
    const char *s;
    if(quote) {
        if(n + 1 >= size) return -1;
        out[n++] = '"';
    }
    for(s = value; *s; s++) {
        if(n + 2 >= size) return -1;
        if(quote && (*s == '"' || *s == '\\')) out[n++] = '\\';
        out[n++] = *s;
    }
    for(s = suffix; *s; s++) {
        if(n + 1 >= size) return -1;
        out[n++] = *s;
    }
    if(quote && close) {
        if(n + 1 >= size) return -1;
        out[n++] = '"';
    }
    out[n] = 0;
    return n;
}

int complete(char *line, int *len, int *pos, int limit, struct cands *list) {
    static struct word w;
    static char value[LINE_LIMIT + 1], word[2 * LINE_LIMIT + 8];
    struct cands c = { NULL, 0, 0 };
    const char *first;
    int dirlen = 0, common, i, n, single, dir, quote, tail;

    word_at(line, *pos, &w);
    if(w.comment) return 0;
    if(w.cmd && !strchr(w.text, '/')) command_cands(w.text, &c);
    else path_cands(w.text, &c, &dirlen);
    if(!c.n) {
        cands_free(&c);
        return 0;
    }

    /* how far the candidates agree, not cutting a UTF-8 character */
    first = c.v[0].name;
    common = (int)strlen(first);
    for(i = 1; i < c.n; i++) {
        int k = 0;
        while(k < common && c.v[i].name[k] == first[k]) k++;
        common = k;
    }
    while(common > 0 && first[common] && ((unsigned char)first[common] & 0xC0) == 0x80) common--;
    single = c.n == 1;
    dir = single && c.v[0].dir;
    if(snprintf(value, sizeof(value), "%.*s%.*s", dirlen, w.text, common, first) >= (int)sizeof(value)) {
        cands_free(&c);
        return 0;
    }
    if(!single && (int)strlen(value) <= w.len) {    /* nothing more in common: the caller may list them */
        *list = c;
        return 2;
    }

    quote = w.quoted || needs_quotes(value);
    n = write_word(word, sizeof(word), value, dir ? "/" : "", quote, single && !dir);
    if(n >= 0 && single && !dir) {
        if(n + 2 > (int)sizeof(word)) n = -1;
        else word[n++] = ' ';
    }
    cands_free(&c);
    tail = *len - *pos;
    if(n < 0 || w.start + n + tail > limit) return 0;  /* it would not fit in the line */
    memmove(line + w.start + n, line + *pos, (size_t)tail);
    memcpy(line + w.start, word, (size_t)n);
    *len = w.start + n + tail;
    *pos = w.start + n;
    return 1;
}

/**** lists ****/

void columns(char *const *names, int n, int width, const char *indent, const char *eol,
             void (*put)(const char *s, int len)) {
    int i, r, k, maxw = 0, colw, ncols, nrows, ind = (int)strlen(indent);
    for(i = 0; i < n; i++) {
        int w = display_cols(names[i], (int)strlen(names[i]));
        if(w > maxw) maxw = w;
    }
    colw = maxw + 2;
    /* the last column never reaches the edge of the terminal */
    ncols = width - 1 - ind - maxw >= 0 ? (width - 1 - ind - maxw) / colw + 1 : 1;
    nrows = (n + ncols - 1) / ncols;
    for(r = 0; r < nrows; r++) {
        put(indent, ind);
        for(k = 0; k < ncols; k++) {
            int at = k * nrows + r, pad;
            if(at >= n) break;
            put(names[at], (int)strlen(names[at]));
            if(at + nrows >= n) break;          /* the last in this row */
            for(pad = colw - display_cols(names[at], (int)strlen(names[at])); pad > 0; pad--) put(" ", 1);
        }
        put(eol, (int)strlen(eol));
    }
}
