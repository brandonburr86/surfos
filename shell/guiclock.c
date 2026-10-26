/*
SurfOS Desktop: Clock
--------------------
File: guiclock.c Date: 10/17/26
--------------------
Four pages, chosen in the toolbar or with the keys 1 to 4: the time and the date large, from the PC's
clock (gui_state.time, which shell/gui.c reads from the CMOS clock each second, as for the taskbar's
clock); a month's calendar, its weeks from Sunday, today marked; a stopwatch with laps; and a timer.
The weekday is worked out from the date, since firmware does not always keep the CMOS clock's own.
The stopwatch and the timer count on gui_state.now (milliseconds since boot), not on frames, so they
keep running while the window is minimized or behind another. gui_tick() watches the timer: at zero
the window's title, and so its taskbar button, says "Time's up!" and a message comes up in front of
everything (after a dialog that is open has been answered). Closing the window ends both; while one
of them runs, closing it or leaving the desktop asks first.
Track TOOLS of docs/superpowers/plans/2026-10-17-gui-suite.md; the window manager reaches it
through gui_app_clock (struct gui_app, shell/guipriv.h).
10/26/26 (stage C12 of docs/superpowers/plans/2026-10-25-app-windows.md): the Clock is a program
now, user/wclock/ (/initrd/bin/wclock), out of the kernel: the same pages, keys, buttons and
drawing, in a window of its own that stands where this window stood. What stays here is its entry
in the suite's table: the tile, the Apps menu and Next window's list start the program, or bring
its window back (guicore.c's launcher), and Help keeps its lines. The alarm is the program's window
shown again in front of the others, without the focus, rather than a message of the desktop's.
*/
#include "gui_tools.h"

static const char *const help[] = {
    "#Clock",
    "The keys 1 to 4 choose the page: the time and the date,",
    "the calendar, the stopwatch, the timer.",
    "Calendar: Left and Right (or Page Up and Page Down) a",
    "month, Up and Down a year, Home this month again.",
    "Stopwatch: Space starts and stops it, L takes a lap,",
    "R resets it. Timer: Up and Down set the minutes, Left",
    "and Right the seconds (Page Up, Page Down: ten), Home",
    "0:00; Space starts or pauses it, R resets it. At zero",
    "the title says so and the window comes up in front,",
    "even when it is tucked away; a key or a click in it",
    "answers. Both keep running while the window is",
    "minimized; closing it ends them (it asks first).",
    "Ctrl+C copies what the page shows. The Clock is the",
    "program wclock, in a window of its own.",
};

const struct gui_app gui_app_clock = {
    .title = "Clock", .icon = ICON_CLOCK, .shortcut = "", .w = 380, .h = 300,
    .help = help, .help_lines = (int)(sizeof(help) / sizeof(help[0])),
    .program = "/initrd/bin/wclock",
};
