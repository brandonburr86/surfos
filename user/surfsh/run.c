/*
SurfOS shell: surfsh
--------------------
File: run.c     Date: 10/6/26
--------------------
Running a command. The name decides, in the ring-0 shell's order: surfsh's own
commands (builtins.c), then the kernel's (SYS_kcmd, include/surfos/kcmd.h), then
a program; a name with a / is always a program's path, any other name is
/initrd/bin/<name>. So ls, cat and cp are the kernel's, and the programs of the
same names are reached by their paths (/initrd/bin/ls -l).

Kernel commands run in this task, as they did in the ring-0 shell; the kernel's
table (SYS_kcmdinfo) is read once at the start. A kernel without the bridge
answers -ENOSYS, and then no name is a kernel command. "> file" and ">> file" put
the file on descriptor 1 for the command (the kernel writes its output there when
it is done), and descriptor 1 is put back afterwards; "<" is refused, since a
kernel command reads only the console. surfsh's own commands are redirected the
same way. Programs get their files through spawnfd(), the console otherwise.

Every command in the foreground runs with the pager on (ttyctl(TTY_PAGE, 1)) and
off again afterwards. A program in the foreground is waited for: its exit status
is the command's ("[exit status n]" when it is not 0); a program the kernel
killed counts as 137 ("[pid n killed]"). A program started with & is not waited
for ("[pid] started"); it is collected while a later program is waited for, and
how it ended is shown then ("[pid] done", "[pid] exit status n", "[pid] killed").
*/

#include "surfsh.h"

#define NOT_KERNEL   (-1000)        /* run_kernel: the name is not a kernel command */
#define SAVED_STDOUT 15             /* NR_OPEN - 1: descriptor 1 as surfsh was started with it */
#define MAX_NOTES    16

static struct kcmd_info *ktable;    /* the kernel's commands, as SYS_kcmdinfo lists them */
static int nk, bridge;
static int saved_out = -1;

/* background programs that ended while surfsh waited for another one */
static struct { int pid, status; } notes[MAX_NOTES];
static int nnotes, lost_notes;

/**** the kernel's commands ****/

void run_init(void) {
    struct kcmd_info info;
    int i, cap = 0;
    for(i = 0; i < 1024; i++) {
        if(kcmdinfo(i, &info) < 0) {
            if(errno == ENOENT) bridge = 1;     /* the end of the table; ENOSYS: no bridge */
            break;
        }
        bridge = 1;
        if(nk == cap) {
            struct kcmd_info *t = realloc(ktable, (size_t)(cap ? cap * 2 : 64) * sizeof(*t));
            if(!t) break;
            ktable = t;
            cap = cap ? cap * 2 : 64;
        }
        info.name[KCMD_NAME_MAX - 1] = info.args[KCMD_ARGS_MAX - 1] = info.help[KCMD_HELP_MAX - 1] = 0;
        ktable[nk++] = info;
    }
    /* descriptor 1 as it is now: what a redirection gives back (dup2 onto a free slot is a dup) */
    if(dup2(1, SAVED_STDOUT) == SAVED_STDOUT) saved_out = SAVED_STDOUT;
}

int kcmd_bridge(void) { return bridge; }
int kcmd_count(void) { return nk; }
const struct kcmd_info *kcmd_entry(int i) { return i >= 0 && i < nk ? &ktable[i] : NULL; }

const struct kcmd_info *kcmd_find(const char *name) {
    int i;
    for(i = 0; i < nk; i++) if(!strcmp(ktable[i].name, name)) return &ktable[i];
    return NULL;
}

/**** the programs in /initrd/bin ****/

static int cand_cmp(const void *a, const void *b) {
    return strcmp(((const struct cand *)a)->name, ((const struct cand *)b)->name);
}

