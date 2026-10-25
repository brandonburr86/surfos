/*
SurfOS shell: surfsh
--------------------
File: parse.c   Date: 10/6/26, rewritten 10/24/26
--------------------
Text becomes a tree (struct node, surfsh.h), the shell grammar of POSIX in the part surfsh
runs. A complete command is what the text holds up to a newline outside every command (or
its end):

    list       and-or lists separated by ; and &              a & b ; c
    and-or     pipelines joined by && and ||, left to right   a && b || c
    pipeline   commands joined by |, perhaps ! first          ! a | b | c
    command    a simple command, { list ; } or ( list ) with redirections after them
    simple     words, name=value words before them, and redirections anywhere among them

Words are kept as typed, quotes and all; expand.c makes fields of them when the command
runs, so $? is always the status of the command just before. The scanner finds where a word
ends: at a blank, a newline, or one of ; & | < > ( ) outside quotes. In a word 'single'
quotes keep everything up to the next ', "double" quotes everything but \ $ and `, a \ the
next character; $( ... ), ${ ... }, $(( ... )) and ` ... ` are read to their ends, nested and
quoted as they are (the inside of $( ... ) is parsed, so that its ) is the one that ends a
command). A \ before a newline joins the two lines. A # where a word begins starts a comment
to the end of the line. An empty command (a leading ;, ";;") is skipped, as surfsh always did.

Redirections: [n]< [n]> [n]>> [n]>| [n]<> [n]<& [n]>& [n]<< [n]<<-, with the descriptor n
written right before the operator, then a word: a file, a descriptor or -, or a here-
document's end line. A here-document is the lines after the next newline up to its end line
(<<- takes the tabs off the front of each first); with a quote in the end line it is taken as
it is, otherwise $? is expanded in it. { and } are words of their own: { starts a group where
a command starts, and } ends it there; ! starts a pipeline.

When the text ends inside something (a quote, a group, a $( ), after | && or ||, a \ at the
end of a line, before a here-document's end line), the answer is PARSE_MORE with what is
open, so that the prompt asks for the next line; a file that ends so is an error. The tree's
memory is the parser's arena, which the caller frees once the command has run.
*/

#include "surfsh.h"
#include <stdarg.h>

#define BLANK(c) ((c) == ' ' || (c) == '\t')
#define OPCHAR(c) ((c) == ';' || (c) == '&' || (c) == '|' || (c) == '<' || (c) == '>' || (c) == '(' || (c) == ')')

enum {
    T_EOF, T_NEWLINE, T_WORD, T_SEMI, T_DSEMI, T_AMP, T_AND, T_OR, T_PIPE, T_LPAREN, T_RPAREN, T_REDIR
};

static const char *const tok_text[] = { "the end", "a newline", "a word", ";", ";;", "&", "&&", "||", "|", "(", ")", "" };
static const char *const redir_text[] = { "<", ">", ">>", ">|", "<>", "<&", ">&", "<<", "<<-" };

struct heredoc {
    struct heredoc *next;
    struct redir *r;
    char *end;                      /* the end line */
    int strip;                      /* <<-: the tabs at the front of its lines go */
};

enum { LIST_TOP, LIST_GROUP, LIST_SUB };     /* what ends a list: a newline, a }, a ) */

static int peek(struct parser *p);
static void take(struct parser *p);
static struct node *parse_list(struct parser *p, int kind);
static int skip_dollar(struct parser *p, int i);

/**** names, words, lines ****/

