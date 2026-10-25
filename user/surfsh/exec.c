/*
SurfOS shell: surfsh
--------------------
File: exec.c    Date: 10/24/26
--------------------
The tree runs (parse.c makes it, from a complete command). A list runs its parts one after
the other; a && b runs b when a succeeded, a || b when it failed; ! turns a status of 0 into
1 and anything else into 0; { list } runs the list in this shell with its redirections; a job
started with & is not waited for (jobs.c reports how it ended).

Redirections are made in this shell: the descriptor's file as it was is kept on a spare
descriptor (10-15, then 3-9) and put back afterwards. A program gets this shell's 0, 1 and 2
as they are then (SYS_spawnx), so 2>&1 and 3>&1 1>&2 2>&3 work on it as on anything else.
What stdout has not written when 1 goes back is written into the redirection's file, or, when
it cannot be (a full disk), dropped and said, so it never comes out on the console later. A
here-document goes in through a pipe: for a program it is written after the program has
started, as the program reads; for surfsh's own command it must all be in the pipe first, so
one of more than PIPE_SIZE bytes is written by a new surfsh (running cat), as surfsh's own
command reads it.

A pipeline. Its programs are processes, each with the pipe from the stage before as 0 and the
one to the next as 1. A stage that is surfsh's own (a builtin, a kernel command, a group, a
command whose name comes from an expansion) runs in a new surfsh (start_surfsh()), except the
last stage, which runs in this shell with the last pipe as its descriptor 0, so that
"... | read line" and a loop at the end of a pipeline (once surfsh has loops) can change this
shell. A kernel command reads only the console: one after a | is refused. Every process of
the pipeline is waited for; its status is the last stage's. A pipeline in the background has
no stage in this shell.

A new surfsh is "surfsh --sub STATUS PID" with descriptors 0 and 1 given, 2 this shell's, and
on its 3 the text to run (unparse.c), which this shell writes into a pipe after starting it.
It is how SurfOS, which has no fork(), runs ( list ), a stage of a pipeline and a job in the
background that are surfsh's own.
*/

#include "surfsh.h"

#define SAVE_MAX 12

struct doc {                                    /* a here-document for a program: written after it starts */
    int fd;                                     /* the pipe's writing end */
    char *text;
};

struct saved {
    int fd[SAVE_MAX];                           /* the descriptors the redirections changed */
    int copy[SAVE_MAX];                         /* where each was kept: -1 when it was not open */
    char *name[SAVE_MAX];                       /* the file put on it last (messages about 1) */
    int n;
    struct doc docs[4];                         /* here-documents to write once the program has started */
    int ndocs;
    int feeders[4];                             /* new surfshs writing here-documents: collected at the end */
    int nfeeders;
};

static int is_open(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0;
}

/* a descriptor no one uses: 10-15 first, then 3-9; -1 */
static int spare_fd(void) {
    int fd;
    for(fd = 10; fd < 16; fd++) if(!is_open(fd)) return fd;
    for(fd = 3; fd < 10; fd++) if(!is_open(fd)) return fd;
    return -1;
}

/* fd as it is now, kept in s (once): 0, or -1 said */
static int save(struct saved *s, int fd) {
    int i, copy = -1;
    for(i = 0; i < s->n; i++) if(s->fd[i] == fd) return 0;
    if(s->n == SAVE_MAX) {
        fprintf(stderr, "surfsh: too many redirections at once\n");
        return -1;
    }
    if(fd == 1) fflush(stdout);
    if(fd == 2) fflush(stderr);
    if(is_open(fd)) {
        copy = spare_fd();
        if(copy < 0 || dup2(fd, copy) < 0) {
            fprintf(stderr, "surfsh: no descriptor free to keep %d in\n", fd);
            return -1;
        }
    }
    s->fd[s->n] = fd;
    s->copy[s->n] = copy;
    s->name[s->n] = NULL;
    s->n++;
    return 0;
}

