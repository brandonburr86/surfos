/*
SurfOS processes, as programs see them
--------------------
File: proc.c    Date: 10/6/26
--------------------
SYS_procinfo and SYS_detach (surfos/proc.h). The task list itself is kernel/task.c's:
task_info() copies one entry while interrupts are off, and this file hands it to the
caller; task_detach() moves a process out of its shell's job.
*/

#include <surfos/types.h>
#include <surfos/task.h>
#include <surfos/proc.h>
#include <surfos/sysimpl.h>
#include <fs/vfs.h>
#include <blibc_common.h>

/* task number index (0, 1, ...): pid, parent, state, flags, terminal, name; -ENOENT after the last */
int sys_procinfo(int index, struct procinfo *pi) {
    struct procinfo k;
    int r;
    if(!user_buf(pi, sizeof(*pi), true)) return -EFAULT;
    r = task_info(index, &k);
    if(r == 0) memcpy(pi, &k, sizeof(k));
    return r;
}

/* the caller leaves its parent and its shell's job: init's child from now on */
int sys_detach(void) {
    return task_detach(curTask);
}
