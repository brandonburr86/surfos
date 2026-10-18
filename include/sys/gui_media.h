/*
SurfOS Desktop: Image Viewer and Hex
--------------------
File: gui_media.h Date: 10/17/26
--------------------
Their state in gui_state (shell/guiview.c, shell/guihex.c). Included by sys/gui.h; owned by
track MEDIA of docs/superpowers/plans/2026-10-17-gui-suite.md.
*/
#ifndef _SYS_GUI_MEDIA_H
#define _SYS_GUI_MEDIA_H

struct gui_viewer {
    char path[GUI_PATH_MAX];            /* the picture shown, "" for none */
    char status[GUI_STATUS];
};

struct gui_hex {
    char path[GUI_PATH_MAX];            /* the file shown, "" for none */
    u32 size, top;                      /* its size; the first byte shown */
    char status[GUI_STATUS];
};
#endif
