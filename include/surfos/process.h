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

/* 10/5/26: the same, with the child's descriptors 0, 1 and 2 sharing files[0..2] (each gains a
   reference; NULL or no array: the console) */
struct file;
int process_spawn_files(const char *path, int argc, char *const argv[], surf_console *con, u_int flags,
                        struct file *const files[3]);

void init_syscalls(void);

#endif
