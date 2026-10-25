/*
SurfOS shell: surfsh
--------------------
File: expand.c  Date: 10/24/26
--------------------
Words become fields when their command runs (parse.c keeps them as typed). The quotes go:
what 'single' quotes hold is taken as it is; in "double" quotes a \ keeps only a $ ` " \ or
a newline after it (a \ and a newline both go); outside them a \ keeps the next character,
and a \ before a newline goes with it. $? becomes the status of the command before, in
decimal, outside quotes or in double quotes ('$?' and \$? are the two characters). That is
the one expansion yet: any other $, a ` and every * are themselves, as surfsh always had
them. Each word is one field, also an empty "" (a command name or an argument of its own).
*/

#include "surfsh.h"

void fields_free(struct fields *f) {
    int i;
    for(i = 0; i < f->n; i++) free(f->v[i]);
    free(f->v);
    f->v = NULL;
    f->n = f->cap = 0;
}

static void fields_add(struct fields *f, char *s) {
    if(f->n + 1 >= f->cap) {
        f->cap = f->cap ? f->cap * 2 : 16;
        f->v = xrealloc(f->v, (size_t)f->cap * sizeof(char *));
    }
    f->v[f->n++] = s;
    f->v[f->n] = NULL;
}

/* $? at s: the status put into b, and the bytes it took (2), or 0 when s is not $? */
static int status_at(const char *s, struct sbuf *b) {
    if(s[0] != '$' || s[1] != '?') return 0;
    sb_printf(b, "%d", last_status);
    return 2;
}

/* one word, its quotes taken off and $? put in */
static char *expand_word(const char *w) {
    struct sbuf b = { NULL, 0, 0 };
    int n;
    while(*w) {
        if(*w == '\'') {
            const char *end = strchr(w + 1, '\'');
            if(!end) end = w + strlen(w);           /* the parser saw it closed; be safe */
            sb_put(&b, w + 1, (int)(end - w - 1));
            w = *end ? end + 1 : end;
        } else if(*w == '"') {
            for(w++; *w && *w != '"'; ) {
                if(*w == '\\' && w[1] == '\n') w += 2;
                else if(*w == '\\' && (w[1] == '$' || w[1] == '`' || w[1] == '"' || w[1] == '\\')) {
                    sb_putc(&b, w[1]);
                    w += 2;
                } else if((n = status_at(w, &b))) w += n;
                else sb_putc(&b, *w++);
            }
            if(*w) w++;
        } else if(*w == '\\') {
            if(w[1] == '\n') w += 2;
            else if(w[1]) {
                sb_putc(&b, w[1]);
                w += 2;
            } else sb_putc(&b, *w++);               /* a \ at the very end: itself */
        } else if((n = status_at(w, &b))) {
            w += n;
        } else {
            sb_putc(&b, *w++);
        }
    }
    if(!b.s) return xstrdup("");
    return b.s;
}

int expand_words(char **words, int n, struct fields *out) {
    int i;
    for(i = 0; i < n; i++) fields_add(out, expand_word(words[i]));
    return 0;
}

char *expand_one(const char *word) {
    return expand_word(word);
}

/* a here-document's text: $?, and a \ before $ ` \ or a newline (POSIX's rule for them) */
char *expand_doc(const char *s) {
    struct sbuf b = { NULL, 0, 0 };
    int n;
    while(*s) {
        if(*s == '\\' && s[1] == '\n') s += 2;
        else if(*s == '\\' && (s[1] == '$' || s[1] == '`' || s[1] == '\\')) {
            sb_putc(&b, s[1]);
            s += 2;
        } else if((n = status_at(s, &b))) s += n;
        else sb_putc(&b, *s++);
    }
    if(!b.s) return xstrdup("");
    return b.s;
}
