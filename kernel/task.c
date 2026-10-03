/*
SurfOS Multi-tasking Routine's
--------------------
File: task.c    Date: 6/10/04
--------------------
(C)2004 Brandon Burr
*/

#include <surfos/types.h>
#include <surfos/task.h>
#include <surfos/console.h>
#include <mm/kalloc.h>
#include <surfos/interrupt.h>
#include <surfos/system.h>
#include <surfos/gdt.h>
#include <surfos/trap.h>

#include <blibc_common.h>

/*
    The SurfOS tasking model is one of elegance and stuff. It is based on a 6 priority level
    software task switching model. However, there are only four of those PL's that runnable
    tasks operate at.

    SurfOS Prioirty Levels:
    -----------------------
    PL 0: FIFO Priority - tasks are run until they terminate (non pre-empted)
    PL 1: High Prioirty - Tasks run twice as much as low prioirty
    PL 2: Normal Priority -Tasks run more than low prio.. but less than High
    PL 3: Low Prioirty - Tasks run half as much as high prio
    PL 4: Sleeping - Tasks that are not running, but waiting to be re activated
    PL 5: Marked for removal - non running tasks that need to be deleted

    The first timer interrupt causes the ESP to be saved in the curTask. So the default curTask
    must be the kernel idle task. From there, the getNextTask function determines what task
    is to run next.

    getNextTask chooses its selection based on this order sequence:
    order: FHNL FHNH FHNL FHNH FHNL FHNH FHNL

    where F is PL0, H is PL1, N is PL2, L is PL3. that tasks maintains the current state of the
    prioirty selection, so it determines the next task based on that.
*/

extern surf_console conArray[];
extern surf_console *conActive;

/** Global variables **/

u_int inKernCritSect; // An integer so when it is non-zero, the kernel is in a critical section


surf_runqueue *tqActive[NUM_PRIO]; // The main task queue linked lists. Normalls 6 prio levels, but map change


/* pointers to the current task and the idle task. These are mostly used for exceptions and
panics. It allows for a kernel function to know who made the exception. With this implementation, curTask is irrelevant in the task switching process, it does not screw up
the selection order */
surf_task *curTask,*idleTask;

//This is basically a fancy bitmap holding selection information for getNextTask
u_int plOrder[2][4]; //priority level ordering

/**********************/

extern void startShell();
extern void do_banner();

void shell() {
    do_banner();
    startShell();
}

/* I decided to create a task stublet for processes.. instead of directly iret'ing to the EIP
for the process. This gives a lot more flexibility, and it makes process termination a little
more structured (as opposed to an invalid opcode terminating the program). */
void task_stublet() {
    //do any init work...

    curTask->start_func(); //call the beginning ESP

    //end the task
    kill_task(curTask);
    yield();
}

/**** Queue functions ****/

/* This function will remove a task from its current prioirty level when it is no longer
 needed at that level. It assumes the task structure has the necessary prio level
  information within it. */
void dequeue_task(surf_task *task) {
    if(!task) return;
    u_int level=task->prio;
    surf_task *tmp;

    KCRIT_ENTER
    tmp = (surf_task*)task->prev;
    if(tmp) {
        tmp->next = task->next;
    } else {
        tqActive[level]->first = (surf_task*)task->next;
    }

    tmp = (surf_task*)task->next;
    if(tmp) {
        tmp->prev = task->prev;
    } else {
        tqActive[level]->last = (surf_task*)task->prev;
    }
    KCRIT_LEAVE
}

/* This will append a task on to the end of a prioirty level. It assumes the task structure
    has the necessary prio level information within it. */
void enqueue_task(surf_task *task) {
    if(!task) return;
    u_int level=task->prio;
    KCRIT_ENTER

    surf_task *tmp = tqActive[level]->last;

    if(!tmp) { //queue empty
        tqActive[level]->first = task;
        tqActive[level]->last = task;
        task->next = NULL;
        task->prev = NULL;
    } else {
        tmp->next = task;
        task->prev = tmp;
        task->next = NULL;
        tqActive[level]->last = task;
    }

    KCRIT_LEAVE
}

/* This is just for convenience.. for when a task is run, you want to move it to the back of
    the queue. Notice this is inlined ;)  */
inline void requeue_task(surf_task *task) { // :) simple
    if(!task) return;
    dequeue_task(task);
    enqueue_task(task);
}

/* This is a quick and simple function to wipe clean all the tasks in the removal queue. */
inline void flush_remove_queue() {
    KCRIT_ENTER
    surf_task *ntmp, *tmp = tqActive[PL_REMOVE]->first;
    while(tmp) {
        ntmp = tmp->next;
        delete_task(tmp);
        tmp=ntmp;
    }
    KCRIT_LEAVE
}

/**************************/

