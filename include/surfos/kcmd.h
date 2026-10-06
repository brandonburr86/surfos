/*
SurfOS kernel commands for user programs
----------------------
File: kcmd.h    Date: 10/6/26
----------------------
The kernel's shell commands (shell/main.c, fdisk.c, fscmd.c: lsblk, fdisk, mount, ifconfig,
ps, dmesg, ...) reach the user shell, surfsh, through two system calls (surfos/syscall.h).

SYS_kcmd (char *const argv[]) runs the command argv[0] with its arguments in the calling
task, as the ring-0 shell would, and returns its status: 0 success, 1 failure, 2 a usage
error. It is -ENOENT when there is no such command. What the command prints goes to the
caller's descriptor 1 when that is a file (a redirection; at most KCMD_CAPTURE_MAX bytes,
written when the command is done), otherwise to the caller's console, paged when the
caller asked for paging (TTY_PAGE, surfos/tty.h). Its questions (fdisk's and mkfs's) are
answered on the caller's console.

SYS_kcmdinfo (int index, struct kcmd_info *out) copies entry index (0, 1, ...) of the
command table, for help and for Tab completion; -ENOENT after the last one.

This header uses no SurfOS types: user programs reach it through user/include/surfos/kcmd.h.
*/

#ifndef _SURFOS_KCMD_H
#define _SURFOS_KCMD_H

#define KCMD_NAME_MAX    16
#define KCMD_ARGS_MAX    48
#define KCMD_HELP_MAX    112
#define KCMD_CAPTURE_MAX (256 * 1024)  /* a redirected command's output that is kept; the rest is cut */

struct kcmd_info {
    char name[KCMD_NAME_MAX];       /* NUL-terminated, cut to fit */
    char args[KCMD_ARGS_MAX];       /* as help shows them, e.g. "<disk> [cmd]" */
    char help[KCMD_HELP_MAX];       /* one line */
};

#endif
