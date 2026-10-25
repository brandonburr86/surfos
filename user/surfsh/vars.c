/*
SurfOS shell: surfsh
--------------------
File: vars.c    Date: 10/24/26
--------------------
Variables. A variable has a name (letters, digits and _, not a digit first) and a value;
it may be exported (export: programs get it in their environment, SYS_spawnx) and read-only
(readonly: no assignment or unset changes it). At the start surfsh takes its environment
(environ) as exported variables, and sets PATH (/initrd/bin), PWD and SHELL when they are
missing; cd changes PWD and OLDPWD. The positional parameters are $1, $2, ... ($# of them),
$0 the shell's name or its script's. A command's assignments before its name (x=1 cmd) are
for that command alone: a program finds them in its environment, a builtin sees them while
it runs (var_push()); a line of assignments alone keeps them.

A new surfsh (exec.c) gets the exported variables as its environment and the others, the
read-only marks and the positional parameters as a script that sets them up (var_state()).

10/25/26 (scripts): set's options (-e -u -x -f, $-); a function's local variables (local, put
back when it returns); the positional parameters kept while a function or a . file has its own;
and the functions themselves: a function is kept as the text of its definition (unparse.c),
parsed again into memory of its own, so it outlives the command that defined it, and a new
surfsh gets the same text. A function that is replaced or unset while it runs goes when its
last call ends.
*/

#include "surfsh.h"
#include <surfos/spawn.h>

struct var {
    struct var *next;
    char *name;
    char *value;                    /* NULL: not set (export x before x has a value) */
    int flags;                      /* V_EXPORT, V_READONLY */
};

struct pushed {                     /* a variable as it was, to be put back */
    char *name, *value;
    int flags, existed;
};

static struct var *vars;            /* sorted by name */
static struct stack {               /* var_push()'s, and local's (10/25/26) */
    struct pushed *v;
    int n, cap;
} pushes, locals;

int opt_e, opt_u, opt_x, opt_f;     /* 10/25/26: set -e -u -x -f */

char *arg0 = "surfsh";
char **posargs;
int npos;

static struct var *find(const char *name) {
    struct var *v;
    for(v = vars; v; v = v->next) {
        int c = strcmp(v->name, name);
        if(!c) return v;
        if(c > 0) break;
    }
    return NULL;
}

static struct var *make(const char *name) {
    struct var **pp, *v;
    for(pp = &vars; *pp && strcmp((*pp)->name, name) < 0; pp = &(*pp)->next);
    if(*pp && !strcmp((*pp)->name, name)) return *pp;
    v = xmalloc(sizeof(*v));
    v->name = xstrdup(name);
    v->value = NULL;
    v->flags = 0;
    v->next = *pp;
    *pp = v;
    return v;
}

const char *var_get(const char *name) {
    struct var *v = find(name);
    return v ? v->value : NULL;
}

int var_flags(const char *name) {
    struct var *v = find(name);
    return v ? v->flags | (v->value ? V_SET : 0) : 0;
}

int var_set(const char *name, const char *value, int flags) {
    struct var *v = make(name);
    if(v->flags & V_READONLY) {
        fprintf(stderr, "surfsh: %s: read-only\n", name);
        return -1;
    }
    if(value) {
        char *copy = xstrdup(value);
        free(v->value);
        v->value = copy;
    }
    v->flags |= flags;
    return 0;
}

int var_unset(const char *name) {
    struct var **pp, *v;
    for(pp = &vars; *pp && strcmp((*pp)->name, name); pp = &(*pp)->next);
    if(!(v = *pp)) return 0;
    if(v->flags & V_READONLY) {
        fprintf(stderr, "surfsh: %s: read-only\n", name);
        return -1;
    }
    *pp = v->next;
    free(v->name);
    free(v->value);
    free(v);
    return 0;
}

/* name=value, the value expanded already: 0, -1 said */
int var_assign(const char *word, int flags) {
    const char *eq = strchr(word, '=');
    char name[WORD_LIMIT + 1];
    if(!eq || !is_name(word, (int)(eq - word)) || eq - word > WORD_LIMIT) {
        fprintf(stderr, "surfsh: %s: not a name=value\n", word);
        return -1;
    }
    memcpy(name, word, (size_t)(eq - word));
    name[eq - word] = 0;
    return var_set(name, eq + 1, flags);
}

