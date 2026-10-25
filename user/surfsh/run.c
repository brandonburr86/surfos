/*
SurfOS shell: surfsh
--------------------
File: run.c     Date: 10/6/26, 10/24/26
--------------------
One command. The name decides: surfsh's own commands (builtins.c), then a program, then the
kernel's (SYS_kcmd, include/surfos/kcmd.h); a name with a / is always a program's path, any
other name is looked for in the directories of PATH (10/24/26; /initrd/bin when it is not
set). 10/25/26 (track A): programs come before the kernel's commands of the same names, which
came first until then, in the ring-0 shell's order. So ls, cat, cp and df are the programs;
kernel NAME runs the kernel's command (script.c), and type NAME says which one runs
(builtins.c). exec.c decides which applies and makes the redirections; this file runs them.

Kernel commands run in this task, as they did in the ring-0 shell; the kernel's table
(SYS_kcmdinfo) is read once at the start. A kernel without the bridge answers ENOSYS, and
then no name is a kernel command. A name in the table is the kernel's, whatever kcmd()
answers. A kernel command prints on the console, or, when descriptor 1 is a file or a pipe
(a redirection), the kernel keeps its output and writes it there when it is done. The kernel
takes a path or an argument of at most 255 bytes: exec.c refuses a longer word with that
reason rather than leaving it to the kernel's EFAULT.

A program runs with this shell's descriptors 0, 1 and 2 as they are (SYS_spawnx) and the
exported variables as its environment (vars.c). In the foreground it is waited for: its exit status is the command's
("[exit status n]" when it is not 0, said for a command of its own at the prompt); a program
the kernel killed counts as 137 ("[pid n killed]"). Those lines come after paging ends, so
q at --More-- does not hide them. A program started in the background (SPAWN_BG) is not in
the foreground of the terminal: Ctrl+C there does not reach it. The pager pages every line
typed at the prompt (main.c).

10/25/26 (scripts): a program that Ctrl+C killed counts as 130 (waitx says so), as does a kernel
command it cut short, and the rest of the line, loop or script is left (exec.c). A file that
is neither a program nor a #! script (the kernel answers ENOEXEC) runs as a surfsh script, as
POSIX's shells do; a #! line that names something that cannot run is said as such.
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
    if(r == KCMD_INTERRUPTED) note_interrupt(1);    /* 10/25/26: cut short by Ctrl+C, which it said */
    return r;
}

/**** the programs of PATH ****/

static int cand_cmp(const void *a, const void *b) {
    return strcmp(((const struct cand *)a)->name, ((const struct cand *)b)->name);
}

/* PATH's directories one after the other: dir gets the next (an empty one is "."); 0 at the end */
static int path_next(const char **at, char *dir, int size) {
    const char *p = *at, *colon;
    int n;
    if(!p) return 0;
    colon = strchr(p, ':');
    n = colon ? (int)(colon - p) : (int)strlen(p);
    if(n >= size) n = size - 1;
    if(n) memcpy(dir, p, (size_t)n);
    else dir[n++] = '.';
    dir[n] = 0;
    *at = colon ? colon + 1 : NULL;
    return 1;
}

static const char *path_list(void) {
    const char *p = var_get("PATH");
    return p ? p : PROGRAM_DIR;
}

int program_names(struct cands *c) {
    const char *at = path_list();
    char dir[PATH_MAX];
    int any = 0, i, n = 0;
    while(path_next(&at, dir, sizeof(dir))) {
        DIR *d = opendir(dir);
        struct dirent *de;
        if(!d) continue;
        any = 1;
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
    }
    if(!any) return -1;
    qsort(c->v, (size_t)c->n, sizeof(struct cand), cand_cmp);
    for(i = 0; i < c->n; i++) {                     /* a name in two directories: once */
        if(n && !strcmp(c->v[n - 1].name, c->v[i].name)) free(c->v[i].name);
        else c->v[n++] = c->v[i];
    }
    c->n = n;
    return 0;
}

/* 10/25/26: the program a name runs, saying nothing (exec.c asks before a kernel command, type and
   help to tell): a path as it is, any other name in PATH's directories (/initrd/bin when PATH is not
   set), the first that has it as a file; 0 and its path in path, -1 when there is none */
int program_path(const char *name, char *path, int size) {
    struct stat st;
    const char *at = path_list();
    char dir[PATH_MAX];
    int n;
    if(strchr(name, '/')) {
        n = snprintf(path, (size_t)size, "%s", name);
        return n < size && stat(path, &st) == 0 && S_ISREG(st.type) ? 0 : -1;
    }
    while(path_next(&at, dir, sizeof(dir))) {
        n = snprintf(path, (size_t)size, "%s/%s", dir, name);
        if(n < size && stat(path, &st) == 0 && S_ISREG(st.type)) return 0;
    }
    return -1;
}