static void set_name(struct saved *s, int fd, const char *name) {
    int i;
    for(i = 0; i < s->n; i++) {
        if(s->fd[i] != fd) continue;
        free(s->name[i]);
        s->name[i] = name ? xstrdup(name) : NULL;
    }
}

/* The descriptors as they were, in the reverse order. What stdout holds goes into the file on 1
   first; when it cannot (a full disk), it is dropped, said, and the command failed. */
static int restore(struct saved *s, int status) {
    int i;
    for(i = s->n - 1; i >= 0; i--) {
        if(s->fd[i] == 1 && fflush(stdout) == EOF) {
            int e = errno;
            fpurge(stdout);
            if(s->name[i]) fprintf(stderr, "surfsh: %s: %s\n", s->name[i], strerror(e));
            else fprintf(stderr, "surfsh: %s\n", strerror(e));
            if(!status) status = STATUS_FAIL;
        }
        if(s->fd[i] == 2) fflush(stderr);
        if(s->copy[i] >= 0) {
            dup2(s->copy[i], s->fd[i]);
            close(s->copy[i]);
        } else {
            close(s->fd[i]);
        }
        free(s->name[i]);
    }
    s->n = 0;
    return status;
}

/* the here-documents for a program that has started, then the feeders: all of them written and closed */
static void finish_docs(struct saved *s) {
    int i, st;
    broken_pipe_exit(0);                        /* a reader that ends early is no reason for this shell to */
    for(i = 0; i < s->ndocs; i++) {
        const char *p = s->docs[i].text;
        size_t left = strlen(p);
        while(left) {
            ssize_t n = write(s->docs[i].fd, p, left);
            if(n <= 0) break;                   /* the program does not read it all */
            p += n;
            left -= (size_t)n;
        }
        close(s->docs[i].fd);
        free(s->docs[i].text);
    }
    if(interactive) broken_pipe_exit(0);
    else broken_pipe_exit(1);
    s->ndocs = 0;
    for(i = 0; i < s->nfeeders; i++) waitpid(s->feeders[i], &st, 0);
    s->nfeeders = 0;
}

static int all_digits(const char *s) {
    if(!*s) return 0;
    for(; *s; s++) if(!isdigit((unsigned char)*s)) return 0;
    return 1;
}

/* a here-document's text on descriptor fd. for_program: the program starts after this and reads it
   as it goes (finish_docs() writes it); otherwise it must be in the pipe before the command runs */
static int here_doc(struct saved *s, int fd, char *text, int for_program) {
    int p[2];
    size_t n = strlen(text);
    if(pipe(p) < 0) {
        fprintf(stderr, "surfsh: a here-document: %s\n", strerror(errno));
        free(text);
        return -1;
    }
    if(for_program && s->ndocs < 4) {
        s->docs[s->ndocs].fd = p[1];
        s->docs[s->ndocs++].text = text;
    } else if(n <= PIPE_SIZE || s->nfeeders == 4) {
        if(n > PIPE_SIZE) n = PIPE_SIZE;        /* four feeders at once: the rest is cut */
        if(n) write(p[1], text, n);
        close(p[1]);
        free(text);
    } else {
        struct sbuf b = { NULL, 0, 0 };
        char end[32];
        int k = 0, pid;
        do snprintf(end, sizeof(end), "SURFSH_END_%d", k++);    /* an end line that is no line of it */
        while(strstr(text, end));
        sb_printf(&b, "%s/cat <<'%s'\n%s%s\n", PROGRAM_DIR, end, text, end);
        pid = start_surfsh(sb_str(&b), -1, p[1], 0);
        sb_free(&b);
        close(p[1]);
        free(text);
        if(pid > 0) s->feeders[s->nfeeders++] = pid;
    }
    if(p[0] != fd) {
        dup2(p[0], fd);
        close(p[0]);
    }
    return 0;
}

/* Make the redirections of r in this shell, keeping what they change in s: 0, or -1 said (s is
   then to be restored all the same). for_program: a program starts next, with these descriptors */
