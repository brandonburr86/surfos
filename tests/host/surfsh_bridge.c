/* The bridge of tests/host/test_surfsh.c (10/24/26): surfsh's parser, unparser, expansions and memory
   (user/surfsh/parse.c, unparse.c, expand.c, util.c as they are, built for the host against SurfOS's user
   headers with the C library's names, tests/host/libsurf_names.h), and what they need of the rest of
   surfsh; tests/host/sim_fs.c stands in for the system calls. The test reaches them through
   tests/host/surfsh_sim.h. */
#include "../../user/surfsh/surfsh.h"
#include "surfsh_sim.h"

int last_status, exit_requested, exit_code, interactive, shell_pid = 1;

static int err_line;

static int copy_out(const char *s, char *out, int size) {
    if((int)strlen(s) >= size) return -1;
    strcpy(out, s);
    return 0;
}

int sh_parse(const char *text, char *out, int size, int *used) {
    struct arena mem = { NULL };
    struct sbuf b = { NULL, 0, 0 };
    struct parser p;
    struct node *n;
    int r;
    parser_init(&p, text, &mem);
    r = parse_command(&p, &n);
    if(r == PARSE_OK) {
        unparse(n, &b);
        copy_out(sb_str(&b), out, size);
    } else {
        copy_out(r == PARSE_EOF ? "" : p.err, out, size);
    }
    err_line = p.err_line;
    if(used) *used = p.pos;
    sb_free(&b);
    arena_free(&mem);
    return r;
}

int sh_err_line(void) {
    return err_line;
}

int sh_expand(const char *word, char *out, int size) {
    struct fields f = { NULL, 0, 0 };
    char *words[1];
    struct sbuf b = { NULL, 0, 0 };
    int i, r;
    words[0] = (char *)word;
    if(expand_words(words, 1, &f) < 0) return -1;
    for(i = 0; i < f.n; i++) {
        sb_puts(&b, f.v[i]);
        sb_putc(&b, '|');
    }
    r = copy_out(sb_str(&b), out, size);
    sb_free(&b);
    fields_free(&f);
    return r;
}

int sh_expand_doc(const char *body, char *out, int size) {
    char *s = expand_doc(body);
    int r = copy_out(s, out, size);
    free(s);
    return r;
}

void sh_set_status(int status) {
    last_status = status;
}
