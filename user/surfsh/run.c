/*
SurfOS shell: surfsh
--------------------
File: run.c     Date: 10/6/26, 10/24/26
--------------------
One command. The name decides, in the ring-0 shell's order: surfsh's own commands
(builtins.c), then the kernel's (SYS_kcmd, include/surfos/kcmd.h), then a program; a name
with a / is always a program's path, any other name is /initrd/bin/<name>. So ls, cat and
cp are the kernel's, and the programs of the same names are reached by their paths
(/initrd/bin/ls -l). exec.c decides which applies and makes the redirections; this file runs
them.

Kernel commands run in this task, as they did in the ring-0 shell; the kernel's table
(SYS_kcmdinfo) is read once at the start. A kernel without the bridge answers ENOSYS, and
then no name is a kernel command. A name in the table is the kernel's, whatever kcmd()
answers. A kernel command prints on the console, or, when descriptor 1 is a file or a pipe
(a redirection), the kernel keeps its output and writes it there when it is done. The kernel
takes a path or an argument of at most 255 bytes: exec.c refuses a longer word with that
reason rather than leaving it to the kernel's EFAULT.

A program runs with this shell's descriptors 0, 1 and 2 as they are (SYS_spawnx) and this
shell's environment. In the foreground it is waited for: its exit status is the command's
("[exit status n]" when it is not 0, said for a command of its own at the prompt); a program
the kernel killed counts as 137 ("[pid n killed]"). Those lines come after paging ends, so
q at --More-- does not hide them. A program started in the background (SPAWN_BG) is not in
the foreground of the terminal: Ctrl+C there does not reach it. The pager pages every line
typed at the prompt (main.c).
*/

#include "surfsh.h"

static struct kcmd_info *ktable;    /* the kernel's commands, as SYS_kcmdinfo lists them */
static int nk, bridge;
static int paging;                  /* page(1) is in effect */

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
}

int kcmd_bridge(void) { return bridge; }
int kcmd_count(void) { return nk; }
const struct kcmd_info *kcmd_entry(int i) { return i >= 0 && i < nk ? &ktable[i] : NULL; }

const struct kcmd_info *kcmd_find(const char *name) {
    int i;
    for(i = 0; i < nk; i++) if(!strcmp(ktable[i].name, name)) return &ktable[i];
    return NULL;
}

/* a kernel command in this task: its status; *found 0 when the kernel has no such command after
   all (a kernel without the bridge, which leaves the name to the programs) */
int run_kernel(char **argv, int *found) {
    int r, e;
    *found = 1;
    fflush(stdout);
    r = kcmd(argv);
    e = errno;
    if(r < 0) {
        if(e == ENOSYS || (e == ENOENT && !kcmd_find(argv[0]))) {
            *found = 0;
            return 0;
        }
        fprintf(stderr, "surfsh: %s: %s\n", argv[0], strerror(e));
        return STATUS_FAIL;
    }
    return r;
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

/* the program a name runs: its path in path; 0, or the status (said) when there is none */
int find_program(const char *name, char *path, int size) {
    struct stat st;
    int n;
    if(strchr(name, '/')) n = snprintf(path, (size_t)size, "%s", name);
    else n = snprintf(path, (size_t)size, PROGRAM_DIR "/%s", name);
    if(n >= size || stat(path, &st) < 0) {
        if(strchr(name, '/')) fprintf(stderr, "surfsh: %s: %s\n", name, strerror(n >= size ? ENAMETOOLONG : errno));
        else fprintf(stderr, "surfsh: %s: command not found\n", name);
        return STATUS_NOTFOUND;
    }
    if(!S_ISREG(st.type)) {
        fprintf(stderr, "surfsh: %s: is a directory\n", name);
        return STATUS_CANNOT;
    }
    return 0;
}

/**** programs ****/

void page(int on) {
    fflush(stdout);
    ttyctl(TTY_PAGE, (unsigned int)on);
    paging = on;
}

static int fd_for_child(int fd, int mine) {
    struct stat st;
    if(fd >= 0) return fd;
    return fstat(mine, &st) == 0 ? mine : SPAWN_CLOSED;
}

int start_program(const char *path, char **argv, int in, int out, int background) {
    struct spawnx o;
    int pid;
    memset(&o, 0, sizeof(o));
    o.fds[0] = fd_for_child(in, 0);
    o.fds[1] = fd_for_child(out, 1);
    o.fds[2] = fd_for_child(-1, 2);
    o.fds[3] = -1;
    o.envp = environ;
    o.flags = background ? SPAWN_BG : 0;
    fflush(stdout);
    pid = spawnx(path, argv, &o);
    if(pid < 0) fprintf(stderr, "surfsh: %s: %s\n", argv[0], strerror(errno));
    return pid;
}

/* a line about a program that ended, after paging ends so that q does not hide it */
static void say(const char *fmt, int n) {
    int was = paging;
    if(was) page(0);
    printf(fmt, n);
    fflush(stdout);
    if(was) page(1);
}

int wait_program(int pid, int report) {
    int status = 0;
    if(waitpid(pid, &status, 0) != pid) {
        fprintf(stderr, "surfsh: waiting for %d: %s\n", pid, strerror(errno));
        return STATUS_FAIL;
    }
    if(status == -1) {
        if(report) say("[pid %d killed]\n", pid);
        return STATUS_KILLED;
    }
    if(status && report) say("[exit status %d]\n", status);
    return status;
}