/**** Priority Functions ****/
char *plNumToName(prio_level prio) {
    switch(prio) {
    case PL_FIFO:
        return "FIFO Priority";
    case PL_HIGH:
        return "High Priority";
    case PL_NORMAL:
        return "Normal Priority";
    case PL_LOW:
        return "Low Priority";
    case PL_SLEEPING:
        return "Sleeping tasks";
    case PL_REMOVE:
        return "Flagged for removal";
    }
    return "Unknown";
}

/* Simple yet effective function, move a process to the end of a different prioirty queue */
surf_task *set_task_prio(surf_task *task, prio_level prio) {
    if(!task) return NULL;
    KCRIT_ENTER
    dequeue_task(task);
    task->prio = prio;
    enqueue_task(task);
    KCRIT_LEAVE
    return task;
}

/* Time to get up! Remove the task from the sleeping queue and restore it to its original prio */
void wake_task(surf_task *task) {
    if(!task) return;
    if(task->prio != PL_SLEEPING) return;

    KCRIT_ENTER
    task->status = TS_RUNNABLE;
    set_task_prio(task,task->slpSav);
    KCRIT_LEAVE

}

/* Save the current prioirty level, then move the task to the sleeping queue. Also save the
    amount of milliseconds that the task is to sleep */
void sleep_task(surf_task *task, u_int ms) {
    if(!task) return;
    if(task->prio == PL_SLEEPING) return;

    task->status = TS_SLEEPING;
    task->timeleft = ms;

    task->slpSav = task->prio;

    set_task_prio(task,PL_SLEEPING);
}

/****************************/


/****** Task item functions *******/

/* This function retrieves an open PID for a new process */
u_int getPID() {
    static u_int PID=0;
    return PID++;
}

/* Build the first trap frame of a task at the top of its stack, so that "returning"
from a trap into it starts task_stublet() with the right segments and flags. The frame
has no useresp/ss for a ring-0 task because iret does not pop them within ring 0. */
static struct trapframe *build_initial_frame(u_char *stack_top, u_int ring, u_long eip) {
    u_int size = (ring == USER) ? sizeof(struct trapframe) : TRAPFRAME_KERNEL_SIZE;
    struct trapframe *tf = (struct trapframe*)(stack_top - size);
    memset(tf, 0, size);
    tf->eip = eip;
    if(ring == USER) {
        tf->cs = USER_CS;
        tf->ds = tf->es = tf->fs = tf->gs = tf->ss = USER_DS;
        tf->eflags = DEF_EFLAGS_3;
    } else {
        tf->cs = KERNEL_CS;
        tf->ds = tf->es = tf->fs = tf->gs = KERNEL_DS;
        tf->eflags = DEF_EFLAGS_0;
    }
    return tf;
}

/* This is the head honcho. A new process is created by setting up a stack frame for it,
making a task structure for it, then queueing it on a prioiry level. Note that the EIP
is saved.. so the task stublet can call it. EIP is not called directly. */
surf_task *new_task(char *name, surf_console *con, u_int ring, prio_level prio, u_long *eip) {
    static bool isIdle=true;
    surf_task *nTask = (surf_task*)kalloc(sizeof(surf_task));
    char *pName = (char*)kalloc(strlen(name)+1);
    u_char *stack = (u_char*)kalloc(KSTACK_SIZE);
    if(!nTask || !pName || !stack) return NULL; //problems

    memset(nTask,0,sizeof(*nTask));
    memset(stack,0,KSTACK_SIZE);

    nTask->pid = getPID(); //new process ID

    nTask->stackmem = (u_long*)stack; //save the buffer so we can kfree it later
    nTask->esp = (u_long*)build_initial_frame(stack + KSTACK_SIZE, ring, (u_long)task_stublet);

    nTask->name = pName; strcpy(nTask->name,name);

    nTask->prio = prio;
    nTask->status = TS_RUNNABLE;
    nTask->swapCount = 0;
    nTask->prev = nTask->next = NULL;
    nTask->slpSav = prio;
    nTask->con = con;

    //set timeleft used for sleeping tasks
    nTask->timeleft = 0;

    //set the starting EIP for the process...
    nTask-> start_func = (task_stub)eip;

    //make sure that the first task created is the idle task
    if(isIdle) idleTask=nTask,isIdle=false;

    enqueue_task(nTask);
    return nTask;
}
/* Quite a simple function.. just checking to see if the idle task was killed.. if not then
    proceed. */
void kill_task(surf_task *task) {
    KCRIT_ENTER
    if(task->pid == 0) {
        kprintf("FATAL: Kernel Idle Task Killed\n");
        kprintf("HALTING\n");
        halt();
    }
    delete_task(task);
    KCRIT_LEAVE
}