/* the variable name as it is now, kept on st */
static void keep(struct stack *st, const char *name) {
    struct var *v = find(name);
    struct pushed *p;
    if(st->n == st->cap) {
        st->cap = st->cap ? st->cap * 2 : 16;
        st->v = xrealloc(st->v, (size_t)st->cap * sizeof(struct pushed));
    }
    p = &st->v[st->n++];
    p->name = xstrdup(name);
    p->existed = v != NULL;
    p->value = v && v->value ? xstrdup(v->value) : NULL;
    p->flags = v ? v->flags : 0;
}

/* what st kept since to, put back, the latest first */
static void put_back(struct stack *st, int to) {
    while(st->n > to) {
        struct pushed *p = &st->v[--st->n];
        struct var *v = find(p->name);
        if(!p->existed) {
            if(v && !(v->flags & V_READONLY)) var_unset(p->name);
        } else if(v && !(v->flags & V_READONLY)) {
            free(v->value);
            v->value = p->value;
            v->flags = p->flags;
            p->value = NULL;
        } else if(!v) {
            var_set(p->name, p->value, p->flags);
        }
        free(p->name);
        free(p->value);
    }
}

/* x=1 before a builtin: set for as long as it runs; var_pop() puts back what was */
int var_push(const char *word) {
    const char *eq = strchr(word, '=');
    char name[WORD_LIMIT + 1];
    if(!eq || eq - word > WORD_LIMIT) return var_assign(word, 0);
    memcpy(name, word, (size_t)(eq - word));
    name[eq - word] = 0;
    keep(&pushes, name);
    return var_assign(word, 0);
}

void var_pop(int to) {
    put_back(&pushes, to);
}

int var_pushed(void) {
    return pushes.n;
}

/**** 10/25/26: a function's local variables ****/

int local_mark(void) {
    return locals.n;
}

/* local name: kept (once for each call: from mark on) to be put back when the function returns */
int local_keep(const char *name, int mark) {
    int i;
    if(var_flags(name) & V_READONLY) {
        fprintf(stderr, "surfsh: local: %s: read-only\n", name);
        return -1;
    }
    for(i = mark; i < locals.n; i++) if(!strcmp(locals.v[i].name, name)) return 0;
    keep(&locals, name);
    return 0;
}

void local_restore(int mark) {
    put_back(&locals, mark);
}

/* The environment for a program: the exported variables, and extra assignments (x=1 prog) over
   them. NULL, said, when it is more than a program may get (SPAWN_ENV_MAX, SPAWN_ENV_BYTES). */
char **var_environ(char **extra, int nextra) {
    struct var *v;
    char **env;
    int n = 0, i, k, bytes = 0;
    for(v = vars; v; v = v->next) if((v->flags & V_EXPORT) && v->value) n++;
    env = xmalloc((size_t)(n + nextra + 1) * sizeof(char *));
    n = 0;
    for(i = 0; i < nextra; i++) env[n++] = xstrdup(extra[i]);
    for(v = vars; v; v = v->next) {
        size_t len;
        if(!(v->flags & V_EXPORT) || !v->value) continue;
        len = strlen(v->name);
        for(k = 0; k < nextra && !(!strncmp(extra[k], v->name, len) && extra[k][len] == '='); k++);
        if(k < nextra) continue;                    /* the command's own assignment wins */
        env[n] = xmalloc(len + strlen(v->value) + 2);
        sprintf(env[n++], "%s=%s", v->name, v->value);
    }
    env[n] = NULL;
    for(i = 0; i < n; i++) bytes += (int)strlen(env[i]) + 1;
    if(n > SPAWN_ENV_MAX || bytes > SPAWN_ENV_BYTES) {
        fprintf(stderr, "surfsh: the environment is too big for a program: %d variables in %d bytes, at most %d in %d\n",
                n, bytes, SPAWN_ENV_MAX, SPAWN_ENV_BYTES);
        var_env_free(env);
        return NULL;
    }
    return env;
}

