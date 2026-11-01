/*
SurfOS windows for programs
----------------------
File: win.h     Date: 10/25/26
----------------------
A window on the desktop for an ordinary program (milestone M12,
docs/superpowers/plans/2026-10-25-app-windows.md). The program draws in its own memory and
presents what changed; the desktop (shell/gui.c) draws the window's pixels inside a frame of
its own and sends the program what happens in the window, as 16-byte events.

SYS_winopen (const struct win_open *o): a window on the desktop of the caller's terminal, as a
    descriptor (it takes one of the program's NR_OPEN). o->version is WIN_VERSION; w x h the
    size of its content, its pixels, kept within min_w..max_w and min_h..max_h (0: no limit)
    and within the desktop's largest content area; flags WIN_RESIZABLE or 0; background the
    0x00RRGGBB its pixels start as, and that the desktop shows where they do not reach; title
    up to WIN_TITLE_MAX bytes of UTF-8 with its NUL, cut between characters. The window
    appears with its first present, or 250 ms after it opened. -EFAULT, -EINVAL (a version,
    flag, size or colour that is none) come first; then -ENODEV when the caller's terminal is
    no desktop's: the console where gui runs, or a pty that desktop's Terminal opened (text
    mode, another console and telnet's ptys are none); -EAGAIN past WIN_PER_DESK windows on
    the desktop or WIN_MAX in all, -ENOMEM past WIN_DESK_BYTES of pixels on the desktop or
    WIN_ALL_BYTES in all, or without memory; -EMFILE without a free descriptor.
SYS_winctl (int fd, int op, void *arg): -EBADF, or -EINVAL for a descriptor that is no window;
    -EPIPE once the desktop let the window go (but WIN_INFO).
    WIN_PRESENT (const struct win_present *): copies the rectangle x, y, w, h of the program's
        image, whose top left pixel is at pixels and whose rows are stride pixels apart (at
        most WIN_STRIDE_MAX), to the same place in the window, during the call. The rectangle
        lies within the window and within a row (x + w <= stride); an empty one copies nothing
        and still counts as a present. -EFAULT when any of its rows is not the caller's to read.
    WIN_SETSIZE ((w << 16) | h): the window's pixels at a new size, all background again (the
        program presents the new image): within its limits and the desktop's, -EINVAL beyond.
    WIN_TITLE (const char *): a new title, cut as at the open.
    WIN_INFO (struct win_info *): the window's size; the desktop's logical screen and scale (0
        once the desktop let it go); whether it has the focus; how many events were lost.
    WIN_CLIP (const struct win_clip *, 10/26/26): len bytes at text (1 to WIN_CLIP_MAX, the
        desktop's clipboard's size; UTF-8 as a rule) for the desktop's clipboard, as its own Copy
        puts text there (Notepad and the Terminal paste it): text only, so it ends at a NUL if one
        comes first, and -EINVAL for none (len 0, or a NUL first); only while the window has the
        focus, -EACCES otherwise, so that a program in the background plants nothing to be pasted,
        and the desktop keeps it only if the window has its focus still when it takes the text;
        -EINVAL past WIN_CLIP_MAX, -EFAULT for text that is not the caller's to read, -ENOMEM.
    WIN_ALERT (10/26/26): the window asks to be seen (a timer that ran out): the desktop brings it
        to the front, shown again if it was tucked away into the taskbar, and marks its taskbar
        button until it has the focus. It never takes the focus or a key: nothing typed into
        another window reaches it. Once in WIN_ALERT_MS a program, whichever of its windows asks
        (-EAGAIN for another within them). The desktop acts on it after the pointer events it
        already has, and holds it while a pointer button is down or a drag goes on, until it
        ends, so that no window comes up under a click already under way; for the 500 ms after
        it came up, a press on the window is nobody's (it neither focuses the window nor reaches
        the program: a click meant for what was there). A kernel before these two ops says
        -EINVAL for them: a program goes on without.
read(): whole events, as many as fit in the buffer (-EINVAL for less than one) and are queued;
    it waits for one (a kill or Ctrl+C cuts the wait short: -EINTR), and returns 0 once the
    desktop let the window go and everything queued was read. WIN_EVENTS are queued at most:
    a pointer move with the same buttons as the move queued last takes its place (never a
    press's or a release's), as a new size does the last one's; past that an event is lost
    (WIN_INFO counts them).
poll() (surfos/poll.h): POLLIN when events are queued or the window was let go, POLLHUP once it
    was and everything was read, POLLOUT always (a present never waits).
The last close of the descriptor, or the program's reap after a kill, ends the window. A window
is the program's that opened it: that program's death takes it away at once, also when a child
of it holds the descriptor. The desktop's exit, or its death, lets every window of it go (and
their pixels); the programs are not killed for it.

The events (struct win_event): a key as it was typed (value: the byte, or WIN_KEY_* for the
keys that are no bytes); the pointer over the window's content, and anywhere while a button
pressed there is held (x, y: logical pixels from the content's top left; a position outside
the content says the pointer left it; value: the buttons held, WIN_BUTTON_*); the wheel over
the content (x, y as for the pointer; value: rows, as an int, positive down, 3 a notch); the
focus (value 1: the window came to the front, 0: it left it); a new size (x, y: the content
size the user gave the window by its corner or by maximizing it: the program answers with an
image of that size); and the close button. mods is 0, but for 11/1/26's WIN_MOD_SHIFT on an
Enter typed with Shift: to a program that does not look, it is Enter.

This header uses no SurfOS types: user programs reach it through user/include/surfos/win.h.
The desktop's side at its end is seen only by a file that has the kernel's types
(surfos/types.h, included first as everywhere in the kernel).
*/

