/* The bridge of tests/host/test_surfsh.c (10/24/26): surfsh's parser, unparser, expansions, variables and
   memory (user/surfsh/parse.c, unparse.c, expand.c, vars.c, arith.c, glob.c, util.c as they are, built for the host against SurfOS's user
   headers with the C library's names, tests/host/libsurf_names.h), and what they need of the rest of
   surfsh; tests/host/sim_fs.c stands in for the system calls. The test reaches them through
   tests/host/surfsh_sim.h. */
#include "../../user/surfsh/surfsh.h"
#include "surfsh_sim.h"
#include "sim_fs.h"

int last_status, exit_requested, exit_code, interactive, shell_pid = 1;

/* what expand.c needs of the rest of surfsh: $( ) says back what it was asked ("[text]", the way
   exec.c's exec_capture() gives its output: without the newlines at its end), and $! is 42 */
int exec_capture(const char *text, struct sbuf *out) {
    sb_putc(out, '[');
    sb_puts(out, text);
    sb_putc(out, ']');
    return 3;
}

int jobs_last_pid(void) {
    return 42;
}

/* opendir() and readdir() over the names of sh_tree(), for glob.c */
static const char *const *tree;

struct _DIR {
    char dir[256];                              /* the directory, no / at its end; "" for "." */
    int at;
    struct dirent de;
};

DIR *opendir(const char *path) {
    DIR *d = malloc(sizeof(*d));
    size_t n = strlen(path);
    if(!d) return NULL;
    while(n > 1 && path[n - 1] == '/') n--;
    if(n == 1 && path[0] == '.') n = 0;
    memcpy(d->dir, path, n);
    d->dir[n] = 0;
    d->at = 0;
    return d;
}

/* the next name of the tree right inside d->dir: a name's directory is what comes before its last /
   (before a / at its end, which says it is a directory itself) */
struct dirent *readdir(DIR *d) {
    while(tree && tree[d->at]) {
        const char *name = tree[d->at++], *base = name, *slash = NULL;
        char parent[256];
        size_t len = strlen(name), k;
        int dir = len && name[len - 1] == '/';
        if(dir) len--;
        for(k = 0; k < len; k++) if(name[k] == '/') slash = name + k;
        if(!slash) parent[0] = 0;
        else if(slash == name) strcpy(parent, "/");
        else {
            memcpy(parent, name, (size_t)(slash - name));
            parent[slash - name] = 0;
        }
        if(slash) base = slash + 1;
        if(strcmp(parent, d->dir)) continue;
        memcpy(d->de.name, base, (size_t)(name + len - base));
        d->de.name[name + len - base] = 0;
        d->de.type = dir ? DT_DIR : DT_REG;
        return &d->de;
    }
    return NULL;
}

int closedir(DIR *d) {
    free(d);
    return 0;
}

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
    int r;
    if(!s) return -1;
    r = copy_out(s, out, size);
    free(s);
    return r;
}

void sh_set_status(int status) {
    last_status = status;
}

void sh_set_var(const char *name, const char *value) {
    if(value) var_set(name, value, 0);
    else var_unset(name);
}

const char *sh_get_var(const char *name) {
    return var_get(name);
}

void sh_readonly(const char *name) {
    var_set(name, NULL, V_READONLY);
}

void sh_set_args(int n, const char *const *args) {
    pos_set(n, (char **)args);
}

int sh_arith(const char *text, long *result, char *err, int errsize) {
    return arith(text, result, err, errsize);
}

void sh_tree(const char *const *names) {
    int i;
    sim_reset();
    tree = names;
    for(i = 0; names[i]; i++) {
        char path[256];
        size_t n = strlen(names[i]);
        memcpy(path, names[i], n + 1);
        if(n && path[n - 1] == '/') {
            path[n - 1] = 0;
            sim_mkdir(path);
        } else {
            sim_put(path, "x");
        }
    }
}

int sh_expand_one(const char *word, char *out, int size) {
    char *s = expand_one(word);
    int r;
    if(!s) return -1;
    r = copy_out(s, out, size);
    free(s);
    return r;
}