void var_env_free(char **env) {
    int i;
    for(i = 0; env && env[i]; i++) free(env[i]);
    free(env);
}

/* the start: the environment as exported variables, then what is missing */
void vars_init(char **env) {
    char cwd[PATH_MAX];
    int i;
    for(i = 0; env && env[i]; i++) {
        const char *eq = strchr(env[i], '=');
        if(eq && is_name(env[i], (int)(eq - env[i]))) var_assign(env[i], V_EXPORT);
    }
    if(!var_get("PATH")) var_set("PATH", PROGRAM_DIR, V_EXPORT);
    if(!var_get("SHELL")) var_set("SHELL", SURFSH_PATH, V_EXPORT);
    if(getcwd(cwd, sizeof(cwd))) var_set("PWD", cwd, V_EXPORT);
}

/* set, export -p, readonly -p: the variables as lines that set them (in single quotes where needed) */
void vars_print(int only) {
    struct var *v;
    struct sbuf b = { NULL, 0, 0 };
    for(v = vars; v; v = v->next) {
        if(only && !(v->flags & only)) continue;
        b.len = 0;
        if(only == V_EXPORT) sb_puts(&b, "export ");
        else if(only == V_READONLY) sb_puts(&b, "readonly ");
        sb_puts(&b, v->name);
        if(v->value) {
            sb_putc(&b, '=');
            if(v->value[0]) sb_quote(&b, v->value);
            else sb_puts(&b, "''");
        }
        printf("%s\n", sb_str(&b));
    }
    sb_free(&b);
}

/* what a new surfsh needs to be this one: the variables that are not exported (those come in its
   environment), the read-only marks, the functions, the positional parameters, the options */
void var_state(struct sbuf *out) {
    struct var *v;
    int i;
    for(v = vars; v; v = v->next) {
        if(v->value && !(v->flags & V_EXPORT)) {
            sb_puts(out, v->name);
            sb_putc(out, '=');
            if(v->value[0]) sb_quote(out, v->value);
            else sb_puts(out, "''");
            sb_putc(out, '\n');
        }
        if(v->flags & V_READONLY) sb_printf(out, "readonly %s\n", v->name);
    }
    funcs_text(out);                                /* 10/25/26: the functions */
    if(npos) {
        sb_puts(out, "set --");
        for(i = 0; i < npos; i++) {
            sb_putc(out, ' ');
            if(posargs[i][0]) sb_quote(out, posargs[i]);
            else sb_puts(out, "''");
        }
        sb_putc(out, '\n');
    }
    if(opt_e || opt_f || opt_u || opt_x) {          /* 10/25/26: the options, last (set -e) */
        const char *o = options_string();
        sb_puts(out, "set -");
        for(; *o; o++) if(*o != 'i') sb_putc(out, *o);
        sb_putc(out, '\n');
    }
}

/**** the positional parameters ****/

/* 10/25/26: a function's or a . file's own: the shell's are kept meanwhile */
void pos_save(struct pos_saved *s) {
    s->v = posargs;
    s->n = npos;
    posargs = NULL;
    npos = 0;
}

void pos_restore(struct pos_saved *s) {
    int i;
    for(i = 0; i < npos; i++) free(posargs[i]);
    free(posargs);
    posargs = s->v;
    npos = s->n;
}

void pos_set(int n, char **v) {
    int i;
    char **copy = xmalloc((size_t)(n + 1) * sizeof(char *));
    for(i = 0; i < n; i++) copy[i] = xstrdup(v[i]);
    copy[n] = NULL;
    for(i = 0; i < npos; i++) free(posargs[i]);
    free(posargs);
    posargs = copy;
    npos = n;
}

int pos_shift(int n) {
    int i;
    if(n < 0 || n > npos) return -1;
    for(i = 0; i < n; i++) free(posargs[i]);
    memmove(posargs, posargs + n, (size_t)(npos - n + 1) * sizeof(char *));
    npos -= n;
    return 0;
}

const char *ifs(void) {
    const char *s = var_get("IFS");
    return s ? s : " \t\n";
}

/**** 10/25/26: set's options ****/

