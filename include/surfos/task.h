/*
SurfOS Multitasking Header
----------------------
File: task.h    Date: 6/10/04, trapframe since 10/2026
----------------------
(C)2004 Brandon Burr
*/

#ifndef _SURFOS_TASK_H
#define _SURFOS_TASK_H

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/trap.h>


#define NUM_PRIO 6

#define DEF_EFLAGS_0 0x202   /* IF, plus bit 1 which is always set */
#define DEF_EFLAGS_3 0x3202  /* the same with IOPL 3 */
#define KSTACK_SIZE 0x4000   /* 16 KB kernel stack per task */

#define KERNEL 0
#define USER 3

typedef void(*task_stub) (void);

/* A task's saved registers are its struct trapframe at the top of its stack (trap.h). */

typedef enum {
    TS_FIFO = 1,
    TS_RUNNABLE = 2, //runnable means the task can be run normally
    TS_SLEEPING = 3, //sleeping means the task is sleeping for so many ms
} task_status;


typedef enum {
    PL_FIFO = 0,
    PL_HIGH = 1,
    PL_NORMAL = 2,
    PL_LOW = 3,
    PL_SLEEPING = 4,
    PL_REMOVE = 5
} prio_level;

typedef struct surf_task {
    char *name;
    u_long pid;
    prio_level prio,slpSav;
    surf_console *con;
    task_status status;
    u_int swapCount;
    u_long *esp, *stackmem;
    u_int timeleft; //milliseconds left... multiple of 10
    task_stub start_func;

    struct surf_task *prev, *next;
} surf_task;

typedef struct {
    prio_level prio;
    surf_task *first;
    surf_task *last;
    surf_task *current;
} surf_runqueue;

void init_task();


//kernel critical sections!

#define KCRIT_ENTER kcritical_enter();
#define KCRIT_LEAVE kcritical_leave();

void kcritical_enter();
void kcritical_leave();

extern surf_task *curTask;

void yield();
void dequeue_task(surf_task *task);
void enqueue_task(surf_task *task);
inline void requeue_task(surf_task *task);
char *plNumToName(prio_level prio);
surf_task *set_task_prio(surf_task *task, prio_level prio);
void wake_task(surf_task *task) ;
void sleep_task(surf_task *task, u_int ms);
u_int getPID();
surf_task *new_task(char *name, surf_console *con, u_int ring, prio_level prio, u_long *eip);
void kill_task(surf_task *task);
void delete_task(surf_task *task);
inline surf_task *getNextTask();
void print_tasks() ;
void makePlOrder();
u_long *schedule(struct trapframe *tf);
inline void flush_remove_queue();

#endif
