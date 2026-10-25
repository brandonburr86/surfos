/*
SurfOS starting programs
----------------------
File: spawn.h   Date: 10/24/26
----------------------
SYS_spawnx (const char *path, char *const argv[], const struct spawnx *o) starts path as a
new process, as SYS_spawnfd does (argv as for SYS_spawn, NULL: { path }), with what a
shell needs besides:

  fds[0..2]  the child's descriptors 0, 1 and 2: the caller's descriptor of that number
             (its open file is shared), -1 for the console, SPAWN_CLOSED for none
  fds[3]     the child's descriptor 3, the same way; -1 (or SPAWN_CLOSED) for none. surfsh
             hands a new surfsh the text it is to run on it.
  envp       the child's environment: "name=value" strings, NULL-terminated (envp NULL: an
             empty one), at most SPAWN_ENV_MAX of them in SPAWN_ENV_BYTES bytes, -E2BIG
             beyond. The program finds it on its stack after argv: crt0 makes it libsurf's
             environ, and main's third argument.
  job        0: the caller's job; SPAWN_JOB_NEW: a new job, the child its first task (its id
             the child's pid); a job's id: the child joins it; SPAWN_JOB_NONE: in no job
             (10/25/26, surfos/job.h)
  flags      SPAWN_BG: the child is not in the foreground of its terminal even when the
             caller's output is paged (a job started with &): Ctrl+C there does not reach it.

The new pid; -ENOENT, -EACCES (not a file), -EBADF (a descriptor that is not open), -EFAULT,
-E2BIG, -EINVAL (flags or a job not known, too many arguments), -ENOMEM. libsurf's spawn()
and spawnfd() pass the program's own environ on through this call.

This header uses no SurfOS types: user programs reach it through user/include/surfos/spawn.h.
*/

#ifndef _SURFOS_SPAWN_H
#define _SURFOS_SPAWN_H

/* the arguments a program (or a kernel command, SYS_kcmd) takes: 128 in 8 KB (until 10/24/26 they were 32
   in 2 KB), so that a wildcard's names fit (rm *.tmp); each at most 255 bytes long */
#define SPAWN_MAX_ARGS  128
#define SPAWN_ARG_BYTES 8192

#define SPAWN_ENV_MAX   64          /* environment strings at most */
#define SPAWN_ENV_BYTES 4096        /* their bytes, terminators included */
#define SPAWN_CLOSED    (-2)        /* fds[]: the child has no such descriptor */

#define SPAWN_BG        0x2         /* flags: not in the foreground of its terminal */
#define SPAWN_JOB_NEW   (-1)        /* job: a new job (surfos/job.h) */
#define SPAWN_JOB_NONE  (-2)        /* job: in none (a shell's $( ), which no Ctrl+Z may stop) */

struct spawnx {
    int fds[4];                     /* the child's 0, 1, 2 and 3 */
    char *const *envp;              /* its environment */
    int job;                        /* 0: the caller's; SPAWN_JOB_NEW; a job's id */
    unsigned int flags;             /* SPAWN_BG */
};

#endif