/* Free all memory associated with a task and dequeue it */
void delete_task(surf_task *task) {
    if(!task) return;
    bool del = false;
    if(task == curTask) del = true;
    KCRIT_ENTER
    dequeue_task(task);
    kfree(task->name);
    kfree(task->stackmem);
    kfree(task);
    if(del==true) curTask=0;
    KCRIT_LEAVE
}

/* For a more detailed explanation of this function.. see top. This basically selects what
process is to be run next. */
// order: FHNL FHNH FHNL FHNH FHNL FHNH FHNL
inline surf_task *getNextTask() {
    static u_int iCur = 0, iCount=0;

    KCRIT_ENTER
    surf_task *tNext = tqActive[plOrder[iCur][iCount++]]->first;
    if(iCount > 3) {
        iCount=0;
        if(iCur==0) iCur++;
        else iCur--;
    }
    KCRIT_LEAVE
    return tNext;
}

/**********************************/

//halt all execution and switch processes. Similar to a Visual Basic DoEvents!!! :P
void yield() {
    asm("int $0x40"); //  :)
}

void print_tasks() {
    int i=0;
    surf_task *tmp;
    printf("\nSurfOS Task List\n");
    printf("--------------------------\n");
    KCRIT_ENTER
    for(i=0;i<PL_SLEEPING;i++) {
        tmp=tqActive[i]->first;
        if(!tmp) continue;
        printf("%s:\n",plNumToName((prio_level)i));
        while(tmp) {
            printf("  %i) \'%s\'\n",tmp->pid,tmp->name);
            tmp=tmp->next;
        }
        printf("\n");
    }
    KCRIT_LEAVE
    printf("\n");
}

/* Populate the prioirty level bitmap. This defines the rules for what processes are selected
    and in what order. */
void makePlOrder() {
    plOrder[0][0]=PL_FIFO;
    plOrder[0][1]=PL_HIGH;
    plOrder[0][2]=PL_NORMAL;
    plOrder[0][3]=PL_LOW;

    plOrder[1][0]=PL_FIFO;
    plOrder[1][1]=PL_HIGH;
    plOrder[1][2]=PL_NORMAL;
    plOrder[1][3]=PL_HIGH;

}

/* yield() arrives here through int 0x40: just reschedule */
static u_long *yield_trap(struct trapframe *tf) {
    return schedule(tf);
}

/* Do some housecleaning.. and set up the initial tasks. The first curTask must be the idle task
    because the curTask's ESP will be written over by the first timer interrupt. */
void init_task() {
    int i=0;
    inKernCritSect=0;

    for(i=0;i<NUM_PRIO;i++) { // null the runqueues
        tqActive[i] = (surf_runqueue*)kalloc(sizeof(surf_runqueue));
        tqActive[i]->current = NULL;
        tqActive[i]->first = NULL;
        tqActive[i]->last = NULL;

        tqActive[i]->prio = (prio_level)i;
    }

    //priority ordering
    makePlOrder();

    trap_set_handler(T_YIELD, yield_trap); //interrupt for yield()

    /*
        REMEMBER: the default current task will have its
        registers overwritten by the first timer interrupt!
        so the curTask MUST!!! start out as the Idle Task
    */

    curTask = new_task("Kernel Idle Task",&conArray[0],KERNEL,PL_LOW,NULL);

    new_task("Shell 0",&conArray[0],KERNEL,PL_HIGH,(u_long*)shell);
}

/* Ahh... sexy. The infamous schedule(). This inputs the current proceses ESP, and outputs
    the new processes ESP. This lends it to be called stack swapping... :/
    This also decrements the timers of sleeping functions.
*/
u_long *schedule(struct trapframe *tf) {
    surf_task *slpTmp;
    slpTmp=tqActive[PL_SLEEPING]->first;

    while(slpTmp) {
        slpTmp->timeleft -= 10;
        if(slpTmp->timeleft <= 0) wake_task(slpTmp);
        slpTmp = (surf_task*)slpTmp->next;
    }

    if(curTask) { //else current task was deleted

        curTask->esp = (u_long*)tf; //save the current ESP
        curTask->swapCount++;

        if(inKernCritSect>0) return curTask->esp;
        if(curTask->prio == PL_FIFO) return curTask->esp; //in a FIFO task

        requeue_task(curTask); //send current task to end of queue
   }

    while((curTask=getNextTask()) == NULL); //lets hope there are tasks! (or hang)

    conActive = curTask->con;

    return curTask->esp;
}
/******************************/

/* Kernel critical sections are important. the SurfOS critical sections are essentially
    integers. When someone enters a crit section, the integer is incremented. when they
    leave the critical section, the integer is decremented. The scheduler is ONLY allowed
    to task switch if that integer is 0. This allows for nesting of critical sections.
*/
void kcritical_enter() { //Enter a kernel Critical Section. Prevent task switch
    inKernCritSect++;
}

void kcritical_leave() { //Leave a kernel Critical Section. Allow task switch
    inKernCritSect--;
}