static int redirect(struct redir *r, struct saved *s, int for_program) {
    for(; r; r = r->next) {
        int fd = r->fd, nfd, flags = 0;
        char *target;
        if(fd > 9) {
            fprintf(stderr, "surfsh: %d%s: a redirection is for descriptors 0 to 9\n", fd, r->op == R_IN ? "<" : ">");
            return -1;
        }
        if(save(s, fd) < 0) return -1;
        if(r->op == R_HERE || r->op == R_HERESTRIP) {
            if(here_doc(s, fd, r->quoted ? xstrdup(r->body ? r->body : "") : expand_doc(r->body ? r->body : ""), for_program) < 0)
                return -1;
            continue;
        }
        target = expand_one(r->word);
        if(r->op == R_DUPIN || r->op == R_DUPOUT) {
            if(!strcmp(target, "-")) close(fd);
            else if(!all_digits(target) || atoi(target) > 15 || !is_open(atoi(target))) {
                fprintf(stderr, "surfsh: %s: %s\n", target, all_digits(target) ? "no such descriptor" : "a descriptor's number (or -) must follow <& and >&");
                free(target);
                return -1;
            } else if(atoi(target) != fd) {
                if(fd == 1) fflush(stdout);
                dup2(atoi(target), fd);
            }
            free(target);
            continue;
        }
        if(strlen(target) > WORD_LIMIT) {
            fprintf(stderr, "surfsh: a file name of %u bytes: paths have at most %d\n", (unsigned)strlen(target), WORD_LIMIT);
            free(target);
            return -1;
        }
        switch(r->op) {
        case R_IN:      flags = O_RDONLY; break;
        case R_APPEND:  flags = O_WRONLY | O_CREAT | O_APPEND; break;
        case R_RDWR:    flags = O_RDWR | O_CREAT; break;
        default:        flags = O_WRONLY | O_CREAT | O_TRUNC; break;
        }
        nfd = open(target, flags);
        if(nfd < 0) {
            fprintf(stderr, "surfsh: %s: %s\n", target, strerror(errno));
            free(target);
            return -1;
        }
        if(fd == 1) fflush(stdout);
        if(nfd != fd) {
            dup2(nfd, fd);
            close(nfd);
        }
        set_name(s, fd, target);
        free(target);
    }
    return 0;
}

/* does r make a redirection for descriptor 0 (which a kernel command never reads)? */
static int has_input(struct redir *r) {
    for(; r; r = r->next) if(r->fd == 0) return 1;
    return 0;
}

/**** a new surfsh ****/

int start_surfsh(const char *text, int in, int out, int background) {
    char st[16], pid[16], *argv[] = { "surfsh", "--sub", st, pid, NULL };
    struct spawnx o;
    size_t left = strlen(text);
    int p[2], child;
    snprintf(st, sizeof(st), "%d", last_status);
    snprintf(pid, sizeof(pid), "%d", shell_pid);
    if(pipe(p) < 0) {
        fprintf(stderr, "surfsh: a new surfsh: %s\n", strerror(errno));
        return -1;
    }
    fflush(stdout);
    memset(&o, 0, sizeof(o));
    o.fds[0] = in >= 0 ? in : is_open(0) ? 0 : SPAWN_CLOSED;
    o.fds[1] = out >= 0 ? out : is_open(1) ? 1 : SPAWN_CLOSED;
    o.fds[2] = is_open(2) ? 2 : SPAWN_CLOSED;
    o.fds[3] = p[0];
    o.envp = environ;
    o.flags = background ? SPAWN_BG : 0;
    child = spawnx(SURFSH_PATH, argv, &o);
    close(p[0]);
    if(child < 0) {
        fprintf(stderr, "surfsh: %s: %s\n", SURFSH_PATH, strerror(errno));
        close(p[1]);
        return -1;
    }
    broken_pipe_exit(0);                        /* a new surfsh that could not start: an error here, no end */
    while(left) {
        ssize_t n = write(p[1], text, left);
        if(n <= 0) break;
        text += n;
        left -= (size_t)n;
    }
    broken_pipe_exit(!interactive);
    close(p[1]);
    return child;
}