int is_name(const char *s, int n) {
    int i;
    if(n <= 0 || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for(i = 1; i < n; i++) if(!(isalnum((unsigned char)s[i]) || s[i] == '_')) return 0;
    return 1;
}

/* a word that the parser would split or change (completion and unparse.c quote it) */
int needs_quotes(const char *s) {
    if(!*s || *s == '#' || !strcmp(s, "{") || !strcmp(s, "}") || !strcmp(s, "!")) return 1;
    for(; *s; s++) {
        if(BLANK(*s) || *s == '\n' || OPCHAR(*s) || strchr("\"'\\$`*?[=", *s)) return 1;
    }
    return 0;
}

int line_of(const char *text, int pos) {
    int line = 1, i;
    for(i = 0; i < pos && text[i]; i++) if(text[i] == '\n') line++;
    return line;
}

/* the line of byte pos, counted on from the last one asked for */
static int line_at(struct parser *p, int pos) {
    if(pos < p->lpos) {
        p->line = 1;
        p->lpos = 0;
    }
    for(; p->lpos < pos && p->src[p->lpos]; p->lpos++) if(p->src[p->lpos] == '\n') p->line++;
    return p->line;
}

/**** what went wrong ****/

/* the text ended inside something: PARSE_MORE, with what is open. -1, for the callers */
static int open_at_end(struct parser *p, const char *what) {
    if(!p->status) {
        p->status = PARSE_MORE;
        snprintf(p->err, sizeof(p->err), "%s", what);
        p->err_line = line_at(p, p->pos);
    }
    return -1;
}

static int syntax(struct parser *p, int at, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int syntax(struct parser *p, int at, const char *fmt, ...) {
    va_list ap;
    if(!p->status) {
        p->status = PARSE_ERROR;
        va_start(ap, fmt);
        vsnprintf(p->err, sizeof(p->err), fmt, ap);
        va_end(ap);
        p->err_line = line_at(p, at);
    }
    return -1;
}

/* what the next token is, in a message */
static const char *what_comes(struct parser *p) {
    static char word[40];
    int t = peek(p), n;
    if(t == T_REDIR) return redir_text[p->look.op];
    if(t != T_WORD) return tok_text[t];
    n = p->look.end - p->look.start;
    snprintf(word, sizeof(word), "%.*s%s", n > 24 ? 24 : n, p->src + p->look.start, n > 24 ? "..." : "");
    return word;
}

/**** the scanner ****/

/* each skip_*(): src[i] begins the construct; the index past its end, or -1 (p->status says why) */

static int skip_squote(struct parser *p, int i) {
    const char *s = p->src;
    for(i++; s[i] && s[i] != '\''; i++);
    if(!s[i]) return open_at_end(p, "a ' is not closed");
    return i + 1;
}

static int skip_backquote(struct parser *p, int i) {
    const char *s = p->src;
    for(i++; s[i] && s[i] != '`'; i++) if(s[i] == '\\' && s[i + 1]) i++;
    if(!s[i]) return open_at_end(p, "a ` is not closed");
    return i + 1;
}

static int skip_dquote(struct parser *p, int i) {
    const char *s = p->src;
    for(i++; i >= 0 && s[i] && s[i] != '"'; ) {
        if(s[i] == '\\' && s[i + 1]) i += 2;
        else if(s[i] == '`') i = skip_backquote(p, i);
        else if(s[i] == '$') i = skip_dollar(p, i);
        else i++;
    }
    if(i < 0) return -1;
    if(!s[i]) return open_at_end(p, "a \" is not closed");
    return i + 1;
}

/* $(( ... )): src[i] is the $; to the ) that closes the ( after it */
static int skip_arith(struct parser *p, int i) {
    const char *s = p->src;
    int depth = 0;
    for(i++; i >= 0 && s[i]; ) {
        if(s[i] == '(') {
            depth++;
            i++;
        } else if(s[i] == ')') {
            i++;
            if(--depth == 0) return i;
        } else if(s[i] == '\\' && s[i + 1]) i += 2;
        else if(s[i] == '\'') i = skip_squote(p, i);
        else if(s[i] == '"') i = skip_dquote(p, i);
        else if(s[i] == '`') i = skip_backquote(p, i);
        else if(s[i] == '$') i = skip_dollar(p, i);
        else i++;
    }
    if(i < 0) return -1;
    return open_at_end(p, "a $(( is not closed");
}

/* ${ ... }: to its } */
static int skip_brace(struct parser *p, int i) {
    const char *s = p->src;
    for(i += 2; i >= 0 && s[i] && s[i] != '}'; ) {
        if(s[i] == '\\' && s[i + 1]) i += 2;
        else if(s[i] == '\'') i = skip_squote(p, i);
        else if(s[i] == '"') i = skip_dquote(p, i);
        else if(s[i] == '`') i = skip_backquote(p, i);
        else if(s[i] == '$') i = skip_dollar(p, i);
        else i++;
    }
    if(i < 0) return -1;
    if(!s[i]) return open_at_end(p, "a ${ is not closed");
    return i + 1;
}

/* $( ... ): the list inside is parsed, and thrown away, to find the ) that ends it */
static int skip_subst(struct parser *p, int i) {
    struct parser sub;
    struct arena junk = { NULL };
    int end = -1;
    if(p->depth + 1 > DEPTH_LIMIT) return syntax(p, i, "commands inside each other: at most %d", DEPTH_LIMIT);
    parser_init(&sub, p->src, &junk);
    sub.pos = i + 2;
    sub.depth = p->depth + 1;
    sub.line = line_at(p, i);
    sub.lpos = i;
    while(peek(&sub) == T_NEWLINE) take(&sub);
    if(peek(&sub) == T_RPAREN || (parse_list(&sub, LIST_SUB) && peek(&sub) == T_RPAREN)) {
        take(&sub);
        end = sub.pos;
    } else if(!sub.status) {
        if(peek(&sub) == T_EOF) open_at_end(&sub, "a $( is not closed");
        else syntax(&sub, sub.look.start, "unexpected %s inside $( )", what_comes(&sub));
    }
    if(end >= 0 && sub.pending) {                   /* its here-documents end inside it */
        end = -1;
        open_at_end(&sub, "a here-document has no end line");
    }
    if(end < 0 && !p->status) {
        p->status = sub.status;
        memcpy(p->err, sub.err, sizeof(p->err));
        p->err_line = sub.err_line;
    }
    arena_free(&junk);
    return end;
}

static int skip_dollar(struct parser *p, int i) {
    const char *s = p->src;
    if(s[i + 1] == '(' && s[i + 2] == '(') return skip_arith(p, i);
    if(s[i + 1] == '(') return skip_subst(p, i);
    if(s[i + 1] == '{') return skip_brace(p, i);
    return i + 1;
}

/* surfsh.h: past what begins at s[i] (a quote, $( ), ${ }, $(( )) or ` `), for expand.c on a word
   this parser has read already (so it is well made); i + 1 past anything else; the end of s when
   the construct would not end */
int word_skip(const char *s, int i) {
    struct parser p;
    struct arena junk = { NULL };
    int r;
    parser_init(&p, s, &junk);
    if(s[i] == '\'') r = skip_squote(&p, i);
    else if(s[i] == '"') r = skip_dquote(&p, i);
    else if(s[i] == '`') r = skip_backquote(&p, i);
    else if(s[i] == '$') r = skip_dollar(&p, i);
    else r = i + 1;
    arena_free(&junk);
    return r < 0 ? (int)strlen(s) : r;
}

/* a word from i: the index where it ends, or -1 */
static int scan_word(struct parser *p, int i) {
    const char *s = p->src;
    while(i >= 0 && s[i] && !BLANK(s[i]) && s[i] != '\n' && !OPCHAR(s[i])) {
        if(s[i] == '\\') {
            if(s[i + 1] == '\n' && !s[i + 2]) {     /* the line goes on in the next one */
                p->pos = i + 2;
                return open_at_end(p, "the line ends in a \\: it goes on");
            }
            i += s[i + 1] ? 2 : 1;                  /* a \ at the very end stays itself */
        } else if(s[i] == '\'') i = skip_squote(p, i);
        else if(s[i] == '"') i = skip_dquote(p, i);
        else if(s[i] == '`') i = skip_backquote(p, i);
        else if(s[i] == '$') i = skip_dollar(p, i);
        else i++;
    }
    return i;
}

/* the lines of the here-documents waiting for this newline: from p->pos on, to each one's end line */
static int read_heredocs(struct parser *p) {
    struct heredoc *h;
    for(h = p->pending; h; h = h->next) {
        struct sbuf body = { NULL, 0, 0 };
        size_t endlen = strlen(h->end);
        for(;;) {
            const char *line = p->src + p->pos, *nl = strchr(line, '\n'), *l = line;
            int len = nl ? (int)(nl - line) : (int)strlen(line), ll = len;
            if(!*line) {
                sb_free(&body);
                return open_at_end(p, "a here-document has no end line");
            }
            if(h->strip) while(ll > 0 && *l == '\t') { l++; ll--; }
            p->pos += len + (nl ? 1 : 0);
            if((size_t)ll == endlen && !memcmp(l, h->end, endlen)) break;
            sb_put(&body, l, ll);
            sb_putc(&body, '\n');
        }
        h->r->body = arena_strndup(p->mem, sb_str(&body), (size_t)body.len);
        sb_free(&body);
    }
    p->pending = NULL;
    return 0;
}

/* the next token into *t: 0, or -1 (p->status says why; *t is then the end) */
static int scan(struct parser *p, struct token *t) {
    const char *s = p->src;
    int i, two;
    char c;
    for(;;) {
        while(BLANK(s[p->pos])) p->pos++;
        if(s[p->pos] == '\\' && s[p->pos + 1] == '\n') {
            p->pos += 2;
            if(!s[p->pos]) {
                t->type = T_EOF;
                return open_at_end(p, "the line ends in a \\: it goes on");
            }
            continue;
        }
        if(s[p->pos] == '#') {
            while(s[p->pos] && s[p->pos] != '\n') p->pos++;
            continue;
        }
        break;
    }
    t->start = p->pos;
    t->op = t->fd = -1;
    t->type = T_EOF;
    c = s[p->pos];
    if(!c) {
        t->end = p->pos;
        return 0;
    }
    if(c == '\n') {
        t->type = T_NEWLINE;
        t->end = ++p->pos;
        return p->pending ? read_heredocs(p) : 0;
    }
    for(i = p->pos; isdigit((unsigned char)s[i]); i++);
    if(i > p->pos && i - p->pos <= 2 && (s[i] == '<' || s[i] == '>')) {    /* n> : the descriptor n */
        t->fd = atoi(s + p->pos);
        p->pos = i;
        c = s[i];
    }
    two = s[p->pos + 1];
    switch(c) {
    case '|':
        t->type = two == '|' ? T_OR : T_PIPE;
        break;
    case '&':
        t->type = two == '&' ? T_AND : T_AMP;
        break;
    case ';':
        t->type = two == ';' ? T_DSEMI : T_SEMI;
        break;
    case '(':
        t->type = T_LPAREN;
        break;
    case ')':
        t->type = T_RPAREN;
        break;
    case '<':
        t->type = T_REDIR;
        if(two == '<') t->op = s[p->pos + 2] == '-' ? R_HERESTRIP : R_HERE;
        else t->op = two == '&' ? R_DUPIN : two == '>' ? R_RDWR : R_IN;
        break;
    case '>':
        t->type = T_REDIR;
        t->op = two == '>' ? R_APPEND : two == '&' ? R_DUPOUT : two == '|' ? R_CLOBBER : R_OUT;
        break;
    default:
        i = scan_word(p, p->pos);
        if(i < 0) {
            t->type = T_EOF;
            return -1;
        }
        t->type = T_WORD;
        t->end = p->pos = i;
        return 0;
    }
    if(t->type == T_REDIR) p->pos += (int)strlen(redir_text[t->op]);
    else p->pos += (t->type == T_OR || t->type == T_AND || t->type == T_DSEMI) ? 2 : 1;
    t->end = p->pos;
    return 0;
}

/**** tokens for the grammar ****/

static int peek(struct parser *p) {
    if(!p->have) {
        if(p->status) {                             /* after an error: nothing more */
            p->look.type = T_EOF;
            p->look.start = p->look.end = p->pos;
        } else {
            scan(p, &p->look);
        }
        p->have = 1;
    }
    return p->look.type;
}

static void take(struct parser *p) {
    p->have = 0;
}

/* the token looked at is the word w, as it stands (no quotes) */
static int is_word(struct parser *p, const char *w) {
    size_t n = strlen(w);
    return peek(p) == T_WORD && (size_t)(p->look.end - p->look.start) == n && !memcmp(p->src + p->look.start, w, n);
}

static char *token_text(struct parser *p) {
    return arena_strndup(p->mem, p->src + p->look.start, (size_t)(p->look.end - p->look.start));
}

static void skip_newlines(struct parser *p) {
    while(peek(p) == T_NEWLINE) take(p);
}

/* after | && || : newlines may come before the command; the end of the text is PARSE_MORE */
static int need_command_after(struct parser *p, const char *op) {
    char why[64];
    skip_newlines(p);
    if(peek(p) != T_EOF || p->status) return p->status ? -1 : 0;
    snprintf(why, sizeof(why), "a %s needs a command after it", op);
    return open_at_end(p, why);
}

static struct node *new_node(struct parser *p, int type) {
    struct node *n = arena_alloc(p->mem, sizeof(*n));
    n->type = type;
    n->line = line_at(p, p->look.start);
    return n;
}

/* a list that grows, its arena-allocated array copied at the end */
struct grow {
    void **v;
    int n, cap;
};

static void grow_add(struct grow *g, void *x) {
    if(g->n == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 8;
        g->v = xrealloc(g->v, (size_t)g->cap * sizeof(void *));
    }
    g->v[g->n++] = x;
}

static void **grow_done(struct parser *p, struct grow *g) {
    void **v = arena_alloc(p->mem, (size_t)(g->n + 1) * sizeof(void *));
    if(g->n) memcpy(v, g->v, (size_t)g->n * sizeof(void *));
    free(g->v);
    g->v = NULL;
    return v;
}

/**** the grammar ****/

/* a here-document's end line: the word with its quotes taken off; *quoted when it had any */
static char *end_line(struct parser *p, const char *w, int *quoted) {
    struct sbuf b = { NULL, 0, 0 };
    char *r;
    *quoted = 0;
    for(; *w; w++) {
        if(*w == '\'' || *w == '"') {
            *quoted = 1;
            continue;
        }
        if(*w == '\\' && w[1]) {
            *quoted = 1;
            w++;
        }
        sb_putc(&b, *w);
    }
    r = arena_strndup(p->mem, sb_str(&b), (size_t)b.len);
    sb_free(&b);
    return r;
}

/* a redirection: its operator is the token looked at; it joins the end of *list */
static int parse_redir(struct parser *p, struct redir **list) {
    struct redir *r = arena_alloc(p->mem, sizeof(*r)), **tail;
    int at = p->look.start;
    r->op = p->look.op;
    r->fd = p->look.fd >= 0 ? p->look.fd : (r->op == R_IN || r->op == R_RDWR || r->op == R_DUPIN ||
                                             r->op == R_HERE || r->op == R_HERESTRIP) ? 0 : 1;
    take(p);
    if(peek(p) != T_WORD) {
        if(p->status) return -1;
        if(r->op == R_HERE || r->op == R_HERESTRIP) return syntax(p, at, "an end line must follow %s", redir_text[r->op]);
        return syntax(p, at, "a file name must follow %s", redir_text[r->op]);
    }
    r->word = token_text(p);
    take(p);
    if(r->op == R_HERE || r->op == R_HERESTRIP) {
        struct heredoc *h = arena_alloc(p->mem, sizeof(*h)), **hp;
        h->r = r;
        h->end = r->end = end_line(p, r->word, &r->quoted);
        h->strip = r->op == R_HERESTRIP;
        for(hp = &p->pending; *hp; hp = &(*hp)->next);
        *hp = h;
    }
    for(tail = list; *tail; tail = &(*tail)->next);
    *tail = r;
    return 0;
}

static int parse_redirs(struct parser *p, struct redir **list) {
    while(peek(p) == T_REDIR) if(parse_redir(p, list) < 0) return -1;
    return 0;
}

/* name=value with an unquoted name */
static int is_assignment(const char *w) {
    const char *eq = strchr(w, '=');
    return eq && is_name(w, (int)(eq - w));
}

static struct node *parse_simple(struct parser *p) {
    struct node *n = new_node(p, N_SIMPLE);
    struct grow words = { NULL, 0, 0 }, assigns = { NULL, 0, 0 };
    for(;;) {
        int t = peek(p);
        if(t == T_REDIR) {
            if(parse_redir(p, &n->redirs) < 0) {
                free(words.v);
                free(assigns.v);
                return NULL;
            }
            continue;
        }
        if(t != T_WORD) break;
        if(!words.n && is_assignment(p->src + p->look.start) &&
           strchr(p->src + p->look.start, '=') < p->src + p->look.end) grow_add(&assigns, token_text(p));
        else grow_add(&words, token_text(p));
        take(p);
    }
    n->nwords = words.n;
    n->words = (char **)grow_done(p, &words);
    n->nassigns = assigns.n;
    n->assigns = (char **)grow_done(p, &assigns);
    return n;
}

/* { list ; } and ( list ): the list, the closing token, the redirections after it */
static struct node *parse_compound(struct parser *p, int type) {
    struct node *n = new_node(p, type);
    int at = p->look.start;
    take(p);                                        /* the { or ( */
    skip_newlines(p);
    if(type == N_GROUP ? is_word(p, "}") : peek(p) == T_RPAREN) {
        syntax(p, at, type == N_GROUP ? "a { } group needs a command in it" : "a ( ) needs a command in it");
        return NULL;
    }
    n->a = parse_list(p, type == N_GROUP ? LIST_GROUP : LIST_SUB);
    if(!n->a) return NULL;
    if(type == N_GROUP ? !is_word(p, "}") : peek(p) != T_RPAREN) {
        if(peek(p) == T_EOF && !p->status) open_at_end(p, type == N_GROUP ? "a { group is not closed with }" : "a ( is not closed");
        else syntax(p, p->look.start, "unexpected %s", what_comes(p));
        return NULL;
    }
    take(p);
    if(parse_redirs(p, &n->redirs) < 0) return NULL;
    return n;
}

static struct node *parse_cmd(struct parser *p) {
    struct node *n = NULL;
    int t = peek(p);
    if(p->status) return NULL;
    if(++p->depth > DEPTH_LIMIT) {
        syntax(p, p->look.start, "commands inside each other: at most %d", DEPTH_LIMIT);
        return NULL;
    }
    if(t == T_LPAREN) n = parse_compound(p, N_SUB);
    else if(is_word(p, "{")) n = parse_compound(p, N_GROUP);
    else if(is_word(p, "}")) syntax(p, p->look.start, "} without a { before it");
    else if(t == T_WORD || t == T_REDIR) n = parse_simple(p);
    else if(t == T_EOF) open_at_end(p, "a command is missing");
    else syntax(p, p->look.start, "%s needs a command before it", what_comes(p));
    p->depth--;
    return n;
}

static struct node *parse_pipeline(struct parser *p) {
    struct node *first, *n;
    struct grow kids = { NULL, 0, 0 };
    int bang = 0;
    if(is_word(p, "!")) {
        int t;
        take(p);
        bang = 1;
        if(((t = peek(p)) == T_EOF || t == T_NEWLINE) && !p->status) {
            if(t == T_EOF) open_at_end(p, "a ! needs a command after it");
            else syntax(p, p->look.start, "a ! needs a command after it");
            return NULL;
        }
    }
    if(!(first = parse_cmd(p))) return NULL;
    n = first;
    if(peek(p) == T_PIPE) {
        n = new_node(p, N_PIPE);
        n->line = first->line;
        grow_add(&kids, first);
        while(peek(p) == T_PIPE) {
            struct node *c;
            int at = p->look.start;
            take(p);
            if(need_command_after(p, "|") < 0 || !(c = parse_cmd(p))) {
                free(kids.v);
                return NULL;
            }
            grow_add(&kids, c);
            if(kids.n > STAGES_LIMIT) {
                free(kids.v);
                syntax(p, at, "commands in a pipeline: at most %d", STAGES_LIMIT);
                return NULL;
            }
        }
        n->nkids = kids.n;
        n->kids = (struct node **)grow_done(p, &kids);
    }
    if(bang) {
        struct node *not = new_node(p, N_NOT);
        not->a = n;
        not->line = n->line;
        n = not;
    }
    return n;
}

static struct node *parse_and_or(struct parser *p) {
    struct node *left = parse_pipeline(p), *right, *n;
    int t;
    while(left && ((t = peek(p)) == T_AND || t == T_OR)) {
        take(p);
        if(need_command_after(p, t == T_AND ? "&&" : "||") < 0 || !(right = parse_pipeline(p))) return NULL;
        n = new_node(p, t == T_AND ? N_AND : N_OR);
        n->a = left;
        n->b = right;
        n->line = left->line;
        left = n;
    }
    return left;
}

/* does the token looked at end a list of this kind (it is not taken)? */
static int list_ends(struct parser *p, int kind) {
    int t = peek(p);
    if(t == T_EOF) return 1;
    if(kind == LIST_TOP) return t == T_NEWLINE;
    if(kind == LIST_SUB) return t == T_RPAREN;
    return is_word(p, "}");
}

/* And-or lists separated by ; & (and newlines, inside a group or a ( )): a chain of N_SEQ, a list
   ending in & is N_BG. A top list ends at a newline (not taken), the others at their } or ). */
static struct node *parse_list(struct parser *p, int kind) {
    struct node *first = NULL, **link = &first;
    for(;;) {
        struct node *n;
        int t;
        while((t = peek(p)) == T_SEMI || t == T_DSEMI || (kind != LIST_TOP && t == T_NEWLINE)) take(p);   /* empty commands */
        if(list_ends(p, kind)) {
            if(first) break;
            if(peek(p) == T_EOF) open_at_end(p, "a command is missing");
            else syntax(p, p->look.start, "a command is missing before %s", what_comes(p));
            return NULL;
        }
        if(!(n = parse_and_or(p))) return NULL;
        if(peek(p) == T_AMP) {
            struct node *bg = new_node(p, N_BG);
            take(p);
            bg->a = n;
            bg->line = n->line;
            n = bg;
        } else if(!list_ends(p, kind) && peek(p) != T_SEMI && peek(p) != T_DSEMI && peek(p) != T_NEWLINE) {
            if(!p->status) syntax(p, p->look.start, "unexpected %s", what_comes(p));
            return NULL;
        }
        if(!*link) *link = n;
        else {
            struct node *seq = new_node(p, N_SEQ);
            seq->a = *link;
            seq->b = n;
            seq->line = (*link)->line;
            *link = seq;
            link = &seq->b;
        }
    }
    return first;
}

/**** the parser ****/

void parser_init(struct parser *p, const char *text, struct arena *mem) {
    memset(p, 0, sizeof(*p));
    p->src = text;
    p->mem = mem;
    p->line = 1;
}

int parse_command(struct parser *p, struct node **out) {
    struct node *n;
    int t;
    *out = NULL;
    p->have = 0;
    while((t = peek(p)) == T_NEWLINE || t == T_SEMI || t == T_DSEMI) take(p);
    if(p->status) return p->status;
    if(t == T_EOF) return PARSE_EOF;
    n = parse_list(p, LIST_TOP);
    if(!n) return p->status ? p->status : PARSE_ERROR;
    t = peek(p);                                    /* a newline (its here-documents are read), or the end */
    if(p->status) return p->status;
    if(t == T_NEWLINE) take(p);
    else if(t != T_EOF) {
        syntax(p, p->look.start, "unexpected %s", what_comes(p));
        return PARSE_ERROR;
    }
    if(p->pending) {
        open_at_end(p, "a here-document's lines are still to come");
        return PARSE_MORE;
    }
    *out = n;
    return PARSE_OK;
}
