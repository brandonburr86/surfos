/*
SurfOS Desktop: Disks' Side of struct gui_sys
--------------------
File: guisys_disk.c Date: 10/17/26
--------------------
The block devices for Disks (struct gui_disk), as lsblk lists them; track SYS of
docs/superpowers/plans/2026-10-17-gui-suite.md. A placeholder until the track's work lands.
*/
#include <surfos/types.h>
#include <fs/vfs.h>
#include "guisys.h"

int guisys_disks(void *ctx, struct gui_disk *out, int max) {
    (void)ctx; (void)out; (void)max;
    return -ENOSYS;
}
