/*
SurfOS ring0 Shell - internal prototypes
(C)2004 Brandon Burr, command table 10/2026 (roadmap U1)
*/

#ifndef _SHELL_SHELL_H
#define _SHELL_SHELL_H

#include <surfos/types.h>

#define SHELL_MAX_ARGS 16
#define SHELL_LINE_LEN 255

struct command {
    const char *name;
    const char *args;   /* shown in help */
    const char *help;
    void (*fn)(int argc, char **argv);
};

/* demos.c: the 2004 demonstrations */
void runHanoi(void);
void invokeDemo(void);
void demoBeep(void);
void getFunky(int tempo);
void funky(void);

/* selftest.c */
int heaptest(void);         /* 0 = pass */
int run_selftest(void);     /* 0 = all pass */

#endif
