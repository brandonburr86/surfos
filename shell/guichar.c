/*
SurfOS Desktop: Character Map
--------------------
File: guichar.c Date: 10/17/26
--------------------
The console's 256 characters (code page 437 with SurfOS's three, driver/cp437.c) in a grid of 16 by 16,
drawn with the console's own font. The chosen one is shown large with its code, its Unicode code point
and its UTF-8 bytes. Copy (Enter, Ctrl+C, a double-click, the button) puts its UTF-8 on the desktop's
clipboard, so it pastes into Notepad, whose text is UTF-8, and into the Terminal: the laptop's keyboard
types nothing beyond ASCII. Codes 1 to 31 and 127 are the graphic characters the console draws there
(cp437_utf8() gives U+263A for 1, say), never control characters, and that is what is copied; 0 is the
console's empty cell, which copies nothing.
Track TOOLS of docs/superpowers/plans/2026-10-17-gui-suite.md; the window manager reaches it
through gui_app_charmap (struct gui_app, shell/guipriv.h).
10/18/26: laid out for the height the window gets (on a 640x400 screen the status bar covered the last
row); Go to code takes only what it says it takes.
10/26/26 (stage C12 of docs/superpowers/plans/2026-10-25-app-windows.md): the Character Map is a
program now, user/wcharmap/ (/initrd/bin/wcharmap), out of the kernel: the same grid, panel, keys and
button, in a window of its own that stands where this window stood; Go to code (Ctrl+L) is a band of
that window, where the desktop's dialog was. What stays here is its entry in the suite's table: the
tile, the Apps menu and Next window's list start the program, or bring its window back (guicore.c's
launcher), and Help keeps its lines.
*/
#include "gui_tools.h"

static const char *const help[] = {
    "#Character Map",
    "The console's 256 characters (code page 437). Click one,",
    "or move with the arrows: Home and End go to the start and",
    "the end of the row, Page Up and Page Down to the top and",
    "the bottom of the column, and a key typed to its own",
    "character. Ctrl+L goes to a code (247, 0xF7, U+224B).",
    "Enter, Ctrl+C or a double-click copies the character as",
    "UTF-8: Ctrl+V pastes it in Notepad or the Terminal.",
    "Codes 1-31 and 127 copy the symbols the console shows",
    "there; 0, the empty cell, copies nothing. The Character",
    "Map is the program wcharmap, in a window of its own.",
};

const struct gui_app gui_app_charmap = {
    .title = "Character Map", .icon = ICON_CHARMAP, .shortcut = "", .w = 460, .h = 360,
    .help = help, .help_lines = (int)(sizeof(help) / sizeof(help[0])),
    .program = "/initrd/bin/wcharmap",
};
