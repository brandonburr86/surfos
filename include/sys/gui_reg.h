/*
SurfOS Desktop: the Application Registry
--------------------
File: gui_reg.h Date: 11/05/26
--------------------
The programs of /initrd/apps/<name>.app (rootfs/apps/README is the format; docs/superpowers/plans/
2026-11-05-desktop-apps.md, track desk), as the desktop keeps them: read once when it starts
(shell/guireg.c, through struct gui_fs), every length and count bounded here, nothing allocated.
*/
#ifndef _SYS_GUI_REG_H
#define _SYS_GUI_REG_H
#include <surfos/types.h>
#include <surfos/win.h>

#define GUI_REG_MAX 32                  /* entries, the first .app files by name */
#define GUI_REG_FILE 4096               /* a .app file's bytes, at most */
#define GUI_REG_LINE 128                /* a line's, without its end */
#define GUI_REG_NAME 32                 /* a program's name, a title: bytes with the NUL */
#define GUI_REG_TYPES WIN_TYPES         /* types of a key, at most (surfos/win.h's win_types() reads them) */
#define GUI_REG_TYPE (WIN_TYPE_LEN + 2) /* a type's characters ('=' and a whole name, or an extension) with the NUL */
#define GUI_REG_FILENAME 64             /* a .app file's name with the NUL */
#define GUI_REG_ICON 16                 /* its icon's side */

struct gui_reg {
    char file[GUI_REG_FILENAME];        /* its .app file, for the log's lines */
    char program[GUI_REG_NAME];         /* in /initrd/bin */
    char title[GUI_REG_NAME];
    char types[GUI_REG_TYPES][GUI_REG_TYPE];        /* a double-click in Files opens these here (lower case) */
    char open_with[GUI_REG_TYPES][GUI_REG_TYPE];    /* Open with offers it for these too */
    int ntypes, nopen;
    bool apps, desktop, icon_set;
    char icon[GUI_REG_ICON][GUI_REG_ICON];          /* rows of the palette's letters, '.' for nothing */
    int app;                            /* the desktop's application whose program it is (its window number), -1 */
};

/* the registry: count entries in file-name order, by[] their numbers in their titles' order; and, for each, the
   program the desktop last started for it from Apps or its tile (pid, when, its window not come yet), and the
   programs it started with a file (opened[]: pid and entry, the newest first), whose windows get its icon */
#define GUI_REG_OPENED 8
struct gui_registry {
    bool loaded;
    int count;
    struct gui_reg e[GUI_REG_MAX];
    int by[GUI_REG_MAX];
    int pid[GUI_REG_MAX];
    u32 at[GUI_REG_MAX];
    bool starting[GUI_REG_MAX];
    int opened_pid[GUI_REG_OPENED], opened_entry[GUI_REG_OPENED];
    char text[GUI_REG_FILE + 1];        /* a file being read */
};

/* a .app file's text as an entry: 0, or -1 with why ("line 4: types: ...") */
int gui_reg_parse(const char *text, u32 len, struct gui_reg *out, char *why, size_t n);
u32 gui_reg_color(char c);              /* a palette letter's colour (0x00RRGGBB); 0xFF000000 for '.' and the rest */
#endif
