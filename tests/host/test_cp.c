/* Host-side tests for shell/cp.c, cp for both kernels' shells (10/18/26, audit B53, B60): the real
   cmd_cp() against a file layer this test scripts (what each vfs_read(), vfs_write() and vfs_close()
   returns, and when Ctrl+C arrives: TI_INT on the calling task), checking what cp says, its status,
   the bytes it says it wrote and whether it takes the Ctrl+C back. cp.c is built with its printf()
   and strerror() renamed (cp_printf, cp_strerror: the Makefile), so this test keeps the host's.
   Built like the kernel objects and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <surfos/task.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include "../../shell/shell.h"

int cp_printf(const char *fmt, ...);
const char *cp_strerror(int err);
int cmd_cp(int argc, char **argv);

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

/* what cp says */
static char said[1024];
static int said_len;

int cp_printf(const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(said + said_len, sizeof(said) - said_len, fmt, ap);
    va_end(ap);
    if(n > 0) said_len += n;
    if(said_len > (int)sizeof(said) - 1) said_len = sizeof(said) - 1;
    return n;
}

const char *cp_strerror(int err) {
    if(err < 0) err = -err;
    switch(err) {
    case EINTR: return "Interrupted system call";
    case EIO: return "I/O error";
    case ENOSPC: return "No space left on device";
    case ENOENT: return "No such file or directory";
    }
    return "Unknown error";
}

/* the calling task: only its interrupt flags matter here */
static surf_task task;
surf_task *curTask = &task;

void task_interrupt_absorbed(void) {
    curTask->intr &= ~TI_INT;
}

/* the heap */
static char heap[8192];
static int heap_used;
void *kalloc(size_t size) { (void)size; if(heap_used) return NULL; heap_used = 1; return heap; }
void kfree(void *mem) { if(mem == heap) heap_used = 0; }

/* the script: the source's size; what write call i returns (WHOLE: all it was given) and whether Ctrl+C has
   arrived once it returns; what the destination's close returns and whether Ctrl+C arrives in it; a read error */
#define WHOLE 99999
static struct file fin, fout;
static long src_size, src_pos, dst_bytes;
static int nwrites, wret[8], wintr[8], close_ret, close_intr, read_err;

int vfs_open(const char *path, int flags, struct file **f) {
    (void)path;
    *f = (flags & O_WRONLY) ? &fout : &fin;
    return 0;
}

int vfs_read(struct file *f, void *buf, u32 len) {
    long left = src_size - src_pos;
    (void)f; (void)buf;
    if(read_err) return read_err;
    if(left <= 0) return 0;
    if(left < (long)len) len = (u32)left;
    src_pos += len;
    return (int)len;
}

int vfs_write(struct file *f, const void *buf, u32 len) {
    int i = nwrites++, r = i < 8 ? wret[i] : WHOLE;
    (void)f; (void)buf;
    if(r == WHOLE) r = (int)len;
    if(r > 0) dst_bytes += r;
    if(i < 8 && wintr[i]) curTask->intr |= TI_INT;
    return r;
}

int vfs_close(struct file *f) {
    if(f != &fout) return 0;
    if(close_intr) curTask->intr |= TI_INT;
    return close_ret;
}

/* one cp: the source's size, the writes' results and their Ctrl+Cs, the close's; its status */
static int run(long size, int n, const int *ret, const int *intr, int cret, int cintr, int rerr, int intr_before) {
    char *argv[] = { "cp", "/src", "/dst/X", NULL };
    int i;
    src_size = size;
    src_pos = dst_bytes = 0;
    nwrites = 0;
    for(i = 0; i < 8; i++) { wret[i] = i < n ? ret[i] : WHOLE; wintr[i] = i < n ? intr[i] : 0; }
    close_ret = cret;
    close_intr = cintr;
    read_err = rerr;
    task.intr = intr_before ? TI_INT : 0;
    said[0] = 0;
    said_len = 0;
    return cmd_cp(3, argv);
}

static int has(const char *s) { return strstr(said, s) != NULL; }

