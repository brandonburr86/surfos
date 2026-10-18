/*
SurfOS Desktop: System Monitor and Disks
--------------------
File: gui_sysapps.h Date: 10/17/26
--------------------
Their state in gui_state (shell/guimon.c, shell/guidisks.c). Included by sys/gui.h; owned by
track SYS of docs/superpowers/plans/2026-10-17-gui-suite.md.
*/
#ifndef _SYS_GUI_SYSAPPS_H
#define _SYS_GUI_SYSAPPS_H

struct gui_monitor {
    int page;                           /* the tab shown */
    u32 read_ms;                        /* when the lists were last read (gui_state.now) */
    int ntasks, selected, top;
    struct gui_task task[GUI_TASK_MAX];
    struct gui_memory mem;
    struct gui_machine machine;
    char status[GUI_STATUS];
};

struct gui_disks {
    u32 read_ms;
    int count, selected, top;
    struct gui_disk disk[GUI_DISK_MAX];
    char status[GUI_STATUS];
};
#endif