int program_names(struct cands *c) {
    DIR *d = opendir(PROGRAM_DIR);
    struct dirent *de;
    if(!d) return -1;
    while((de = readdir(d))) {
        char *name;
        if(de->d_type != DT_REG || de->d_name[0] == '.') continue;
        if(c->n == c->cap) {
            struct cand *v = realloc(c->v, (size_t)(c->cap ? c->cap * 2 : 32) * sizeof(*v));
            if(!v) break;
            c->v = v;
            c->cap = c->cap ? c->cap * 2 : 32;
        }
        if(!(name = strdup(de->d_name))) break;
        c->v[c->n].name = name;
        c->v[c->n].dir = 0;
        c->n++;
    }
    closedir(d);
    qsort(c->v, (size_t)c->n, sizeof(struct cand), cand_cmp);
    return 0;
}

/**** redirection and paging ****/

static void page(int on) {
    fflush(stdout);
    ttyctl(TTY_PAGE, (unsigned int)on);     /* inert until the console's pager exists */
}

static int open_out(const struct command *c) {
    int fd = open(c->out, O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC));
    if(fd < 0) fprintf(stderr, "surfsh: %s: %s\n", c->out, strerror(errno));
    return fd;
}

/* a < file must exist even where nothing reads it */
static int check_in(const struct command *c) {
    int fd = open(c->in, O_RDONLY);
    if(fd < 0) {
        fprintf(stderr, "surfsh: %s: %s\n", c->in, strerror(errno));
        return -1;
    }
    close(fd);
    return 0;
}

/* > file and >> file for a command that runs in this task: the file becomes descriptor 1 */
static int redirect_out(const struct command *c) {
    int fd = open_out(c);
    if(fd < 0) return -1;
    fflush(stdout);
    if(fd != 1 && dup2(fd, 1) < 0) {
        fprintf(stderr, "surfsh: %s: %s\n", c->out, strerror(errno));
        close(fd);
        return -1;
    }
    if(fd != 1) close(fd);
    return 0;
}

/* descriptor 1 as it was: the copy kept at the start, or the console */
static void restore_out(void) {
    fflush(stdout);
    if(saved_out >= 0 && dup2(saved_out, 1) == 1) return;
    {
        int fd = open("/dev/console", O_WRONLY);
        if(fd >= 0 && fd != 1) {
            dup2(fd, 1);
            close(fd);
        }
    }
}

/**** surfsh's own commands ****/

static int run_builtin(const struct builtin *b, struct command *c) {
    int status;
    if(c->bg) {
        fprintf(stderr, "surfsh: %s is surfsh's own command: only programs run in the background (&)\n", c->argv[0]);
        return STATUS_USAGE;
    }
    if(c->in && check_in(c)) return STATUS_FAIL;
    if(c->out && redirect_out(c)) return STATUS_FAIL;
    page(1);
    status = b->fn(c->argc, c->argv);
    page(0);
    if(c->out) restore_out();
    return status;
}

/**** the kernel's commands ****/

static int run_kernel(struct command *c) {
    int r, e;
    if(bridge && !kcmd_find(c->argv[0])) return NOT_KERNEL;
    if(!bridge && (c->in || c->out || c->bg)) return NOT_KERNEL;   /* without the bridge only a plain try */
    if(c->bg) {
        fprintf(stderr, "surfsh: %s is a kernel command: only programs run in the background (&)\n", c->argv[0]);
        return STATUS_USAGE;
    }
    if(c->in) {
        fprintf(stderr, "surfsh: %s is a kernel command, which reads only the console: no < here\n", c->argv[0]);
        return STATUS_USAGE;
    }
    if(c->out && redirect_out(c)) return STATUS_FAIL;
    page(1);
    r = kcmd(c->argv);
    e = errno;
    page(0);
    if(c->out) restore_out();
    if(r < 0) {
        if(e == ENOENT || e == ENOSYS) return NOT_KERNEL;
        fprintf(stderr, "surfsh: %s: %s\n", c->argv[0], strerror(e));
        return STATUS_FAIL;
    }
    return r;
}

/**** programs ****/

static void note_background(int pid, int status) {
    if(nnotes == MAX_NOTES) { lost_notes++; return; }
    notes[nnotes].pid = pid;
    notes[nnotes].status = status;
    nnotes++;
}

