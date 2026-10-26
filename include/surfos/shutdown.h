/*
SurfOS Shutdown
----------------------
File: shutdown.h    Date: 10/25/26 (robustness, B4)
----------------------
reboot and poweroff that end the programs and write out and unmount what is mounted
read-write before the machine resets or turns off (kernel/shutdown.c).
*/

#ifndef _SURFOS_SHUTDOWN_H
#define _SURFOS_SHUTDOWN_H

#include <surfos/types.h>

#define SHUT_REBOOT   1
#define SHUT_POWEROFF 2

int shutdown_start(int how);        /* the shutdown thread: 0, -EBUSY when one runs already, -ENOMEM */
bool shutdown_started(void);        /* init starts no shell, spawn no program (-EAGAIN) */
bool shutdown_unmounting(void);     /* 10/25/26 (the review of B4): the unmounts have begun: the network manager
                                       mounts the stick no more (until then the hooks may write it) */
/* run in a worker before any program ends, 10 s at most for all of them, the stick still open to the network manager
   (track A's history, A10): 0, or -ENOMEM when MAX_HOOKS are there already */
int shutdown_hook(void (*fn)(void));
int shutdown_dry_run(void);         /* poweroff -n: what poweroff would do; 0 when soft off is possible */
void shutdown_key(void);            /* Ctrl+Alt+Del, from the keyboard's interrupt (interrupts off) */
bool shutdown_key_take(void);       /* init, woken by it: start the shutdown? (once) */

/* 10/25/26 (the review of B4): the Delete key's codes as the keyboard sends them (kernel/keyboard.c): a press is a make
   that follows a break, never the keyboard's repeats of a key held down. What to do with a code: */
struct del_key { bool down; };
#define DEL_NOTHING 0
#define DEL_DELETE  1                /* the Delete key for the tty */
#define DEL_PRESS   2                /* Ctrl+Alt+Del pressed: shutdown_key() */
int del_key_event(struct del_key *k, bool make, bool ctrl_alt);

#endif