/* a new surfsh for the tree n, with 0 and 1 given (-1: this shell's) */
static int start_tree(struct node *n, int in, int out, int background) {
    struct sbuf b = { NULL, 0, 0 };
    int pid;
    unparse(n, &b);
    pid = start_surfsh(sb_str(&b), in, out, background);
    sb_free(&b);
    return pid;
}

/* the text of a job, for its reports: its first line */
static char *job_text(struct node *n) {
    struct sbuf b = { NULL, 0, 0 };
    char *nl, *r;
    unparse(n, &b);
    r = xstrdup(sb_str(&b));
    if((nl = strchr(r, '\n'))) *nl = 0;
    sb_free(&b);
    return r;
}

/**** simple commands ****/

/* a command's name that is a word with no expansion in it: its text, quotes taken off, in buf; 0 when it has one */
static int literal_name(const char *w, char *buf, int size) {
    char *x;
    if(strpbrk(w, "$`")) return 0;
    x = expand_one(w);
    if((int)strlen(x) >= size) {
        free(x);
        return 0;
    }
    strcpy(buf, x);
    free(x);
    return 1;
}

enum { CMD_PROGRAM, CMD_KERNEL, CMD_SURFSH };

/* what a stage of a pipeline is: a program (a process of its own), a kernel command, or surfsh's */
static int stage_kind(struct node *n) {
    char name[WORD_LIMIT + 1];
    if(n->type != N_SIMPLE || !n->nwords || n->nassigns || !literal_name(n->words[0], name, sizeof(name)))
        return CMD_SURFSH;
    if(!strchr(name, '/') && builtin_find(name)) return CMD_SURFSH;
    if(!strchr(name, '/') && kcmd_find(name)) return CMD_KERNEL;
    return CMD_PROGRAM;
}

/* a word the kernel would refuse (EFAULT): what is wrong with it */
static int too_long(const char *what, const char *word) {
    size_t n = strlen(word);
    if(n <= WORD_LIMIT) return 0;
    fprintf(stderr, "surfsh: %s of %u bytes: paths and arguments have at most %d\n", what, (unsigned)n, WORD_LIMIT);
    return 1;
}

/* /initrd/bin/<name> when that program exists, for a hint; NULL otherwise */
static const char *program_of(const char *name) {
    static char path[PATH_MAX];
    struct stat st;
    if(strchr(name, '/') || snprintf(path, sizeof(path), PROGRAM_DIR "/%s", name) >= (int)sizeof(path)) return NULL;
    return stat(path, &st) == 0 && S_ISREG(st.type) ? path : NULL;
}

/* the words of n as fields, with the old order: assignments before the name are words (no variables yet) */
static int command_fields(struct node *n, struct fields *f) {
    if(expand_words(n->assigns, n->nassigns, f) < 0 || expand_words(n->words, n->nwords, f) < 0) {
        fields_free(f);
        return -1;
    }
    if(f->n > MAX_WORDS) {
        fprintf(stderr, "surfsh: too many words in a command (at most %d)\n", MAX_WORDS);
        fields_free(f);
        return -1;
    }
    return 0;
}

/* A simple command in this shell: surfsh's own, the kernel's or a program (waited for unless
   X_BACK, when it is a job of its own). in, out: its 0 and 1 first (-1: this shell's) */
