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
    WIN_THEME (struct win_theme *, 11/05/26): what WIN_INFO grew by, the desktop's theme
        (WIN_THEME_OCEAN or WIN_THEME_DUSK) and its colours by role (WIN_COLOR_*, 0x00RRGGBB):
        those the desktop draws its own windows in, so that a program's window matches them
        (once the desktop let the window go, the last it had). count says how many of the
        WIN_THEME_SLOTS are filled (WIN_COLORS now; the rest are 0, room for roles to come). A
        window whose program asked it once gets WIN_EV_THEME whenever the theme changes, and asks
        again. (struct win_info keeps its 20 bytes, which programs built before have and
        libsurf's layout check holds, so the theme has a struct of its own; WIN_VERSION stays 1.)
        It is also the test for the ops of 11/05/26: a kernel without them says -EINVAL for it.
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
    11/05/26 (docs/superpowers/plans/2026-11-05-desktop-apps.md, track desk): WIN_THEME above,
    and four ops more. Each checks its argument before anything else: -EFAULT for a struct that is
    not the caller's to read, -EINVAL for what is in it (-ENOMEM when there is no memory to copy it
    into); then -EPIPE once the window is let go (but WIN_TAKE), and -EACCES where the focus is
    needed and the window has it not. A kernel before them says -EINVAL, and one that knows them
    but not yet what they do -ENOSYS; either means: go on without (no menus in the bar, no paste
    but its own, a path typed instead of chosen). WIN_THEME tells a program which kernel it has.
    WIN_MENU (const struct win_menu *): the window's menus, in the desktop's menu bar while the
        window has the focus, in place of the desktop's File and Edit: the bar then reads
        "≋ SurfOS", the window's menus, Apps, Window, Help. Ctrl+G (and Ctrl+Space) opens the bar
        as for the desktop's own windows; the arrows, Enter, Esc and the mouse work in the
        program's menus as in the desktop's. count entries at items (struct win_menu_item),
        copied during the call: each WIN_MENU_TITLE entry is a menu, its label the menu's name in
        the bar, and the entries after it, to the next title, are that menu's items, each an
        item (its label, its shortcut text at the right) or a WIN_MENU_SEPARATOR. The shortcut
        text is only shown: the program sees its keys as keys and acts on them itself. An item
        that is chosen (a click, or Enter) sends WIN_EV_MENU with its id in value and its menu's
        id in x; a WIN_MENU_DISABLED item is greyed and cannot be chosen (a disabled title greys
        its whole menu); a WIN_MENU_CHECKED item has a check mark before it, a WIN_MENU_RADIO
        item a dot (one of a group: the program marks one and clears the others). An item with
        WIN_MENU_SUBMENU opens a submenu, one level deep, in its menu's place as Apps > Programs
        does (Left goes back): the WIN_MENU_CHILD entries right after it, items and separators as
        in a menu (a chosen one's x is still its menu's id). A title with WIN_MENU_HELP is not in
        the bar: its items go into the desktop's Help menu, above the desktop's own, so that there
        is one Help. A label and a shortcut are UTF-8 by the console's rules (driver/cp437.c:
        whole sequences, no overlong form, surrogate or value past U+10FFFF) with no control
        character (C0, DEL or C1), cut between characters to their arrays (WIN_MENU_LABEL,
        WIN_MENU_KEYS bytes with the NUL): a byte that is no such character is -EINVAL, since the
        desktop would draw it a cell of its own.
        The first entry is a title; at most WIN_MENUS menus, each of 1 to WIN_MENU_ITEMS items
        and separators (a submenu's children not counted; a Help menu's WIN_MENU_HELP_ITEMS), a
        submenu of 1 to WIN_MENU_ITEMS, WIN_MENU_ENTRIES entries in all; a title or an item has a
        label that is not empty; the bar's titles' cells (as the desktop draws them), with 3 for
        each title's room around it, come to at most WIN_MENU_BAR, so that the bar fits a
        640 x 400 desktop; no title passes for the desktop's own: none draws ≋ (U+224B, or
        U+2248, which the console draws alike), and none is SurfOS, Apps, Window or Help in any
        case with blanks around it (a WIN_MENU_HELP title may be Help); no flags but those below,
        CHILD only after a SUBMENU item or another child, SUBMENU and CHILD only on items, HELP
        only on one title: -EINVAL otherwise. count 0 takes the window's menus away (the
        desktop's File and Edit come back); a new WIN_MENU replaces the menus whole, so a program
        sends them again when an item's state changes (a menu that is open meanwhile stays open,
        its selection kept where it can be). The ids are the program's own: the desktop only
        sends them back.
    WIN_PASTE (struct win_buffer *): the desktop's clipboard (the text its Copy put there, a
        program's WIN_CLIP included: UTF-8 as a rule, lines ending as the copy had them, no NUL
        in it), copied into buf during the call: its length n in bytes, 0 for an empty
        clipboard, with a NUL after it when size > n. size 0 copies nothing and only says n
        (buf may be NULL then); a size that is not 0 but less than n copies nothing and says
        -ENOSPC (the clipboard holds at most WIN_CLIP_MAX bytes: a buffer of that size always
        takes it). Only while the window has the focus (-EACCES otherwise: a program in the
        background reads nothing the user copied, a password say), as WIN_CLIP is. -EFAULT for
        n bytes (and the NUL) at buf that are not the caller's to write.
    WIN_DIALOG (const struct win_dialog *): the desktop's Open, Save as or Choose folder chooser
        (kind WIN_DLG_OPEN, WIN_DLG_SAVE, WIN_DLG_FOLDER) shown for the window, in front of
        everything and taking the keys until it is answered, as for Notepad. It starts in dir (an
        absolute path; "" or a folder that cannot be read: where the desktop's last chooser was),
        a Save as with name in its box (no '/' in it; "" for none); title is its title bar's ("":
        the chooser's own, "Open a file", "Save as", "Choose a folder"), and the bar names the
        window it is for, so that no program's chooser passes for another's or the desktop's.
        flags is 0 (-EINVAL for any other: room to grow). types lists the file types it is for,
        separated by spaces, at most WIN_TYPES of them within WIN_TYPES_MAX bytes: an extension
        without its dot (letters and digits, 1 to WIN_TYPE_LEN, any case: "png"), or a whole file
        name after '=' (letters, digits, '.', '-' and '_', 1 to WIN_TYPE_LEN: "=Makefile", for a
        name with no extension, matched in any case); "" for every file: Open lists only those
        files (and the folders), and a Save as name without one of them gets the first extension
        given ("cat" becomes "cat.png"; with only whole names, nothing is added).
        A program asks only after the user did something in its window: a WIN_DIALOG needs a
        key, a pointer event with a button held or a WIN_EV_MENU sent to the window since its
        last request (-EACCES otherwise), so that a program that asks again after each answer
        cannot hold the desktop's keys. A Save as answer is a file that is not there, or one that
        is, after the desktop asked "Replace the file?"; never a file the desktop takes care of
        (one that may share its space with a .tmp beside it, docs/GUI.md "Saving": the chooser
        says so, as Notepad's Save as does); the program then writes it as user/bin/saveutil.h
        does. Choose folder lists only folders (Right or a double-click goes into one), and its
        answer is the folder selected in its list, or the one it shows while .. is selected.
        The answer comes as WIN_EV_DIALOG, value 1 when a path was chosen (WIN_TAKE copies it)
        and 0 when the chooser was cancelled (Esc, Cancel, another dialog of the desktop's in its
        place, or the window lost the focus or went before the desktop could show it), x the
        request's id. It always comes: a place in the window's queue is kept for it while a
        request waits, and a desktop that lets the window go answers 0 for it. One request at a
        time for a window (-EBUSY while one waits for its answer); only while the window has the
        focus (-EACCES); -EAGAIN while WIN_EVENTS events wait to be read (no place for the
        answer: read them first). -EINVAL for a kind that is none, flags that are not 0, a dir that is not
        absolute, a name with a '/', types that are not as above, or a dir or a name without its
        NUL within its array or with a control character in it (a title is cut as a window's).
    WIN_TAKE (struct win_buffer *): the path the last WIN_EV_DIALOG chose (absolute, at most
        WIN_PATH_MAX bytes with its NUL), copied into buf with its NUL during the call: its length
        without the NUL, and it is taken (a second WIN_TAKE says -ENOENT). -ENOENT when none
        waits (a request cancelled has none); -ENOSPC when size is less than the length and its
        NUL, and -EFAULT for bytes at buf that are not the caller's to write (the path stays
        either way, for another try). It works after the desktop let the window go too, as
        WIN_INFO does: a path chosen just before is not lost. A path not taken goes at the next
        WIN_DIALOG or with the window.
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
11/05/26: mods is the modifiers held, WIN_MOD_SHIFT, WIN_MOD_ALT and WIN_MOD_CTRL, on every key,
pointer and wheel event (0 on the others; on a pointer or wheel event, those held as the desktop
takes it: Shift+click, Ctrl+wheel), and WIN_MOD_REPEAT on a key the keyboard repeats while it is
held. A program tests the bits it cares about (ev.mods & WIN_MOD_CTRL), never mods as a whole: a
bit it does not use may come with any event. And three events more: WIN_EV_MENU (value: the id of
the item chosen in the window's menus, x: its menu's id), WIN_EV_DIALOG (value: 1 a path was
chosen, WIN_TAKE it; 0 cancelled; x: the request's id) and WIN_EV_THEME (the desktop's theme
changed, value its number: WIN_THEME has its colours; only to a window whose program asked
WIN_THEME). A program ignores an event it does not know.

The keys (11/05/26), exactly. The keyboard driver gives the desktop each key with the modifiers
held as it was pressed (kernel/keyboard.c's keys for a desktop), and the desktop gives a program:
    - a character: its byte (the keyboard types ASCII; Shift is in it, 'A' or '!', and in mods);
      with Ctrl, a letter is its control byte, Ctrl+A 1 to Ctrl+Z 26 (Ctrl+I 9, Ctrl+M 13 among
      them), and Ctrl+[ 27, Ctrl+\ 28, Ctrl+] 29, Ctrl+^ 30, Ctrl+_ and Ctrl+/ 31; any other
      character with Ctrl is itself (Ctrl+0 is '0', Ctrl+= '=', Ctrl+- '-', Ctrl+Shift+= '+');
      each with WIN_MOD_CTRL, and WIN_MOD_SHIFT when Shift was held too (Ctrl+Shift+S: 19, both);
    - Tab 9, Enter 13, Esc 27 and Backspace 127, and with Ctrl held WIN_KEY_TAB, WIN_KEY_ENTER,
      WIN_KEY_ESC and WIN_KEY_BACKSPACE in their place, with WIN_MOD_CTRL, so that Ctrl+Tab is no
      Ctrl+I, Ctrl+Enter no Ctrl+M and Ctrl+Esc no Ctrl+[; Shift+Tab is WIN_KEY_BACKTAB;
    - Alt and a key: the key as without Alt, with WIN_MOD_ALT (Alt+Ctrl+X: 24 with both);
    - the arrows, Home, End, Page Up, Page Down, Insert and Delete: WIN_KEY_*, with every
      modifier held (Ctrl+Shift+Left: WIN_KEY_LEFT, WIN_MOD_CTRL | WIN_MOD_SHIFT);
    - F5 to F12: WIN_KEY_F5 to WIN_KEY_F12, with their modifiers;
    - a key held down comes again as the keyboard repeats it, with WIN_MOD_REPEAT (its first
      press without): a game moves on the repeats, a program that types takes them all.
    Ctrl+G and Ctrl+Space never come (they open the menu bar), nor F1 to F4 (they switch
    consoles), Print Screen (the desktop's screenshot) or Ctrl+Alt+Delete. Keys typed on a
    terminal at the other end of the serial line carry what its escape sequences say, and an
    Esc followed by a key in the same few milliseconds there reads as Alt and that key.

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
#define WIN_MENU        7               /* 11/05/26: the window's menus in the desktop's bar */
#define WIN_PASTE       8               /* 11/05/26: the desktop's clipboard, read */
#define WIN_DIALOG      9               /* 11/05/26: the desktop's Open, Save as or Choose folder chooser */
#define WIN_TAKE        10              /* 11/05/26: the path the chooser chose */
#define WIN_THEME       11              /* 11/05/26: the desktop's theme and its colours */

/* 11/05/26: WIN_THEME: the theme and its colours by role, count of WIN_THEME_SLOTS filled */
#define WIN_COLORS      16              /* the roles below */
#define WIN_THEME_SLOTS 32              /* room for more */
struct win_theme   { unsigned theme, count; unsigned colors[WIN_THEME_SLOTS]; };              /* 136 */
#define WIN_THEME_OCEAN 0
#define WIN_THEME_DUSK  1
#define WIN_COLOR_DESKTOP   0           /* the desktop behind the windows (the one colour ocean and dusk differ in now) */
#define WIN_COLOR_WINDOW    1           /* a window's content: paper */
#define WIN_COLOR_TEXT      2           /* its text: ink */
#define WIN_COLOR_MUTED     3           /* quieter text, hints; a title bar without the focus */
#define WIN_COLOR_ACCENT    4           /* teal: a title bar with the focus, the chosen item, the default button */
#define WIN_COLOR_ON_ACCENT 5           /* text on the accent */
#define WIN_COLOR_BAND      6           /* toolbars, headers, gutters, status bars */
#define WIN_COLOR_BUTTON    7           /* light buttons */
#define WIN_COLOR_LINE      8           /* thin separators, a text box without the focus */
#define WIN_COLOR_SELECTION 9           /* selected text and rows */
#define WIN_COLOR_FIELD     10          /* inside a text box */
#define WIN_COLOR_FRAME     11          /* frames, and dark panels (ink) */
#define WIN_COLOR_DARK      12          /* the darkest: the taskbar, shadows (navy) */
#define WIN_COLOR_LIGHT     13          /* light text on dark (foam) */
#define WIN_COLOR_WARN      14          /* errors, a timer run out (coral) */
#define WIN_COLOR_DISABLED  15          /* greyed items' text */

/* 11/05/26: WIN_MENU */
#define WIN_MENUS        8              /* menus of a window, at most */
#define WIN_MENU_ITEMS   14             /* items and separators of a menu or a submenu, at most (it fits 640 x 400) */
#define WIN_MENU_HELP_ITEMS 11          /* a WIN_MENU_HELP menu's: the desktop's Help adds a separator and its two */
#define WIN_MENU_ENTRIES 128            /* entries of a WIN_MENU, titles and separators included, at most */
#define WIN_MENU_LABEL   48             /* a label's bytes with its NUL (an IRC channel's name is up to 50: cut) */
#define WIN_MENU_KEYS    16             /* a shortcut text's bytes with its NUL ("Ctrl+Shift+Z") */
#define WIN_MENU_BAR     44             /* the titles' characters, 3 counted for each title's room, at most */
#define WIN_MENU_TITLE     0x01         /* a menu: its label is its name in the bar; the entries after it its items */
#define WIN_MENU_SEPARATOR 0x02         /* a line between items (its id, label and keys are not looked at) */
#define WIN_MENU_DISABLED  0x04         /* greyed: it cannot be chosen (a title: none of its menu's items can) */
#define WIN_MENU_CHECKED   0x08         /* a check mark before it */
#define WIN_MENU_RADIO     0x10         /* checked as one of a group: a dot before it */
#define WIN_MENU_SUBMENU   0x20         /* an item that opens a submenu: the WIN_MENU_CHILD entries right after it */
#define WIN_MENU_CHILD     0x40         /* an item or separator of the submenu of the SUBMENU item above it */
#define WIN_MENU_HELP      0x80         /* a title: its items go into the desktop's Help menu (not in the bar) */
struct win_menu_item { unsigned id, flags; char label[WIN_MENU_LABEL]; char keys[WIN_MENU_KEYS]; };   /* 72 */
struct win_menu      { const struct win_menu_item *items; unsigned count; };                        /* 8 */

/* 11/05/26: WIN_PASTE and WIN_TAKE: a buffer of the program's to copy text into */
struct win_buffer    { char *buf; unsigned size; };                                                  /* 8 */

/* 11/05/26: WIN_DIALOG */
#define WIN_DLG_OPEN    1               /* a file to open */
#define WIN_DLG_SAVE    2               /* a name to save as */
#define WIN_DLG_FOLDER  3               /* a folder */
#define WIN_PATH_MAX    256             /* a path's bytes with its NUL (the VFS's longest) */
#define WIN_NAME_MAX    128             /* a Save as name offered: bytes with its NUL */
#define WIN_TYPES_MAX   144             /* the types: bytes with the NUL */
#define WIN_TYPES       16              /* types, at most */
#define WIN_TYPE_LEN    15              /* a type's characters, at most (an extension, or a whole name after '=') */
struct win_dialog {
    unsigned kind, id, flags;           /* WIN_DLG_*; the program's own, WIN_EV_DIALOG's x; 0 */
    char title[WIN_TITLE_MAX];          /* "" for the chooser's own */
    char dir[WIN_PATH_MAX];             /* absolute; "" for the desktop's choice */
    char name[WIN_NAME_MAX];            /* Save as: the name offered, "" */
    char types[WIN_TYPES_MAX];          /* "png bmp", "=Makefile mk", "" for every file */
};                                      /* 604 */

struct win_event { unsigned short type, mods; int x, y; unsigned value; };     /* 16 bytes */
#define WIN_EV_KEY      1
#define WIN_EV_POINTER  2
#define WIN_EV_WHEEL    3
#define WIN_EV_FOCUS    4
#define WIN_EV_RESIZE   5
#define WIN_EV_CLOSE    6
#define WIN_EV_MENU     7               /* 11/05/26: value the item's id, x its menu's id */
#define WIN_EV_DIALOG   8               /* 11/05/26: value 1 chosen (WIN_TAKE), 0 cancelled; x the request's id */
#define WIN_EV_THEME    9               /* 11/05/26: value the theme now (WIN_THEME's colours) */
#define WIN_MOD_SHIFT   1
#define WIN_MOD_ALT     2               /* 11/05/26: as xterm numbers them (its m - 1) */
#define WIN_MOD_CTRL    4               /* 11/05/26 */
#define WIN_MOD_REPEAT  8               /* 11/05/26: a key the keyboard repeats while it is held */

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
/* 11/05/26: Tab, Enter, Esc and Backspace with Ctrl held (without Ctrl: their bytes 9, 13, 27, 127) */
#define WIN_KEY_TAB       0x10c
#define WIN_KEY_ENTER     0x10d
#define WIN_KEY_ESC       0x10e
#define WIN_KEY_BACKSPACE 0x10f
/* 11/05/26: the function keys, WIN_KEY_F(1) to WIN_KEY_F(12); F1 to F4 switch consoles and never come */
#define WIN_KEY_F(n)    (0x110 + (n))
#define WIN_KEY_F5      WIN_KEY_F(5)
#define WIN_KEY_F6      WIN_KEY_F(6)
#define WIN_KEY_F7      WIN_KEY_F(7)
#define WIN_KEY_F8      WIN_KEY_F(8)
#define WIN_KEY_F9      WIN_KEY_F(9)
#define WIN_KEY_F10     WIN_KEY_F(10)
#define WIN_KEY_F11     WIN_KEY_F(11)
#define WIN_KEY_F12     WIN_KEY_F(12)

#define WIN_BUTTON_LEFT   1
#define WIN_BUTTON_RIGHT  2
#define WIN_BUTTON_MIDDLE 4

#ifdef _SURFOS_TYPES_H
/* 11/05/26: a list of file types as WIN_DIALOG's types and the application registry's types and open-with take them
   (rootfs/apps/README): extensions of 1 to WIN_TYPE_LEN letters and digits, or whole names after '=' of 1 to
   WIN_TYPE_LEN letters, digits, '.', '-' and '_', separated by spaces, at most WIN_TYPES of them, within size bytes
   with the NUL. Each is copied into out[] (when out is not NULL) in lower case, a whole name with its '='. How many
   there are (0 for an empty list), or -1 for a list that is none. Pure: the kernel and the desktop's model share it */
static inline int win_types(const char *s, unsigned size, char out[][WIN_TYPE_LEN + 2]) {
    unsigned i = 0, len;
    int n = 0;
    while(i < size && s[i]) {
        int whole = 0;
        if(s[i] == ' ') { i++; continue; }
        if(s[i] == '=') { whole = 1; i++; }
        for(len = 0; i + len < size && s[i + len] && s[i + len] != ' '; len++) {
            char c = s[i + len];
            if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 (whole && (c == '.' || c == '-' || c == '_')))) return -1;
        }
        if(!len || len > WIN_TYPE_LEN || n == WIN_TYPES || i + len >= size) return -1;
        if(out) {
            unsigned k, o = 0;
            if(whole) out[n][o++] = '=';
            for(k = 0; k < len; k++) {
                char c = s[i + k];
                out[n][o++] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
            }
            out[n][o] = 0;
        }
        n++;
        i += len;
    }
    return i < size ? n : -1;                   /* the NUL within size */
}

/* The desktop's side (shell/guiclient.c, through struct gui_sys's win_* calls). A window as the desktop draws it:
   its title; the size of its pixels, which are its content; the limits and flags it asked for (max 0: as large as
   the desktop allows); its background, shown where its pixels do not reach; a serial that moves with every present,
   size and title; the pid of the program that opened it; and (10/25/26, stage C6) whether that program is in the
   foreground job of a pty the desktop adopted, its Terminal's, a job that session's shell made (typed at its prompt
   without &, or started by one that was): the desktop gives such a window the focus when it appears and the Terminal
   has it. 10/26/26: how many times it asked to be seen (WIN_ALERT). 11/05/26: how many times it set its menus
   (WIN_MENU: the view has them), and whether a chooser it asked for waits for the desktop to take it
   (win_dialog_take()). */
struct win_state {
    char title[WIN_TITLE_MAX];
    int w, h, min_w, min_h, max_w, max_h;
    u32 flags, background, serial;
    int pid;
    int foreground;
    u32 alerts;
    u32 menu_serial;
    int dialog;
};
/* its pixels, 0x00RRGGBB in rows of w, lent to the desktop until its next update or close; 11/05/26: and its menus as
   it set them last (menus entries, checked as surfos/win.h says; NULL and 0 for none), lent the same way */
struct win_view { const u32 *pixels; int w, h; const struct win_menu_item *menu; int menus; };

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
/* 11/05/26 (the desktop applications, track desk): the desktop's side of WIN_PASTE, WIN_DIALOG and WIN_TAKE, and the
   theme (a window's menus come with win_update()'s view). win_paste_give: the desktop's clipboard, len bytes of text
   (to a NUL if one comes first; 0: empty), for WIN_PASTE (-ENOMEM, -ENODEV). win_dialog_take: the chooser window h
   asked for, into out, and its number into no: 0, -ENOENT when none waits. win_dialog_answer: the answer to request
   no of window h, path (absolute) or NULL when cancelled, with its WIN_EV_DIALOG; -EINVAL for a request that is not
   the one waiting; -ENOMEM answers it as cancelled. win_theme_set: the desktop's theme and its WIN_COLORS colours,
   for its windows (WIN_THEME) and with WIN_EV_THEME to those that asked. -EINVAL for a handle that is not one of the
   calling task's desktop's; -ENODEV for a task without a desktop. */
int win_paste_give(const char *text, u32 len);
int win_dialog_take(int h, struct win_dialog *out, u32 *no);
int win_dialog_answer(int h, u32 no, const char *path);
int win_theme_set(u32 theme, const u32 *colors);
#endif

#endif