static void show_notes(void) {
    int i;
    for(i = 0; i < nnotes; i++) {
        if(notes[i].status == -1) printf("[%d] killed\n", notes[i].pid);
        else if(notes[i].status) printf("[%d] exit status %d\n", notes[i].pid, notes[i].status);
        else printf("[%d] done\n", notes[i].pid);
    }
    if(lost_notes) printf("(and %d more programs that ran in the background ended)\n", lost_notes);
    nnotes = lost_notes = 0;
    fflush(stdout);
}

/* the foreground program's status; programs from the background that end meanwhile are collected */
static int wait_for(int pid) {
    int status = 0, r;
    for(;;) {
        r = wait(-1, &status);
        if(r < 0) {
            fprintf(stderr, "surfsh: waiting for %d: %s\n", pid, strerror(errno));
            return STATUS_FAIL;
        }
        if(r == pid) break;
        note_background(r, status);
    }
    if(status == -1) {
        printf("[pid %d killed]\n", pid);
        return STATUS_KILLED;
    }
    if(status) printf("[exit status %d]\n", status);
    return status;
}

static int run_program(struct command *c) {
    char path[PATH_MAX];
    struct stat st;
    int fds[3] = { -1, -1, -1 }, pid, e, status, n;
    const char *name = c->argv[0];

    if(strchr(name, '/')) n = snprintf(path, sizeof(path), "%s", name);
    else n = snprintf(path, sizeof(path), PROGRAM_DIR "/%s", name);
    if(n >= (int)sizeof(path) || stat(path, &st) < 0) {
        if(strchr(name, '/')) fprintf(stderr, "surfsh: %s: %s\n", name, n >= (int)sizeof(path) ? "the path is too long" : strerror(errno));
        else fprintf(stderr, "surfsh: %s: command not found\n", name);
        return STATUS_NOTFOUND;
    }
    if(!S_ISREG(st.type)) {
        fprintf(stderr, "surfsh: %s: is a directory\n", name);
        return STATUS_CANNOT;
    }
    if(c->in && (fds[0] = open(c->in, O_RDONLY)) < 0) {
        fprintf(stderr, "surfsh: %s: %s\n", c->in, strerror(errno));
        return STATUS_FAIL;
    }
    if(c->out && (fds[1] = open_out(c)) < 0) {
        if(fds[0] >= 0) close(fds[0]);
        return STATUS_FAIL;
    }
    fflush(stdout);
    if(!c->bg) page(1);
    pid = (c->in || c->out) ? spawnfd(path, c->argv, fds) : spawn(path, c->argv);
    e = errno;
    if(fds[0] >= 0) close(fds[0]);          /* the program has its own references */
    if(fds[1] >= 0) close(fds[1]);
    if(pid < 0) {
        if(!c->bg) page(0);
        fprintf(stderr, "surfsh: %s: %s\n", name, strerror(e));
        return STATUS_CANNOT;
    }
    if(c->bg) {
        printf("[%d] started\n", pid);
        fflush(stdout);
        return 0;
    }
    status = wait_for(pid);
    page(0);
    if(nnotes || lost_notes) show_notes();
    return status;
}

/**** a line ****/

static int run_command(struct command *c) {
    const struct builtin *b;
    int r;
    if(!strchr(c->argv[0], '/')) {
        if((b = builtin_find(c->argv[0]))) return run_builtin(b, c);
        r = run_kernel(c);
        if(r != NOT_KERNEL) return r;
    }
    return run_program(c);
}

int run_line(const char *line) {
    static struct parsed p;
    char err[120];
    int i, status = last_status;
    if(parse_line(line, &p, err, sizeof(err))) {
        fprintf(stderr, "surfsh: %s\n", err);
        return STATUS_USAGE;
    }
    for(i = 0; i < p.ncmd && !exit_requested; i++) {
        status = run_command(&p.cmd[i]);
        fflush(stdout);
    }
    return status;
}
