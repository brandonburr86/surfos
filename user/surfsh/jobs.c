/*
SurfOS shell: surfsh
--------------------
File: jobs.c    Date: 10/24/26
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
*/

#include "surfsh.h"

#define JOBS_MAX 32

struct job {
    int id;                         /* %id, 1 and up */
    int pids[STAGES_LIMIT];
    int done[STAGES_LIMIT];
    int n, left;                    /* its processes, those still running */
    int status;                     /* its last process's, once it ended */
    char *text;                     /* what was typed: the command's first line */
};

static struct job *jobs[JOBS_MAX];
static int last_pid;

int jobs_last_pid(void) {
    return last_pid;
}

void job_add(const char *text, const int *pids, int n) {
    struct job *j;
    int i, id, slot = -1;
    if(n <= 0) return;
    last_pid = pids[n - 1];
    for(id = 1; id <= JOBS_MAX; id++) {             /* the smallest number no job has */
        for(i = 0; i < JOBS_MAX && !(jobs[i] && jobs[i]->id == id); i++);
        if(i == JOBS_MAX) break;
    }
    for(i = 0; i < JOBS_MAX && slot < 0; i++) if(!jobs[i]) slot = i;
    if(slot < 0) {                                  /* no room: reported by pid when it ends */
        if(interactive) printf("[%d] started\n", last_pid);
        fflush(stdout);
        return;
    }
    j = xmalloc(sizeof(*j));
    memset(j, 0, sizeof(*j));
    j->id = id;
    j->n = j->left = n > STAGES_LIMIT ? STAGES_LIMIT : n;
    for(i = 0; i < j->n; i++) j->pids[i] = pids[i];
    j->text = xstrdup(text);
    jobs[slot] = j;
    if(interactive) printf("[%d] started (pid %d)\n", id, last_pid);
    fflush(stdout);
}

static void report(const char *head, int status, const char *text) {
    if(status == -1) printf("%s killed  %s\n", head, text);
    else if(status) printf("%s exit status %d  %s\n", head, status, text);
    else printf("%s done  %s\n", head, text);
}

void jobs_collect(int fresh) {
    int pid, status, any = 0, i, k;
    char head[24];
    while((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        struct job *j = NULL;
        for(i = 0; i < JOBS_MAX && !j; i++) {
            if(!jobs[i]) continue;
            for(k = 0; k < jobs[i]->n; k++) {
                if(jobs[i]->pids[k] == pid && !jobs[i]->done[k]) {
                    j = jobs[i];
                    break;
                }
            }
        }
        if(j) {
            j->done[k] = 1;
            j->left--;
            if(k == j->n - 1) j->status = status;
            if(j->left) continue;
        }
        if(!interactive) {                          /* a script's jobs end without a word */
            if(j) {
                for(i = 0; i < JOBS_MAX; i++) if(jobs[i] == j) jobs[i] = NULL;
                free(j->text);
                free(j);
            }
            continue;
        }
        if(!any && fresh) edit_fresh_row();         /* the first report starts on a fresh row */
        any = 1;
        if(!j) {                                    /* no job's */
            snprintf(head, sizeof(head), "[%d]", pid);
            report(head, status, "");
            continue;
        }
        snprintf(head, sizeof(head), "[%d]", j->id);
        report(head, j->status, j->text);
        for(i = 0; i < JOBS_MAX; i++) if(jobs[i] == j) jobs[i] = NULL;
        free(j->text);
        free(j);
    }
    if(any) fflush(stdout);
}