/* the program a name runs, as program_path() finds it; 0 and its path in path, or the status (said) */
int find_program(const char *name, char *path, int size) {
    struct stat st;
    int n;
    if(!strchr(name, '/')) {
        if(program_path(name, path, size) == 0) return 0;
        fprintf(stderr, "surfsh: %s: command not found\n", name);
        return STATUS_NOTFOUND;
    }
    n = snprintf(path, (size_t)size, "%s", name);
    if(n >= size || stat(path, &st) < 0) {
        fprintf(stderr, "surfsh: %s: %s\n", name, strerror(n >= size ? ENAMETOOLONG : errno));
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

/* 10/25/26: the interpreter a file's #! line names, in line; 0 when it has no #! */
static int hashbang(const char *path, char *line, int size) {
    int fd = open(path, O_RDONLY), n, i;
    if(fd < 0) return 0;
    n = (int)read(fd, line, (size_t)size - 1);
    close(fd);
    if(n < 2 || line[0] != '#' || line[1] != '!') return 0;
    line[n] = 0;
    for(i = 2; line[i] == ' ' || line[i] == '\t'; i++);
    memmove(line, line + i, strlen(line + i) + 1);
    for(i = 0; line[i] && line[i] != ' ' && line[i] != '\t' && line[i] != '\n' && line[i] != '\r'; i++);
    line[i] = 0;
    return 1;
}

int start_program(const char *path, char **argv, int in, int out, int background, char **env, int fd3) {
    struct spawnx o;
    char line[128];
    int pid, e;
    memset(&o, 0, sizeof(o));
    o.fds[0] = fd_for_child(in, 0);
    o.fds[1] = fd_for_child(out, 1);
    o.fds[2] = fd_for_child(-1, 2);
    o.fds[3] = fd3;                             /* 10/25/26: a descriptor 3 the user made (exec.c) */
    o.envp = env;
    o.job = job_spawn_value();                  /* 10/25/26: its job (jobs.c) */
    o.flags = background ? SPAWN_BG : 0;
    fflush(stdout);
    pid = spawnx(path, argv, &o);
    e = errno;
    if(pid < 0 && (e == ENOEXEC || e == ENOENT || e == EACCES) && hashbang(path, line, sizeof(line))) {
        fprintf(stderr, "surfsh: %s: its #! line names %s, which cannot run: %s\n", argv[0], line[0] ? line : "nothing",
                strerror(e == ENOEXEC ? ENOEXEC : e));
        return -1;
    }
    if(pid < 0 && e == ENOEXEC) {               /* a script without #!: a new surfsh runs it */
        char *sargv[MAX_WORDS + 2];
        int i;
        sargv[0] = "surfsh";
        sargv[1] = (char *)path;
        for(i = 1; argv[i] && i < MAX_WORDS; i++) sargv[i + 1] = argv[i];
        sargv[i + 1] = NULL;
        pid = spawnx(SURFSH_PATH, sargv, &o);
        e = errno;
    }
    if(pid < 0) fprintf(stderr, "surfsh: %s: %s\n", argv[0], strerror(e));
    job_spawned(pid);
    return pid;
}

/* a line about a program that ended, after paging ends so that q does not hide it. 10/25/26: on the
   terminal (main.c's term_fd), not into a file the command's output goes to */
static void say(const char *fmt, int n) {
    int was = paging;
    if(was) page(0);
    fflush(stdout);
    dprintf(term_fd, fmt, n);
    if(was) page(1);
}

/* 10/25/26: waitx says how it ended: one Ctrl+C killed is 130 and stops the rest (exec.c). untraced
   (job control): a stop is said too, STATUS_STOPPED, and why it stopped in *why (WX_STOP_*); *why is
   -1 when it did not stop (a program's own exit 148 is no stop) */
int wait_program_ex(int pid, int report, int untraced, int *why) {
    struct waitx w;
    int r;
    if(why) *why = -1;
    while((r = waitx(pid, &w, untraced ? WUNTRACED : 0)) < 0 && errno == EINTR)
        note_interrupt(0);                      /* a Ctrl+C for this shell alone: the program is waited for still */
    if(r != pid) {
        fprintf(stderr, "surfsh: waiting for %d: %s\n", pid, strerror(errno));
        return STATUS_FAIL;
    }
    if(w.how == WX_STOPPED) {
        if(why) *why = w.status;
        return STATUS_STOPPED;
    }
    if(w.how != WX_EXITED) {
        if(report) say("[pid %d killed]\n", pid);
        if(w.how == WX_KILLED) return STATUS_KILLED;
        note_interrupt(report);
        return STATUS_INTERRUPTED;
    }
    w.status &= 0xff;                           /* 10/25/26: 0-255, as a shell's (exit(-1) is 255) */
    if(w.status && report) {
        say("[exit status %d]\n", w.status);
        if(w.status == STATUS_INTERRUPTED) interrupt_said = 1;     /* a script Ctrl+C ended */
    }
    return w.status;
}

int wait_program(int pid, int report) {
    return wait_program_ex(pid, report, 0, NULL);
}
