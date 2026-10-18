/*
SurfOS Desktop: Terminal
--------------------
File: gui_term.h Date: 10/17/26
--------------------
The Terminal's state in gui_state (shell/guiterm.c): a shell session on a pseudo-terminal
(struct gui_sys's term_*) and the screen it draws. Included by sys/gui.h; owned by the
Terminal (track TERM of docs/superpowers/plans/2026-10-17-gui-suite.md).
*/
#ifndef _SYS_GUI_TERM_H
#define _SYS_GUI_TERM_H

struct gui_term {
    int handle;                         /* the session (struct gui_sys), -1 when there is none */
    bool ended;                         /* the session ended (exit): the window says so */
    char status[GUI_STATUS];
};
#endif