int main(void) {
    char what[200];
    int st;

    { int r[] = {WHOLE, WHOLE}, a[] = {0, 0};                   /* the control: two whole writes */
      st = run(8192, 2, r, a, 0, 0, 0, 0);
      check(st == CMD_OK && has("8192 bytes copied") && !has("cut short"), "a copy that nothing stops: 8192 bytes copied, 0"); }

    { int r[] = {1000, -ENOSPC}, a[] = {0, 1};                  /* S1: a short write, Ctrl+C in its retry, a full disk */
      st = run(4096, 2, r, a, 0, 0, 0, 0);
      snprintf(what, sizeof(what), "S1: a full disk is said as that, Ctrl+C or not (said: %s)", said);
      check(st == CMD_FAIL && has("write: No space left on device") && !has("cut short") && dst_bytes == 1000, what); }

    { int r[] = {1000, -EINTR}, a[] = {0, 1};                   /* S1b: the retry cut short in its wait */
      st = run(4096, 2, r, a, 0, 0, 0, 0);
      snprintf(what, sizeof(what), "S1b: cut short, N counts the first 1000 bytes (said: %s)", said);
      check(st == CMD_FAIL && has("cut short after 1000 bytes") && has("is incomplete"), what); }

    { int r[] = {-EIO}, a[] = {1};                              /* S2: the card refuses, Ctrl+C pending */
      st = run(4096, 1, r, a, 0, 0, 0, 0);
      snprintf(what, sizeof(what), "S2: a refused write is said as an I/O error, Ctrl+C or not (said: %s)", said);
      check(st == CMD_FAIL && has("write: I/O error") && !has("cut short"), what); }

    { int r[] = {-EIO}, a[] = {0};                              /* S2b: the same without Ctrl+C */
      st = run(4096, 1, r, a, 0, 0, 0, 0);
      check(st == CMD_FAIL && has("write: I/O error"), "S2b: a refused write without Ctrl+C: an I/O error"); }

    { int r[] = {WHOLE}, a[] = {1};                             /* S3: Ctrl+C in the last write, which went whole */
      st = run(4096, 1, r, a, 0, 0, 0, 0);
      snprintf(what, sizeof(what), "S3: a copy whose last write took the Ctrl+C is complete, 0, the Ctrl+C taken back "
               "(said: %s)", said);
      check(st == CMD_OK && has("4096 bytes copied") && !has("incomplete") && !(task.intr & TI_INT), what); }

    { int r[] = {WHOLE, WHOLE}, a[] = {0, 0};                   /* S4: Ctrl+C in the close, its write-out cut short */
      st = run(8192, 2, r, a, -EIO, 1, 0, 0);
      snprintf(what, sizeof(what), "S4: a close whose write-out Ctrl+C cut short says so, not an I/O error (said: %s)", said);
      check(st == CMD_FAIL && has("8192 bytes copied") && has("interrupted while they were written out") &&
            !has("close: I/O error"), what); }

    { int r[] = {WHOLE, WHOLE}, a[] = {0, 0};                   /* S4b: a close that fails without Ctrl+C */
      st = run(8192, 2, r, a, -EIO, 0, 0, 0);
      check(st == CMD_FAIL && has("close: I/O error"), "S4b: a close that fails without Ctrl+C: close: I/O error"); }

    { int r[] = {WHOLE, 1536}, a[] = {0, 1};                    /* S5: cut after part of a write (B39's short count) */
      st = run(12288, 2, r, a, 0, 0, 0, 0);
      snprintf(what, sizeof(what), "S5: cut short after a part of a write, N 4096 + 1536 (said: %s)", said);
      check(st == CMD_FAIL && has("cut short after 5632 bytes") && dst_bytes == 5632, what); }

    { int r[] = {512}, a[] = {0};                               /* S6: Ctrl+C pending from the start: the first piece */
      st = run(8192, 1, r, a, 0, 0, 0, 1);
      snprintf(what, sizeof(what), "S6: Ctrl+C before the copy: the first piece goes, then cut short (said: %s)", said);
      check(st == CMD_FAIL && has("cut short after 512 bytes"), what); }

    { int r[] = {1000, 0}, a[] = {0, 0};                        /* S7: a write of nothing, no error, no Ctrl+C */
      st = run(4096, 2, r, a, 0, 0, 0, 0);
      check(st == CMD_FAIL && has("write: short write"), "S7: a write that takes nothing without saying why: short write"); }

    { int r[] = {WHOLE}, a[] = {0};                             /* S8: the source cannot be read */
      st = run(4096, 1, r, a, 0, 0, -EIO, 0);
      check(st == CMD_FAIL && has("read: I/O error"), "S8: a source that cannot be read: read: I/O error"); }

    { int r[] = {WHOLE}, a[] = {1};                             /* S9: Ctrl+C in a whole write that is not the last */
      st = run(8192, 1, r, a, 0, 0, 0, 0);
      check(st == CMD_FAIL && has("cut short after 4096 bytes") && has("is incomplete"),
            "S9: Ctrl+C in a whole write with more to copy: cut short after 4096 bytes, incomplete"); }

    printf("CP: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
