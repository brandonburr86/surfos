/*
SurfOS shell: surfsh
--------------------
File: jobs.c    Date: 10/24/26, 10/25/26
--------------------
The jobs in the background: a command started with & (a program, a pipeline's processes, or
a new surfsh that runs the command) is a job with a number, the smallest one free. Starting
it says "[1] started (pid 42)", the pid of its last process ($! later). Before each prompt,
and after each line of a file, the processes that ended are collected (waitpid() with
WNOHANG), and a job whose processes have all ended is reported, with its first line:
"[1] done  count 1 200", "[1] exit status 3  ...", "[1] killed  ..." (its last process says
which). A process of this shell's that is in no job (there should be none) is reported by its
pid, as surfsh did before it had jobs. A shell that runs a script (or a new surfsh) says
nothing about its jobs, as other shells.

10/25/26, job control (an interactive surfsh; include/surfos/job.h). Each command it starts
as processes is a job of the kernel's (SYS_spawnx: SPAWN_JOB_NEW for the first process, the
others join it; job_spawn_value() and job_spawned() tell run.c and exec.c which), and while
it waits for one in the foreground that job is the terminal's foreground job (SYS_job). Ctrl+Z
stops it: the job joins the table, stopped ("[1] stopped  cmd"), and the rest of the line,
loop or function is left (status 148, 128 + SIGTSTP's 20). A job in the background that reads
the terminal stops and is reported before the next prompt ("[1] stopped (reading the
terminal)  cmd"). The builtins of jobs:

    jobs              the jobs: number, state, command
    fg [%n]           job n (the last one) in the foreground again: waited for, stopped again
                      by Ctrl+Z
    bg [%n]           job n goes on in the background
    wait [%n|pid]...  for those to end (none: every job that runs); Ctrl+C ends the wait
    kill %n|pid...    job n's processes killed, the stopped ones too; a pid: as the kernel's kill

%n is a job's number, %% and %+ the last job, %- the one before; a job given by its pid works too.
Commands in a pipeline whose last stage runs in surfsh, $( ) and here-documents' feeders are
in no job of their own (Ctrl+Z does nothing to them), nor is what a script starts.
*/

#include "surfsh.h"

#define JOBS_MAX 32

struct job {
    int id;                         /* %id, 1 and up */
    int kid;                        /* the kernel's job (its first process's pid), 0 for none */
    int pids[STAGES_LIMIT];
    int done[STAGES_LIMIT];
    int n, left;                    /* its processes, those still running */
    int status;                     /* its last process's, once it ended */
    int stopped;                    /* 0 running; WX_STOP_KEY + 1, WX_STOP_READ + 1: stopped, and why */
    int said;                       /* the stop was said */
    int order;                      /* when it was started or stopped last: %+ is the latest */
    char *text;                     /* what was typed: the command's first line */
};

static struct job *jobs[JOBS_MAX];
static int last_pid, order;
static int next_job, next_kid;      /* job_spawn_value(): the job the next process goes into */

int jobs_last_pid(void) {
    return last_pid;
}

void jobs_set_last(int pid) {
    last_pid = pid;
}

/**** the kernel's jobs for what is started ****/

/* what is started from here on, until job_close(), is one new job (an interactive shell's) */
void job_open(void) {
    next_job = interactive ? SPAWN_JOB_NEW : 0;
    next_kid = 0;
}

/* the job that was started (0: none, or not one of its own), and no more */
int job_close(void) {
    int k = next_kid;
    next_job = next_kid = 0;
    return k;
}

/* spawnx's job for the next process; job_spawned() with its pid once it started. Outside job_open()
   an interactive shell starts what it starts in no job (a $( ), a pipeline that ends in this shell):
   inheriting its own (a surfsh typed at a surfsh's prompt is in one) would let Ctrl+Z stop them. */
int job_spawn_value(void) {
    if(next_job) return next_job;
    return interactive ? SPAWN_JOB_NONE : 0;
}

