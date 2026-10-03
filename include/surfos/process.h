/*
SurfOS Processes
----------------------
File: process.h Date: 10/3/26 (roadmap P2)
----------------------
A process is a task with a user address space running an ELF executable.
*/

#ifndef _SURFOS_PROCESS_H
#define _SURFOS_PROCESS_H

#include <surfos/types.h>
#include <surfos/console.h>

/* Start path as a new process with argv[0..argc-1] on console con; returns the pid or -errno.
   The executable is loaded by the new task itself; a load failure ends it with exit code 127. */
int process_spawn(const char *path, int argc, char *const argv[], surf_console *con, u_int flags); /* TF_DETACHED: nobody waits */

void init_syscalls(void);

#endif
