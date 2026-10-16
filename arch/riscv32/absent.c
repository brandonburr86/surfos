/*
SurfOS riscv32: what machine mode does not have
--------------------
File: absent.c  Date: 10/15/26 (SurfCPU)
--------------------
The shared kernel files call into parts of the x86 kernel that the machine-mode port
leaves out (surfcpu/docs/PORTING-RISCV.md): user processes (ELF programs in ring 3,
their address spaces, system calls and the kernel commands they run), pseudo-terminals
(telnetd's sessions) and port I/O. Each entry point is here, saying no in the way its
callers already handle, or stopping the kernel when reaching it would be a porting bug.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/process.h>
#include <surfos/panic.h>
#include <surfos/task.h>
#include <surfos/sysimpl.h>
#include <mm/uvm.h>
#include <asm/io.h>
#include <blibc_common.h>

#ifndef ENOSYS
#define ENOSYS 38
#endif

/* No user mode: a program cannot be started. init (kernel/task.c) starts the kernel shell
   on every console instead, so nothing asks at boot. */
int process_spawn(const char *path, int argc, char *const argv[], surf_console *con, u_int flags) {
    (void)path; (void)argc; (void)argv; (void)con; (void)flags;
    return -ENOSYS;
}

int process_spawn_files(const char *path, int argc, char *const argv[], surf_console *con, u_int flags,
                        struct file *const files[3]) {
    (void)files;
    return process_spawn(path, argc, argv, con, flags);
}

/* no SYS_kcmd: a task never holds a kernel command's capture or arguments for a program */
void kcmd_task_gone(struct surf_task *t) {
    (void)t;
}

/* no user address spaces: every task's uvm is NULL, so these are never reached */
void uvm_switch(struct uvm *m) {
    if(m) panic("uvm_switch: no user address spaces on riscv32");
}

void uvm_destroy(struct uvm *m) {
    if(m) panic("uvm_destroy: no user address spaces on riscv32");
}

/* no user memory: the system calls compiled with the shared files (fs/poll.c's poll) are
   never called, and a user pointer would be refused */
bool user_buf(const void *p, u_long len, bool write) {
    (void)p; (void)len; (void)write;
    return false;
}

bool user_str(const char *s) {
    (void)s;
    return false;
}

/* No pseudo-terminals (no network, no telnetd): their consoles exist in conArray[] but
   nothing ever opens one, so nothing is ever written to a master. */
int pty_out(surf_console *con, const char *s, u_int n, int how) {
    (void)con; (void)s; (void)n; (void)how;
    return 0;
}

void pty_task_gone(struct surf_task *task) {
    (void)task;
}

/* include/arch/riscv32/asm/io.h: x86 port I/O reached at run time */
void riscv32_port_io(const char *what, u_int port) {
    panic("%s on port 0x%x: RISC-V has no I/O ports (a PC code path on riscv32)", what, port);
    for(;;) ;                       /* panic() halts */
}
