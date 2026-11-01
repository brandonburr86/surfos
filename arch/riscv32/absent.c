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
#include <surfos/shutdown.h>
#include <surfos/fpu.h>
#include <net/netman.h>
#include <net/dhcp.h>
#include <fs/vfs.h>
#include <mm/uvm.h>
#include <mm/pmm.h>
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

/**** 10/31/26: what x86's later work calls from the shared files (the join of the two lines) ****/

/* The board is switched off, not shut down: no shutdown thread (kernel/shutdown.c) */
bool shutdown_started(void) {
    return false;
}

int shutdown_start(int how) {
    (void)how;
    return -ENOSYS;
}

bool shutdown_key_take(void) {
    return false;
}

/* No network, so no network manager and none of its files on a stick: devfs's /dev/config has nothing to show */
bool netman_wait_boot(void) {
    return false;
}

bool netman_history_on(void) {
    return false;
}

bool netman_can_save(void) {
    return false;
}

int netman_file_read(const char *name, char *buf, u_int max, u_int *len, u_int *gen, char *why, size_t wsize) {
    (void)name; (void)buf; (void)max; (void)len; (void)gen; (void)why; (void)wsize;
    return -ENOENT;
}

int netman_file_write(const char *name, const char *data, u_int len, u_int *gen, char *why, size_t wsize) {
    (void)name; (void)data; (void)len; (void)gen; (void)why; (void)wsize;
    return -EROFS;
}

void dhcp_hostname(char *out, size_t size) {
    if(size) out[0] = 0;
}

/* No frames: the heaps are RAM from the start (arch/riscv32/mem.c), so a heap's promise holds, and nothing is short of
   frames (init's check for programs). None to spare for a scrollback, which a console has only on a framebuffer. */
int pmm_commit(u_long n, int cls) {
    (void)n; (void)cls;
    return 0;
}

u_long pmm_room(int cls) {
    (void)cls;
    return ~0UL;
}

u_long pmm_free_frames(void) {
    return 0;
}

/* No address spaces (above), and nothing of them being freed */
struct uvm *uvm_current(void) {
    return NULL;
}

u_long uvm_freeing(void) {
    return 0;
}

/* rv32im has no floating point: a process would have no registers of that kind to keep */
void init_fpu(void) {
}

int fpu_task_new(struct surf_task *t) {
    (void)t;
    return 0;
}

void fpu_task_gone(struct surf_task *t) {
    (void)t;
}

void fpu_task_free(struct surf_task *t) {
    (void)t;
}

/* No pseudo-terminals (above): nothing writes to a stream console */
u_int pty_room(surf_console *con) {
    (void)con;
    return 0;
}

int pty_wait_room(surf_console *con, u_int n) {
    (void)con; (void)n;
    return -ENOSYS;
}