void job_spawned(int pid) {
    if(next_job == SPAWN_JOB_NEW && pid > 0) {
        next_kid = pid;
        next_job = pid;                         /* the others join the first */
    }
}

/* $( ) inside a job being started: what it starts is in no job; job_unhold() goes on with the job */
void job_hold(struct jobwin *w) {
    w->next = next_job;
    w->kid = next_kid;
    next_job = next_kid = 0;
}

void job_unhold(struct jobwin *w) {
    next_job = w->next;
    next_kid = w->kid;
}

/**** the table ****/

static struct job *add(const char *text, const int *pids, int n, int kid) {
    struct job *j;
    int i, id, slot = -1;
    for(id = 1; id <= JOBS_MAX; id++) {             /* the smallest number no job has */
        for(i = 0; i < JOBS_MAX && !(jobs[i] && jobs[i]->id == id); i++);
        if(i == JOBS_MAX) break;
    }
    for(i = 0; i < JOBS_MAX && slot < 0; i++) if(!jobs[i]) slot = i;
    if(slot < 0) return NULL;
    j = xmalloc(sizeof(*j));
    memset(j, 0, sizeof(*j));
    j->id = id;
    j->kid = kid;
    j->n = j->left = n > STAGES_LIMIT ? STAGES_LIMIT : n;
    for(i = 0; i < j->n; i++) j->pids[i] = pids[i];
    j->text = xstrdup(text);
    j->order = ++order;
    jobs[slot] = j;
    return j;
}

static void drop(struct job *j) {
    int i;
    for(i = 0; i < JOBS_MAX; i++) if(jobs[i] == j) jobs[i] = NULL;
    free(j->text);
    free(j);
}

void job_add(const char *text, const int *pids, int n, int kid) {
    struct job *j;
    if(n <= 0) return;
    last_pid = pids[n - 1];
    if(!(j = add(text, pids, n, kid))) {            /* no room: reported by pid when it ends */
        if(interactive) printf("[%d] started\n", last_pid);
        fflush(stdout);
        return;
    }
    if(interactive) printf("[%d] started (pid %d)\n", j->id, last_pid);
    fflush(stdout);
}

static void say_stopped(struct job *j) {
    printf("[%d] stopped%s  %s\n", j->id, j->stopped == WX_STOP_READ + 1 ? " (reading the terminal)" : "", j->text);
    j->said = 1;
    fflush(stdout);
}

/* a job in the foreground stopped (WX_STOP_*): the processes from pids on are a job of the table,
   stopped, said, and the rest of the line, loop or function is left (exec.c); it may be one of the
   table already (fg) */
void job_stopped(const char *text, const int *pids, int n, int kid, int why) {
    struct job *j = NULL;
    int i;
    suspended = 1;
    for(i = 0; i < JOBS_MAX && !j; i++) if(jobs[i] && kid && jobs[i]->kid == kid) j = jobs[i];
    if(!j && !(j = add(text, pids, n, kid))) {
        fprintf(stderr, "surfsh: no room for another job: %s is killed\n", text);
        if(kid) jobctl(JOB_KILL, kid);
        return;
    }
    j->stopped = why + 1;
    j->order = ++order;
    edit_fresh_row();
    say_stopped(j);
}

static void report(const char *head, int status, const char *text) {
    if(status == -1) printf("%s killed  %s\n", head, text);
    else if(status) printf("%s exit status %d  %s\n", head, status, text);
    else printf("%s done  %s\n", head, text);
}

/* the job that has pid among its processes still running, and its index there; NULL for none */
static struct job *job_of(int pid, int *k) {
    int i;
    for(i = 0; i < JOBS_MAX; i++) {
        if(!jobs[i]) continue;
        for(*k = 0; *k < jobs[i]->n; (*k)++) if(jobs[i]->pids[*k] == pid && !jobs[i]->done[*k]) return jobs[i];
    }
    return NULL;
}

/* process k of job j ended with status (-1: killed): 1 when j has ended with it (not freed yet) */
static int ended(struct job *j, int k, int status) {
    j->done[k] = 1;
    j->left--;
    if(k == j->n - 1) j->status = status;
    return !j->left;
}

