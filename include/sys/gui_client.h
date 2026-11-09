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
waits for the focus it opened without. 10/26/26 (stage C12): whether it asked to be seen, and
when it came up for it (a press on it is nobody's for GUI_GRACE_MS after). 11/05/26 (the desktop
applications, track desk): its menus as it set them last (WIN_MENU), which its view lends with
the pixels, and the chooser shown for it, with the request's number, which its answer names.
*/
#ifndef _SYS_GUI_CLIENT_H
#define _SYS_GUI_CLIENT_H
#include <surfos/types.h>
#include <surfos/win.h>

struct gui_client {
    bool used;
    int handle;                         /* the system's */
    struct win_state st;                /* from the last update */
    struct win_view view;               /* its pixels, lent until the next update (11/05/26: and its menus) */
    unsigned buttons;                   /* the buttons last sent */
    int px, py;                         /* where the pointer was sent last, from the content's top left */
    bool focused;                       /* what FOCUS said last */
    bool asked;                         /* a RESIZE to sent_w x sent_h waits for the program's image */
    int sent_w, sent_h;
    int closes;                         /* 11/05/26 (the review of stage 2): CLOSEs sent (2 at most counted); 11/09/26
                                           (the second review): since an "End the program?" answered Cancel */
    bool waiting;                       /* 10/25/26: opened without the focus; its taskbar button is marked until
                                           it has it */
    bool alerted;                       /* 10/26/26: it asked to be seen, held while a button or a drag is under way */
    bool raised;                        /* 10/26/26: it came up for that, at raised_at (the desktop's now, ms) */
    u32 raised_at;
    int chooser;                        /* 11/05/26: the kind of the chooser shown for it (WIN_DLG_*), 0: none, */
    u32 request;                        /* and its request's number (win_answer names it) */
};

#define GUI_GRACE_MS 500                /* 10/26/26: a window that came up by itself takes no press for this long */

#define GUI_LAUNCHED 4                  /* 10/25/26: programs the launcher started lately, whose window takes the focus */
struct gui_clients {
    struct gui_client slot[GUI_CLIENTS];
    int launched[GUI_LAUNCHED];         /* 10/25/26: their pids (0: none), the first window of each takes the focus */
    int grab;                           /* the window whose content had a press, while a button is held; -1 */
    int hover;                          /* the window whose content the pointer is over; -1 */
    int opened;                         /* windows opened so far: where the next one goes (a cascade) */
    bool ends_fresh;                    /* 11/09/26: the ends kept from before this desktop were let go (client_ended()) */
};
#endif