static int exec_simple(struct node *n, int flags, int in, int out, int *pidp) {
    struct fields f = { NULL, 0, 0 };
    struct saved s;
    const struct builtin *b;
    char path[PATH_MAX];
    const char *name;
    int status = 0, i, pid;
    memset(&s, 0, sizeof(s));
    if(pidp) *pidp = -1;
    if(command_fields(n, &f) < 0) return STATUS_USAGE;
    if((in >= 0 && (save(&s, 0) < 0 || dup2(in, 0) < 0)) || (out >= 0 && (save(&s, 1) < 0 || dup2(out, 1) < 0))) {
        fields_free(&f);
        return restore(&s, STATUS_FAIL);
    }
    if(!f.n) {                                  /* redirections alone: made (a file made empty), then undone */
        status = redirect(n->redirs, &s, 0) < 0 ? STATUS_FAIL : 0;
        finish_docs(&s);
        return restore(&s, status);
    }
    name = f.v[0];
    if(!name[0] || !strcmp(name, ".") || !strcmp(name, "..")) {    /* no name at all, or a directory's */
        fprintf(stderr, "surfsh: %s: command not found\n", name);
        fields_free(&f);
        return restore(&s, STATUS_NOTFOUND);
    }
    if(!strchr(name, '/') && (b = builtin_find(name))) {
        if(redirect(n->redirs, &s, 0) < 0) status = STATUS_FAIL;
        else status = b->fn(f.n, f.v);
        finish_docs(&s);
        fields_free(&f);
        return restore(&s, status);
    }
    for(i = 0; i < f.n; i++) {
        if(too_long(i ? "an argument" : "a command name", f.v[i])) {
            fields_free(&f);
            return restore(&s, STATUS_USAGE);
        }
    }
    if(!strchr(name, '/') && kcmd_find(name)) {
        if(has_input(n->redirs)) {
            const char *program = program_of(name);
            fprintf(stderr, "surfsh: %s is a kernel command: it reads only the console, not < file", name);
            if(program) fprintf(stderr, "; use %s for that", program);
            fputc('\n', stderr);
            status = STATUS_USAGE;
        } else if(redirect(n->redirs, &s, 0) < 0) {
            status = STATUS_FAIL;
        } else {
            int found;
            status = run_kernel(f.v, &found);
            if(!found) {                        /* the table said so, the kernel not: as a program */
                restore(&s, 0);
                memset(&s, 0, sizeof(s));
                goto program;
            }
        }
        finish_docs(&s);
        fields_free(&f);
        return restore(&s, status);
    }
program:
    if((status = find_program(name, path, sizeof(path))) != 0) {
        fields_free(&f);
        return restore(&s, status);
    }
    if(redirect(n->redirs, &s, 1) < 0) {
        finish_docs(&s);
        fields_free(&f);
        return restore(&s, STATUS_FAIL);
    }
    pid = start_program(path, f.v, -1, -1, (flags & X_BACK) != 0);
    status = restore(&s, pid < 0 ? STATUS_CANNOT : 0);
    finish_docs(&s);                            /* the here-documents, as the program reads them */
    if(pid > 0 && pidp) *pidp = pid;
    else if(pid > 0) status = wait_program(pid, flags & X_TOP);
    fields_free(&f);
    return status;
}

/**** the tree ****/

static int exec_node(struct node *n, int flags);

/* A stage of a pipeline, not in this shell: a process with 0 = in, 1 = out (-1: this shell's).
   Its pid, or -1 (it could not start; *status says why) */
static int start_stage(struct node *n, int in, int out, int background, int *status) {
    int pid = -1;
    if(stage_kind(n) == CMD_PROGRAM) {
        *status = exec_simple(n, background ? X_BACK : 0, in, out, &pid);
        if(pid < 0 && !*status) *status = STATUS_CANNOT;
        return pid;
    }
    pid = start_tree(n, in, out, background);
    *status = pid < 0 ? STATUS_CANNOT : 0;
    return pid;
}