static const struct option {
    char letter;
    const char *name;
    int *on;
} options[] = {
    { 'e', "errexit", &opt_e }, { 'f', "noglob", &opt_f }, { 'u', "nounset", &opt_u }, { 'x', "xtrace", &opt_x },
};

/* set -e, +e, -o errexit, ...: 0, -1 for a letter or a name it does not know */
int option_set(char letter, const char *name, int on) {
    unsigned i;
    for(i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
        if(name ? !strcmp(name, options[i].name) : letter == options[i].letter) {
            *options[i].on = on;
            return 0;
        }
    }
    return -1;
}

/* $-: the letters of the options that are on, and i in an interactive shell */
const char *options_string(void) {
    static char s[8];
    unsigned i, n = 0;
    for(i = 0; i < sizeof(options) / sizeof(options[0]); i++) if(*options[i].on) s[n++] = options[i].letter;
    if(interactive) s[n++] = 'i';
    s[n] = 0;
    return s;
}

/* set -o: each option and whether it is on; set +o: as the commands that set them */
void options_print(int as_commands) {
    unsigned i;
    for(i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
        if(as_commands) printf("set %co %s\n", *options[i].on ? '-' : '+', options[i].name);
        else printf("%-10s %s\n", options[i].name, *options[i].on ? "on" : "off");
    }
}

/**** 10/25/26: functions ****/

struct func {
    struct func *next;
    char *name;
    char *text;                     /* the definition, as unparse.c writes it */
    struct arena mem;               /* the tree of text */
    struct node *body;              /* the compound command */
    int refs;                       /* calls running */
    int gone;                       /* replaced or unset: freed when the last call ends */
};

static struct func *funcs;          /* sorted by name */

static void func_free(struct func *f) {
    arena_free(&f->mem);
    free(f->name);
    free(f->text);
    free(f);
}

/* f leaves the list: freed now, or when its last call ends */
static void func_drop(struct func *f) {
    if(f->refs) f->gone = 1;
    else func_free(f);
}

/* name() body, as it ran: kept as text and parsed again into the function's own memory; 0, or -1 said */
int func_define(struct node *n) {
    struct sbuf b = { NULL, 0, 0 };
    struct parser p;
    struct node *def = NULL;
    struct func *f = xmalloc(sizeof(*f)), **pp;
    memset(f, 0, sizeof(*f));
    unparse(n, &b);
    f->name = xstrdup(n->name);
    f->text = xstrdup(sb_str(&b));
    sb_free(&b);
    parser_init(&p, f->text, &f->mem);
    if(parse_command(&p, &def) != PARSE_OK || !def || def->type != N_FUNC) {
        fprintf(stderr, "surfsh: %s: the function's text did not read back: %s\n", f->name, p.err);
        func_free(f);
        return -1;
    }
    f->body = def->a;
    for(pp = &funcs; *pp && strcmp((*pp)->name, f->name) < 0; pp = &(*pp)->next);
    if(*pp && !strcmp((*pp)->name, f->name)) {      /* a new definition replaces the old one */
        struct func *old = *pp;
        f->next = old->next;
        func_drop(old);
    } else {
        f->next = *pp;
    }
    *pp = f;
    return 0;
}

struct func *func_find(const char *name) {
    struct func *f;
    for(f = funcs; f; f = f->next) {
        int c = strcmp(f->name, name);
        if(!c) return f;
        if(c > 0) break;
    }
    return NULL;
}

struct node *func_body(struct func *f) {
    return f->body;
}

void func_hold(struct func *f) {
    f->refs++;
}

void func_release(struct func *f) {
    if(--f->refs == 0 && f->gone) func_free(f);
}

/* unset -f name: 0, 1 when there is none */
int func_unset(const char *name) {
    struct func **pp;
    for(pp = &funcs; *pp; pp = &(*pp)->next) {
        if(!strcmp((*pp)->name, name)) {
            struct func *f = *pp;
            *pp = f->next;
            func_drop(f);
            return 0;
        }
    }
    return 1;
}

/* the definitions, as text a new surfsh runs, or as set prints them */
void funcs_text(struct sbuf *out) {
    struct func *f;
    for(f = funcs; f; f = f->next) sb_puts(out, f->text);
}
