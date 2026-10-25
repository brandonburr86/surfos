/*
SurfOS Desktop: Programs' Windows
--------------------
File: gui_client.h Date: 10/25/26
--------------------
The desktop's record of each window an ordinary program opened (milestone M12,
docs/superpowers/plans/2026-10-25-app-windows.md): GUI_CLIENTS slots after the fixed windows,
window numbers from GUI_FIRST_CLIENT on (shell/guiclient.c). A slot holds the system's handle
(struct gui_sys's win_* calls), the window's state and pixels as its last update gave them, and
what the desktop last told its program: the buttons and where the pointer was, the focus, the
size it asked for, and whether the close button was pressed. 10/25/26 (the review): whether it
waits for the focus it opened without.
*/
#ifndef _SYS_GUI_CLIENT_H
#define _SYS_GUI_CLIENT_H
#include <surfos/types.h>
#include <surfos/win.h>

struct gui_client {
    bool used;
    int handle;                         /* the system's */
    struct win_state st;                /* from the last update */
    struct win_view view;               /* its pixels, lent until the next update */
    unsigned buttons;                   /* the buttons last sent */
    int px, py;                         /* where the pointer was sent last, from the content's top left */
    bool focused;                       /* what FOCUS said last */
    bool asked;                         /* a RESIZE to sent_w x sent_h waits for the program's image */
    int sent_w, sent_h;
    bool closing;                       /* CLOSE was sent: the close button asks to end the program */
    bool waiting;                       /* 10/25/26: opened without the focus; its taskbar button is marked until
                                           it has it */
};

struct gui_clients {
    struct gui_client slot[GUI_CLIENTS];
    int grab;                           /* the window whose content had a press, while a button is held; -1 */
    int hover;                          /* the window whose content the pointer is over; -1 */
    int opened;                         /* windows opened so far: where the next one goes (a cascade) */
};
#endif
