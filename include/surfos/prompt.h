/*
SurfOS shell prompt
----------------------
File: prompt.h  Date: 10/6/26
----------------------
The one place the SurfOS prompt is drawn (docs/superpowers/plans/2026-10-06-surfsh.md).
The working directory becomes breadcrumbs with "▸" between the names, and the wave "≋"
and a state character come before ">":

    initrd ▸ bin ≋>            normal
    initrd ▸ bin ≋!>           the previous command failed
    SURF ≋#>                   elevated: the kernel's own ring-0 shell
    atlas ▸ home ▸ brandon ≋@> a remote session, its host name first (SurfOS has none yet)
    SURF ≋?>                   a diagnostic or recovery console

The root is "SURF". Only one state is shown, the first of: diagnostic, elevated, remote,
failed, normal. A deep or long path keeps its last names after "…": at most max_depth
items, and at most half the terminal's width. Without Unicode the same prompt is drawn in
ASCII: "initrd > bin ~>", "~!>", "~#>", "...". With colour the prompt is light blue, as
SurfOS's prompt always was, and a state character has its own colour.

prompt.c is plain C without library calls, so the kernel's shell (shell/main.c), the user
shell (user/bin/surfsh.c, through libsurf) and the host tests build the same file. This
header uses no SurfOS types for the same reason; user programs reach it through
user/include/surfos/prompt.h.
*/

#ifndef _SURFOS_PROMPT_H
#define _SURFOS_PROMPT_H

enum prompt_state { PROMPT_NORMAL, PROMPT_FAILED, PROMPT_REMOTE, PROMPT_ELEVATED, PROMPT_DIAGNOSTIC };

struct prompt_ctx {
    const char *cwd;            /* the absolute working directory; "/" (or NULL) is the root */
    const char *hostname;       /* the first breadcrumb of a remote session; NULL or "" otherwise */
    int last_status;            /* the previous command's status: not 0 means it failed */
    int elevated;               /* the shell runs with system privileges (the ring-0 shell) */
    int remote;                 /* the session is on another SurfOS system */
    int diagnostic;             /* a debugger, a diagnostic or a recovery console */
    int width;                  /* the terminal's columns; 0: 80 */
    int unicode;                /* ▸ ≋ …; 0: the ASCII fallback (> ~ ...) */
    int color;                  /* ANSI colours in the SurfOS palette; 0: plain text */
    int max_depth;              /* breadcrumb items at most, "…" included; 0: 5 */
};

#define PROMPT_MAX 640          /* bytes always enough for prompt_render() */

enum prompt_state prompt_state(const struct prompt_ctx *c);

/* The prompt, ending in "> ", NUL-terminated. The number of bytes, or -1 when it did not
   fit in size (out is then cut short, still terminated). */
int prompt_render(const struct prompt_ctx *c, char *out, int size);

/* the columns a rendered prompt takes: its UTF-8 characters, escape sequences left out */
int prompt_columns(const char *rendered);

#endif