#ifndef _SURFOS_WIN_H
#define _SURFOS_WIN_H

#define WIN_VERSION     1
#define WIN_TITLE_MAX   64              /* a title's bytes with its NUL; a longer one is cut between characters */
#define WIN_RESIZABLE   1               /* flags: the user may resize and maximize the window */
#define WIN_STRIDE_MAX  4096            /* a present's rows: pixels apart, at most */
#define WIN_EVENTS      64              /* queued for a window, at most */
#define WIN_PER_DESK    8               /* windows of one desktop */
#define WIN_MAX         32              /* windows in all */
#define WIN_DESK_BYTES  (8u << 20)      /* their pixels, of one desktop */
#define WIN_ALL_BYTES   (32u << 20)     /* and in all */
#define WIN_CLIP_MAX    (128u << 10)    /* 10/26/26: bytes of text a window puts on the clipboard, at most */
#define WIN_ALERT_MS    5000            /* 10/26/26: a window asks to be seen once in these, at most */

struct win_open    { unsigned version; unsigned short w, h, min_w, min_h, max_w, max_h;
                     unsigned flags, background; char title[WIN_TITLE_MAX]; };                 /* 88 bytes */
struct win_present { const unsigned *pixels; unsigned stride; unsigned short x, y, w, h; };  /* 16; 0x00RRGGBB */
struct win_info    { unsigned version; unsigned short w, h, screen_w, screen_h, scale, focused; unsigned lost; };   /* 20 */
struct win_clip    { const char *text; unsigned len; };                                       /* 8; 10/26/26 */

#define WIN_PRESENT     1
#define WIN_SETSIZE     2
#define WIN_TITLE       3
#define WIN_INFO        4
#define WIN_CLIP        5               /* 10/26/26 */
#define WIN_ALERT       6               /* 10/26/26 */

struct win_event { unsigned short type, mods; int x, y; unsigned value; };     /* 16 bytes */
#define WIN_EV_KEY      1
#define WIN_EV_POINTER  2
#define WIN_EV_WHEEL    3
#define WIN_EV_FOCUS    4
#define WIN_EV_RESIZE   5
#define WIN_EV_CLOSE    6
#define WIN_MOD_SHIFT   1

