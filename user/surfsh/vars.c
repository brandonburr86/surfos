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
*/

#include "surfsh.h"
#include <surfos/spawn.h>

struct var {
    struct var *next;
    char *name;
    char *value;                    /* NULL: not set (export x before x has a value) */
    int flags;                      /* V_EXPORT, V_READONLY */
};

static struct var *vars;            /* sorted by name */
static struct pushed {              /* what var_push() changed, for var_pop() */
    char *name, *value;
    int flags, existed;
} pushed[MAX_WORDS];
static int npushed;

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

/* x=1 before a builtin: set for as long as it runs; var_pop() puts back what was */
int var_push(const char *word) {
    const char *eq = strchr(word, '=');
    struct var *v;
    char name[WORD_LIMIT + 1];
    if(!eq || eq - word > WORD_LIMIT || npushed == MAX_WORDS) return var_assign(word, 0);
    memcpy(name, word, (size_t)(eq - word));
    name[eq - word] = 0;
    v = find(name);
    pushed[npushed].name = xstrdup(name);
    pushed[npushed].existed = v != NULL;
    pushed[npushed].value = v && v->value ? xstrdup(v->value) : NULL;
    pushed[npushed].flags = v ? v->flags : 0;
    npushed++;
    return var_assign(word, 0);
}

void var_pop(int to) {
    while(npushed > to) {
        struct pushed *p = &pushed[--npushed];
        struct var *v = find(p->name);
        if(!p->existed) {
            if(v && !(v->flags & V_READONLY)) var_unset(p->name);
        } else if(v && !(v->flags & V_READONLY)) {
            free(v->value);
            v->value = p->value;
            v->flags = p->flags;
            p->value = NULL;
        }
        free(p->name);
        free(p->value);
    }
}

int var_pushed(void) {
    return npushed;
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
   environment), the read-only marks, the positional parameters */
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
    if(npos) {
        sb_puts(out, "set --");
        for(i = 0; i < npos; i++) {
            sb_putc(out, ' ');
            if(posargs[i][0]) sb_quote(out, posargs[i]);
            else sb_puts(out, "''");
        }
        sb_putc(out, '\n');
    }
}

/**** the positional parameters ****/

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
