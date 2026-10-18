/*
SurfOS ring0 Shell - cp
(C)2004 Brandon Burr, written 10/2026

cp <source> <destination>, for both kernels' shells (shell/main.c, arch/riscv32/kshell.c each
had the same copy of it until 10/18/26).

10/16/26 (audit B4): a write that comes up short is written on, to learn why (a full disk says
so then). 10/18/26 (audit B53): stopped by Ctrl+C or a kill, which a long write obeys between
its pieces (a short count) or in a wait (EINTR), it says it was cut short and the copy is
incomplete, not "short write". 10/18/26 (audit B60): an error a write returns is that error,
also with Ctrl+C pending (a full disk, a card that refuses); N counts every byte written, the
first part of a write that came up short too; a copy whose last write took the Ctrl+C but went
whole is complete (its status is its own, as umount's: B54); a close whose write-out Ctrl+C
cut short says so, not "I/O error".
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <blibc_common.h>
#include "shell.h"

int cmd_cp(int argc, char **argv) {
    struct file *in, *out;
    char *buf;
    u_long total = 0;
    int r, w = 0, c, off, err = 0, cut = 0, unexplained = 0, complete;
    if(argc < 3) { printf("    usage: cp <source> <destination>\n"); return CMD_USAGE; }
    r = vfs_open(argv[1], O_RDONLY, &in);
    if(r) { printf("    cp: %s: %s\n", argv[1], strerror(r)); return CMD_FAIL; }
    r = vfs_open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, &out);
    if(r) { printf("    cp: %s: %s\n", argv[2], strerror(r)); vfs_close(in); return CMD_FAIL; }
    buf = (char *)kalloc(4096);
    if(!buf) { printf("    cp: out of memory\n"); vfs_close(in); vfs_close(out); return CMD_FAIL; }
    while((r = vfs_read(in, buf, 4096)) > 0) {
        off = 0;
        while(off < r) {                                /* short (a full disk): the rest, for why */
            w = vfs_write(out, buf + off, r - off);
            if(w <= 0) break;
            total += w;                                 /* (B60: every byte written counts) */
            off += w;
            if(task_interrupted()) break;               /* a kill, Ctrl+C: a long write stops between pieces (B39) */
        }
        if(off == r && !task_interrupted()) continue;   /* (the piece went whole) */
        if(off < r && w < 0 && w != -EINTR) { err = w; break; }   /* (B60: an error is that error) */
        if(off < r && w == 0 && !task_interrupted()) { unexplained = 1; break; }
        if(off == r) {                                  /* whole, but the call is to end: was it the last piece? */
            r = vfs_read(in, buf, 4096);
            if(r == 0) break;                           /* (B60: nothing more to copy: the copy is complete) */
        }
        cut = 1;                                        /* (B53: stopped, not refused) */
        break;
    }
    if(r < 0 && !err && !cut && !unexplained) {         /* the read failed, or was cut short */
        if(r == -EINTR || task_interrupted()) cut = 1;
        else printf("    cp: read: %s\n", strerror(r));
    }
    if(err) printf("    cp: write: %s\n", strerror(err));
    if(unexplained) printf("    cp: write: short write\n");
    if(cut) printf("    cp: cut short after %lu bytes: %s is incomplete\n", total, argv[2]);
    complete = r == 0 && !err && !cut && !unexplained;
    if(complete && task_interrupted()) task_interrupt_absorbed();   /* (B60: it finished: its own status) */
    kfree(buf);
    vfs_close(in);
    c = vfs_close(out);                                 /* (its write-out: a Ctrl+C now cuts that short) */
    if(c && complete && task_interrupted())
        printf("    cp: %s: %lu bytes copied; interrupted while they were written out (sync writes the rest)\n",
               argv[2], total);
    else if(c && !cut) printf("    cp: close: %s\n", strerror(c));   /* (B53: cut short, its write-out was cut too) */
    else if(complete) printf("    %lu bytes copied\n", total);
    return complete && !c ? CMD_OK : CMD_FAIL;
}
