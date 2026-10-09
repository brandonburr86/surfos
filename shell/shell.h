/*
SurfOS ring0 Shell - internal prototypes
(C)2004 Brandon Burr, command table 10/2026 (roadmap U1)

10/6/26 (surfsh): a command returns its status, as the user shell's commands do:
CMD_OK, CMD_FAIL (a refusal and a Cancelled question are failures too) or
CMD_USAGE (surfos/kcmd.h; the user shell shows it in its prompt). The user shell
runs the same commands in its own task (SYS_kcmd): a question is asked with
shell_ask(), so it is seen even when the command's output goes to a file.
*/

#ifndef _SHELL_SHELL_H
#define _SHELL_SHELL_H

#include <surfos/types.h>

#define SHELL_MAX_ARGS 16
#define SHELL_LINE_LEN 255

#define CMD_OK    0     /* success */
#define CMD_FAIL  1     /* failure */
#define CMD_USAGE 2     /* a usage error: what was typed is not how the command is used */

struct command {
    const char *name;
    const char *args;   /* shown in help */
    const char *help;
    int (*fn)(int argc, char **argv);   /* the status, CMD_OK, CMD_FAIL or CMD_USAGE (10/6/26) */
    int flags;          /* 10/6/26: CMD_ASKS */
};

/* 10/6/26: it reads the keyboard as it goes, not through shell_ask(): it is refused (CMD_USAGE)
   when the user shell sends its output to a file (SYS_kcmd), where its questions would go */
#define CMD_ASKS 1

/* main.c: a question on this task's console and its answer, a line (10/6/26) */
void shell_ask(const char *question, char *answer, size_t size);
/* 10/9/26: the same for a secret (a password): nothing typed is shown, kept or logged */
void shell_ask_secret(const char *question, char *answer, size_t size);

/* demos.c: the 2004 demonstrations */
void runHanoi(void);
void invokeDemo(void);
void demoBeep(void);
void getFunky(int tempo);
void funky(void);

/* selftest.c: one of them at a time (10/6/26), a second is refused and fails */
int heaptest(void);         /* 0 = pass */
int run_selftest(void);     /* 0 = all pass */

/* fdisk.c */
int cmd_fdisk(int argc, char **argv);

/* fscmd.c: file system commands (10/5/26) */
int cmd_mount(int argc, char **argv);
int cmd_umount(int argc, char **argv);
int cmd_eject(int argc, char **argv);
int cmd_df(int argc, char **argv);
int cmd_mv(int argc, char **argv);
int cmd_mkfs(int argc, char **argv);
int cmd_fsbench(int argc, char **argv);

#endif