/* the keys that are no bytes (the desktop's GUI_UP .. GUI_BACKTAB, sys/gui.h) */
#define WIN_KEY_UP      0x100
#define WIN_KEY_DOWN    0x101
#define WIN_KEY_LEFT    0x102
#define WIN_KEY_RIGHT   0x103
#define WIN_KEY_HOME    0x104
#define WIN_KEY_END     0x105
#define WIN_KEY_PGUP    0x106
#define WIN_KEY_PGDN    0x107
#define WIN_KEY_DELETE  0x108
#define WIN_KEY_INSERT  0x109
#define WIN_KEY_BACKTAB 0x10a           /* 11/1/26: Shift+Tab */

#define WIN_BUTTON_LEFT   1
#define WIN_BUTTON_RIGHT  2
#define WIN_BUTTON_MIDDLE 4

#ifdef _SURFOS_TYPES_H
/* The desktop's side (shell/guiclient.c, through struct gui_sys's win_* calls). A window as the desktop draws it:
   its title; the size of its pixels, which are its content; the limits and flags it asked for (max 0: as large as
   the desktop allows); its background, shown where its pixels do not reach; a serial that moves with every present,
   size and title; the pid of the program that opened it; and (10/25/26, stage C6) whether that program is in the
   foreground job of a pty the desktop adopted, its Terminal's, a job that session's shell made (typed at its prompt
   without &, or started by one that was): the desktop gives such a window the focus when it appears and the Terminal
   has it. 10/26/26: how many times it asked to be seen (WIN_ALERT). */
struct win_state {
    char title[WIN_TITLE_MAX];
    int w, h, min_w, min_h, max_w, max_h;
    u32 flags, background, serial;
    int pid;
    int foreground;
    u32 alerts;
};
/* its pixels, 0x00RRGGBB in rows of w, lent to the desktop until its next update or close */
struct win_view { const u32 *pixels; int w, h; };

/* kernel/win.c, for the desktop (shell/gui.c, shell/guisys_win.c, shell/guisys_term.c). Each call is the calling
   task's: its desktop is the one it registered. win_register: the calling task's console has a desktop now, of
   width x height logical pixels at scale, whose windows' content is at most max_w x max_h: a descriptor of the
   task (its close, or the task's reap, is the desktop's end: every window is let go); -EBUSY when that console
   has one, -EAGAIN past WIN_DESKS, -ENODEV without a console. win_unregister: that descriptor closed. win_adopt:
   the programs on pty number pty (the Terminal's) may open windows on it too (-EINVAL for no pty, -ENOSPC past
   4, -ENODEV for a task without a desktop); win_drop: no longer. win_pty_gone (kernel/pty.c, interrupts off):
   pty t hung up or is free, adopted by no desktop any more (10/25/26). win_adopt_shell (10/25/26, stage C6): the
   session on adopted pty number pty has the shell pid (the Terminal's surfsh), whose jobs alone count as its
   foreground for win_state.foreground (-EINVAL for a pty not adopted). win_accept,
   win_update, win_event, win_close: as struct gui_sys's (sys/gui_sys.h); -EINVAL for a handle that is not one of
   the calling task's desktop's. win_clip_take (10/26/26): the text a window of the calling task's desktop put on
   its clipboard (WIN_CLIP), the latest since the call before, taken: copied into out, at most size bytes, when it
   came from window h, the one with the desktop's focus (h < 0: none has it), and dropped otherwise; how many bytes,
   0 for none, -ENODEV for a task without a desktop. */
#define WIN_DESKS 4
struct win_desk { int width, height, max_w, max_h, scale; };
int win_register(const struct win_desk *desk);
void win_unregister(int fd);
int win_adopt(int pty);
void win_drop(int pty);
struct tty;
void win_pty_gone(const struct tty *t);
int win_adopt_shell(int pty, int pid);
int win_accept(void);
int win_update(int h, struct win_state *st, struct win_view *view);
int win_event(int h, const struct win_event *e);
void win_close(int h);
int win_clip_take(int h, char *out, u32 size);
#endif

#endif