void jobs_collect(int fresh) {
    struct waitx w;
    int pid, any = 0, k;
    char head[24];
    while((pid = waitx(-1, &w, WNOHANG | WUNTRACED)) > 0) {
        struct job *j = job_of(pid, &k);
        if(w.how == WX_STOPPED) {                   /* 10/25/26: a job in the background stopped */
            if(!j || (j->stopped && j->said)) continue;
            j->stopped = w.status + 1;
            if(!interactive) continue;
            if(!any && fresh) edit_fresh_row();
            any = 1;
            say_stopped(j);
            continue;
        }
        if(j && !ended(j, k, w.how == WX_EXITED ? w.status : -1)) continue;
        if(!interactive) {                          /* a script's jobs end without a word */
            if(j) drop(j);
            continue;
        }
        if(!any && fresh) edit_fresh_row();         /* the first report starts on a fresh row */
        any = 1;
        if(!j) {                                    /* no job's */
            snprintf(head, sizeof(head), "[%d]", pid);
            report(head, w.how == WX_EXITED ? w.status : -1, "");
            continue;
        }
        snprintf(head, sizeof(head), "[%d]", j->id);
        report(head, j->status, j->text);
        drop(j);
    }
    if(any) fflush(stdout);
}

/**** the builtins ****/

/* %n, %%, %+, %-, or a pid of one of its processes; NULL said */
static struct job *find(const char *cmd, const char *arg) {
    struct job *best = NULL, *second = NULL;
    int i, k;
    for(i = 0; i < JOBS_MAX; i++) {
        if(!jobs[i]) continue;
        if(!best || jobs[i]->order > best->order) {
            second = best;
            best = jobs[i];
        } else if(!second || jobs[i]->order > second->order) {
            second = jobs[i];
        }
    }
    if(!arg || !strcmp(arg, "%%") || !strcmp(arg, "%+") || !strcmp(arg, "%")) {
        if(!best) fprintf(stderr, "surfsh: %s: there is no job\n", cmd);
        return best;
    }
    if(!strcmp(arg, "%-")) {
        if(!second) fprintf(stderr, "surfsh: %s: there is no job before the last\n", cmd);
        return second;
    }
    if(arg[0] == '%') {
        char *end;
        long n = strtol(arg + 1, &end, 10);
        for(i = 0; i < JOBS_MAX && !*end && arg[1]; i++) if(jobs[i] && jobs[i]->id == n) return jobs[i];
    } else {
        char *end;
        long pid = strtol(arg, &end, 10);
        struct job *j = *end || !*arg ? NULL : job_of((int)pid, &k);
        if(j) return j;
    }
    fprintf(stderr, "surfsh: %s: %s: no such job\n", cmd, arg);
    return NULL;
}

static const char *state_of(struct job *j) {
    if(j->stopped == WX_STOP_READ + 1) return "stopped (reading the terminal)";
    return j->stopped ? "stopped" : "running";
}

int b_jobs(int argc, char **argv) {
    int i, id;
    (void)argv;
    if(argc > 1) {
        fprintf(stderr, "usage: jobs\n");
        return STATUS_USAGE;
    }
    jobs_collect(0);
    for(id = 1; id <= JOBS_MAX; id++) {             /* by number */
        for(i = 0; i < JOBS_MAX; i++) {
            if(jobs[i] && jobs[i]->id == id) printf("[%d]  %-8s %s\n", id, state_of(jobs[i]), jobs[i]->text);
        }
    }
    return 0;
}

/* job j in the foreground until it ends or stops again: the status of its last process, or 148 */
static int foreground(struct job *j) {
    int prev = j->kid ? jobctl(JOB_FG, j->kid) : 0, k, status = 0, why;
    j->stopped = j->said = 0;
    for(k = 0; k < j->n; k++) {
        int st;
        if(j->done[k]) continue;
        st = wait_program_ex(j->pids[k], 0, j->kid != 0, &why);
        if(st == STATUS_STOPPED) {
            job_stopped(j->text, j->pids, j->n, j->kid, why);
            status = st;
            break;
        }
        ended(j, k, st == STATUS_KILLED || st == STATUS_INTERRUPTED ? -1 : st);
        if(k == j->n - 1) status = st;
    }
    if(j->kid) jobctl(JOB_FG, prev > 0 ? prev : 0);
    if(status != STATUS_STOPPED && !j->left) drop(j);
    check_interrupt();
    return status;
}

