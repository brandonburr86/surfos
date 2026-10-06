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
they are; Ctrl+C interrupts the foreground job as on a console.

On the pty, TTY_GETSIZE answers what PTY_SETSIZE set (80x24 at first), TTY_CAPS answers
TTY_CAP_UTF8 | TTY_CAP_COLOR, and TTY_RAW and TTY_PAGE work as on a console (the pager
counts the rows that PTY_SETSIZE set). Closing the last master descriptor hangs up:
every task whose terminal is the pty is killed (the kill that waits for the end of a
system call, however long it takes: a kernel command is never killed in the middle), its
reads end, and the pty is free again once they are gone.

This header uses no SurfOS types: user programs reach it through user/include/surfos/pty.h.
*/

#ifndef _SURFOS_PTY_H
#define _SURFOS_PTY_H

#define PTY_MAX     8

#define PTY_SETSIZE 1               /* arg: (columns << 16) | rows */
#define PTY_INDEX   2               /* returns the pty's number, 0 .. PTY_MAX - 1 */

#endif