static int exec_pipe(struct node *n, int flags) {
    int i, in = -1, pids[STAGES_LIMIT], np = 0, last = -1, status = 0, here, st;
    int background = (flags & X_BACK) != 0;
    for(i = 1; i < n->nkids; i++) {
        if(stage_kind(n->kids[i]) == CMD_KERNEL) {
            char name[WORD_LIMIT + 1];
            literal_name(n->kids[i]->words[0], name, sizeof(name));
            fprintf(stderr, "surfsh: %s is a kernel command: it reads only the console, not the pipe before it\n", name);
            return STATUS_USAGE;
        }
    }
    here = !background && stage_kind(n->kids[n->nkids - 1]) == CMD_SURFSH;
    fflush(stdout);
    for(i = 0; i < n->nkids; i++) {
        int p[2] = { -1, -1 }, lastone = i == n->nkids - 1, pid;
        if(!lastone && pipe(p) < 0) {
            fprintf(stderr, "surfsh: a pipe: %s\n", strerror(errno));
            status = STATUS_FAIL;
            break;
        }
        if(lastone && here) {                   /* in this shell, reading the last pipe */
            struct saved s;
            memset(&s, 0, sizeof(s));
            if(in >= 0 && (save(&s, 0) < 0 || dup2(in, 0) < 0)) status = STATUS_FAIL;
            else {
                if(in >= 0) close(in);
                in = -1;
                status = exec_node(n->kids[i], flags & ~X_TOP);
            }
            status = restore(&s, status);
        } else {
            pid = start_stage(n->kids[i], in, p[1], background, &st);
            if(pid > 0) pids[np++] = pid;
            if(lastone) {
                last = pid;
                status = st;
            }
        }
        if(in >= 0) close(in);
        if(p[1] >= 0) close(p[1]);
        in = p[0];
    }
    if(in >= 0) close(in);
    if(background) {
        if(np) {
            char *text = job_text(n);
            job_add(text, pids, np);
            free(text);
        }
        return 0;
    }
    for(i = 0; i < np; i++) {
        st = wait_program(pids[i], pids[i] == last && (flags & X_TOP) && stage_kind(n->kids[n->nkids - 1]) == CMD_PROGRAM);
        if(pids[i] == last) status = st;
    }
    return status;
}

/* a job in the background: a program, a pipeline, or anything else in a new surfsh */
static int exec_background(struct node *n) {
    int pid = -1, st;
    if(n->type == N_PIPE) return exec_pipe(n, X_BACK);
    if(stage_kind(n) == CMD_PROGRAM) {
        st = exec_simple(n, X_BACK, -1, -1, &pid);
        if(pid < 0) return st;
    } else if((pid = start_tree(n, -1, -1, 1)) < 0) {
        return STATUS_CANNOT;
    }
    {
        char *text = job_text(n);
        job_add(text, &pid, 1);
        free(text);
    }
    return 0;
}

/* ( list ): a new surfsh, with the redirections made for it here */
static int exec_sub(struct node *n) {
    struct saved s;
    int pid, status;
    memset(&s, 0, sizeof(s));
    if(redirect(n->redirs, &s, 1) < 0) {
        finish_docs(&s);
        return restore(&s, STATUS_FAIL);
    }
    pid = start_tree(n->a, -1, -1, 0);
    status = restore(&s, 0);
    finish_docs(&s);
    if(pid < 0) return STATUS_CANNOT;
    pid = wait_program(pid, 0);
    return pid ? pid : status;
}

static int exec_node(struct node *n, int flags) {
    struct saved s;
    int status;
    if(exit_requested) return last_status;
    switch(n->type) {
    case N_SIMPLE:
        return exec_simple(n, flags, -1, -1, NULL);
    case N_PIPE:
        return exec_pipe(n, flags);
    case N_AND:
    case N_OR:
        status = exec_node(n->a, (flags & ~X_TOP) | X_COND);
        if(exit_requested || (n->type == N_AND ? status != 0 : status == 0)) return status;
        last_status = status;
        return exec_node(n->b, flags);
    case N_NOT:
        return exec_node(n->a, (flags & ~X_TOP) | X_COND) ? 0 : 1;
    case N_SEQ:
        status = exec_node(n->a, flags);
        if(exit_requested) return status;
        last_status = status;
        return exec_node(n->b, flags);
    case N_BG:
        return exec_background(n->a);
    case N_GROUP:
        memset(&s, 0, sizeof(s));
        if(redirect(n->redirs, &s, 0) < 0) status = STATUS_FAIL;
        else status = exec_node(n->a, flags);
        finish_docs(&s);
        return restore(&s, status);
    case N_SUB:
        return exec_sub(n);
    }
    return STATUS_FAIL;
}

int exec_tree(struct node *n, int flags) {
    int status = exec_node(n, flags);
    fflush(stdout);
    return status;
}