int b_fg(int argc, char **argv) {
    struct job *j;
    if(argc > 2) {
        fprintf(stderr, "usage: fg [%%n]\n");
        return STATUS_USAGE;
    }
    if(!(j = find("fg", argc > 1 ? argv[1] : NULL))) return STATUS_FAIL;
    printf("%s\n", j->text);
    fflush(stdout);
    return foreground(j);
}

int b_bg(int argc, char **argv) {
    struct job *j;
    if(argc > 2) {
        fprintf(stderr, "usage: bg [%%n]\n");
        return STATUS_USAGE;
    }
    if(!(j = find("bg", argc > 1 ? argv[1] : NULL))) return STATUS_FAIL;
    if(j->kid && jobctl(JOB_BG, j->kid) < 0) {
        fprintf(stderr, "surfsh: bg: %%%d: %s\n", j->id, strerror(errno));
        return STATUS_FAIL;
    }
    j->stopped = j->said = 0;
    j->order = ++order;
    printf("[%d] %s &\n", j->id, j->text);
    return 0;
}

/* wait for job j's processes (Ctrl+C ends the wait: 130); the status of its last one; a stopped job
   is not waited for (148) */
static int wait_job(struct job *j) {
    int k, status = 0;
    if(j->stopped) {
        fprintf(stderr, "surfsh: wait: %%%d is stopped (fg or bg lets it go on)\n", j->id);
        return STATUS_STOPPED;
    }
    for(k = 0; k < j->n; k++) {
        struct waitx w;
        int r;
        if(j->done[k]) continue;
        if((r = waitx(j->pids[k], &w, 0)) < 0 && errno == EINTR) {
            note_interrupt(0);
            return STATUS_INTERRUPTED;
        }
        if(r < 0) return STATUS_FAIL;
        ended(j, k, w.how == WX_EXITED ? w.status : -1);
        if(k == j->n - 1)
            status = w.how == WX_EXITED ? w.status : w.how == WX_INTERRUPTED ? STATUS_INTERRUPTED : STATUS_KILLED;
    }
    drop(j);
    return status;
}

int b_wait(int argc, char **argv) {
    int i, status = 0;
    if(argc == 1) {                                 /* every job that runs */
        for(i = 0; i < JOBS_MAX && status != STATUS_INTERRUPTED; i++) {
            if(jobs[i] && !jobs[i]->stopped) status = wait_job(jobs[i]);
        }
        return status == STATUS_INTERRUPTED ? status : 0;
    }
    for(i = 1; i < argc; i++) {
        struct job *j = find("wait", argv[i]);
        if(!j) {
            status = STATUS_NOTFOUND;
            continue;
        }
        if((status = wait_job(j)) == STATUS_INTERRUPTED) break;
    }
    return status;
}

int b_kill(int argc, char **argv) {
    int i, status = 0;
    if(argc < 2) {
        fprintf(stderr, "usage: kill %%n|pid...\n");
        return STATUS_USAGE;
    }
    for(i = 1; i < argc; i++) {
        if(argv[i][0] == '%') {
            struct job *j = find("kill", argv[i]);
            if(!j) status = STATUS_FAIL;
            else if(!j->kid || jobctl(JOB_KILL, j->kid) < 0) {
                int k;
                for(k = 0; k < j->n; k++) if(!j->done[k]) kill(j->pids[k]);
            }
        } else {                                    /* a pid: as the kernel's kill does it */
            char *args[] = { "kill", argv[i], NULL };
            int found, r = run_kernel(args, &found);
            if(r) status = r;
        }
    }
    return status;
}
