/*
SurfOS shell: surfsh
--------------------
File: unparse.c  Date: 10/24/26
--------------------
A tree becomes text again, which parse.c reads back as the same tree: what a new surfsh is
given to run (exec.c: a stage of a pipeline, a job in the background, a ( list )). The words
are as they were typed, quotes and all, so that the new surfsh expands them itself. It is
all one line, and the here-documents' lines come after it, each document ending in its end
line, as the parser reads them after the next newline.
*/

#include "surfsh.h"

static const char *const ops[] = { "<", ">", ">>", ">|", "<>", "<&", ">&", "<<", "<<-" };

static void put(struct node *n, struct sbuf *b, struct sbuf *docs);

static int default_fd(int op) {
    return (op == R_IN || op == R_RDWR || op == R_DUPIN || op == R_HERE || op == R_HERESTRIP) ? 0 : 1;
}

/* the redirections, each after a blank (but for the first when nothing came before) */
static void put_redirs(struct redir *r, struct sbuf *b, struct sbuf *docs, int blank) {
    for(; r; r = r->next, blank = 1) {
        if(blank) sb_putc(b, ' ');
        if(r->fd != default_fd(r->op)) sb_printf(b, "%d", r->fd);
        sb_puts(b, ops[r->op]);
        sb_puts(b, r->word);
        if(r->op == R_HERE || r->op == R_HERESTRIP) {
            sb_puts(docs, r->body ? r->body : "");
            sb_puts(docs, r->end ? r->end : "");
            sb_putc(docs, '\n');
        }
    }
}

/* a list's items with ; between them (an item in the background ends in & already) */
static void put_list(struct node *n, struct sbuf *b, struct sbuf *docs) {
    while(n->type == N_SEQ) {
        put(n->a, b, docs);
        sb_puts(b, n->a->type == N_BG ? " " : "; ");
        n = n->b;
    }
    put(n, b, docs);
}

static int ends_in_bg(struct node *n) {
    while(n->type == N_SEQ) n = n->b;
    return n->type == N_BG;
}

static void put(struct node *n, struct sbuf *b, struct sbuf *docs) {
    int i;
    switch(n->type) {
    case N_SIMPLE:
        for(i = 0; i < n->nassigns; i++) {
            if(i) sb_putc(b, ' ');
            sb_puts(b, n->assigns[i]);
        }
        for(i = 0; i < n->nwords; i++) {
            if(i || n->nassigns) sb_putc(b, ' ');
            sb_puts(b, n->words[i]);
        }
        put_redirs(n->redirs, b, docs, n->nassigns || n->nwords);
        break;
    case N_PIPE:
        for(i = 0; i < n->nkids; i++) {
            if(i) sb_puts(b, " | ");
            put(n->kids[i], b, docs);
        }
        break;
    case N_AND:
    case N_OR:
        put(n->a, b, docs);
        sb_puts(b, n->type == N_AND ? " && " : " || ");
        put(n->b, b, docs);
        break;
    case N_NOT:
        sb_puts(b, "! ");
        put(n->a, b, docs);
        break;
    case N_SEQ:
        put_list(n, b, docs);
        break;
    case N_BG:
        put(n->a, b, docs);
        sb_puts(b, " &");
        break;
    case N_GROUP:
        sb_puts(b, "{ ");
        put_list(n->a, b, docs);
        sb_puts(b, ends_in_bg(n->a) ? " }" : "; }");
        put_redirs(n->redirs, b, docs, 1);
        break;
    case N_SUB:
        sb_puts(b, "( ");
        put_list(n->a, b, docs);
        sb_puts(b, " )");
        put_redirs(n->redirs, b, docs, 1);
        break;
    }
}

void unparse(struct node *n, struct sbuf *out) {
    struct sbuf docs = { NULL, 0, 0 };
    put(n, out, &docs);
    sb_putc(out, '\n');
    if(docs.len) sb_put(out, docs.s, docs.len);
    sb_free(&docs);
}
