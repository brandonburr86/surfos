/*
SurfOS user program: wsettings
--------------------
File: main.c    Date: 11/05/26 (the desktop applications, track settings)
--------------------
Settings: what the wifi and settings commands do, in a window on the desktop (window.c, on the
toolkit gk.h; the model is settings.c, the kernel's side SYS_netctl in kernel/netctl.c): the
Wi-Fi card and its networks, joining one with its password and saving it, the saved networks,
Ethernet's DHCP and the host name, the desktop's theme and pointer speed, telnetd at startup and
the shell's history, and what SurfOS runs on. Its loop is the toolkit's pattern: gk_next() until
the window closes or goes. The desktop's Apps menu and its tile start it (rootfs/apps/wsettings.app).

wsettings     the window, on the desktop of the terminal it was started from
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "window.h"
#include "sys.h"

int main(int argc, char **argv) {
    static struct wswin s;
    struct gk_event ev;
    (void)argv;
    if(argc > 1) {
        fprintf(stderr, "usage: wsettings\n");
        return 2;
    }
    if(wsw_open(&s, ws_sys_netctl, NULL, ws_sys_about) < 0) {
        fprintf(stderr, "wsettings: no window: %s\n", errno == ENODEV ? "this terminal is no desktop's" : strerror(errno));
        return 1;
    }
    while(gk_next(s.ui, &ev, -1) > 0) if(!wsw_event(&s, &ev)) break;
    wsw_close(&s);
    return 0;
}
