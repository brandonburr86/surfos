/*
SurfOS pseudo-terminals
----------------------
File: pty.h     Date: 10/6/26
----------------------
A terminal that is not a screen: what its programs print goes to whoever holds the
master side, and what the master side writes arrives as typed keys. The telnet server
gives each connection one, so that surfsh, its line editor, the pager, Ctrl+C and the
kernel's commands work over the network as on the laptop's own consoles.

SYS_openpty   (): a descriptor for the master side of a free pseudo-terminal (PTY_MAX in
              all); -EAGAIN when none is free.
SYS_spawnpty  (const char *path, char *const argv[], int masterfd): starts a program
              whose terminal is that pseudo-terminal: its console (/dev/console, its
              descriptors 0, 1 and 2, curTask->tty and ->con) is the pty, so the kernel's
              commands it runs print there too. The new pid, or -errno.
SYS_ptyctl    (int masterfd, int op, u32 arg): PTY_SETSIZE, PTY_INDEX.

read() on the master side waits for what the session printed and returns it as it was
printed: no '\n' to "\r\n" translation (the telnet server does that), escape sequences
as they are, colours of kernel messages (kprintf's yellow) as ANSI SGR sequences. It
returns 0 once the session has ended (no task has the pty as its terminal any more) and
everything was read. write() on the master side delivers the bytes as typed keys, with
the serial line's rules: in line mode CR is Enter, DEL is Backspace and the escape
sequences of the cursor keys become those keys; in raw mode (TTY_RAW) the bytes pass as
they are; Ctrl+C interrupts the foreground job as on a console, and in line mode drops the
keys typed ahead (on the consoles too). write() never waits: it takes the bytes in order
while each fits in the terminal's input queue (256 keys) and returns how many it took, or
-EAGAIN when the first does not fit; a byte that needs no room always fits (a line-mode
Ctrl+C; the rest of a cursor key's escape sequence, which line mode swallows). poll() on the
master side (surfos/poll.h): POLLIN when there is output or the session has ended, POLLHUP
once it has ended and everything was read (the read that returns 0 is next), POLLOUT while
a key fits.

On the pty, TTY_GETSIZE answers what PTY_SETSIZE set (80x24 at first), TTY_CAPS answers
TTY_CAP_UTF8 | TTY_CAP_COLOR, and TTY_RAW and TTY_PAGE work as on a console (the pager
counts the rows that PTY_SETSIZE set). Closing the last master descriptor hangs up:
every task whose terminal is the pty is killed (the kill that waits for the end of a
system call, however long it takes: a kernel command is never killed in the middle), its
reads end, and the pty is free again once they are gone.

This header uses no SurfOS types: user programs reach it through user/include/surfos/pty.h.
10/17/26: but for the kernel's own calls at its end, which only a file that has the kernel's
types (surfos/types.h, included first as everywhere in the kernel) sees: the same for a caller
in the kernel (the desktop's Terminal, shell/guisys_term.c), with a master that is an open file
of its own rather than a descriptor (kernel/pty.c).
*/

#ifndef _SURFOS_PTY_H
#define _SURFOS_PTY_H

#define PTY_MAX     8

#define PTY_SETSIZE 1               /* arg: (columns << 16) | rows */
#define PTY_INDEX   2               /* returns the pty's number, 0 .. PTY_MAX - 1 */

#ifdef _SURFOS_TYPES_H
/* 10/17/26: a master for the kernel. pty_open: a free pty's master (-EAGAIN when every one is in use),
   which pty_close closes: the hangup, as the last descriptor's close is. pty_spawn: a program on it as
   SYS_spawnpty starts one (argv the kernel's, argc of them), its pid. pty_read: what the session
   printed, never waiting: how many bytes, 0 while nothing is there, -EPIPE once the session has ended
   and everything was read. pty_write: as write() on the master (how many it took, -EAGAIN), never
   waiting. pty_resize: PTY_SETSIZE. pty_number: PTY_INDEX. Each: -EINVAL for a file that is no master. */
struct file;
int pty_open(struct file **master);
int pty_spawn(struct file *master, const char *path, int argc, char *const argv[]);
int pty_read(struct file *master, char *buf, u32 len);
int pty_write(struct file *master, const char *buf, u32 len);
int pty_resize(struct file *master, u32 cols, u32 rows);
int pty_number(struct file *master);
void pty_close(struct file *master);
#endif

#endif
