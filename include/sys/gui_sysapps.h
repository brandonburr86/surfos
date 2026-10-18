/*
SurfOS Desktop: System Monitor and Disks
--------------------
File: gui_sysapps.h Date: 10/17/26
--------------------
Their state in gui_state (shell/guimon.c, shell/guidisks.c). Included by sys/gui.h; owned by
track SYS of docs/superpowers/plans/2026-10-17-gui-suite.md. System Monitor reads the tasks,
memory and the machine about once a second while its window is open and in view; its
Processes page is in the order of a column, and the selection follows a pid from one reading
to the next. Disks reads the block devices about every two seconds, and its selection
follows a device's name.
*/
#ifndef _SYS_GUI_SYSAPPS_H
#define _SYS_GUI_SYSAPPS_H

/* System Monitor's pages, and the columns of Processes, which it sorts by */
#define GUI_MON_PROCESSES 0
#define GUI_MON_MEMORY 1
#define GUI_MON_SYSTEM 2
#define GUI_MON_PAGES 3
#define GUI_MON_PID 0
#define GUI_MON_NAME 1
#define GUI_MON_STATE 2
#define GUI_MON_CONSOLE 3
#define GUI_MON_KIND 4
#define GUI_MON_COLUMNS 5

struct gui_monitor {
    int page;                           /* the tab shown: GUI_MON_* */
    u32 read_ms;                        /* when the lists were last read (gui_state.now) */
    bool read;                          /* they were read since the window opened */
    int ntasks, selected, top;          /* task[] in the order shown; selected: its row, -1 none */
    int pid;                            /* the selected task's pid, which the selection follows (-1: none) */
    int sort;                           /* the column of the order (GUI_MON_PID ...) */
    bool descending;
    bool thumb;                         /* the scroll bar's thumb is held, scroll_at pixels below its top */
    int scroll_at;
    int err_tasks, err_memory, err_machine;   /* each reading's -errno, 0 */
    char find[GUI_TASK_NAME];           /* what Find looks for (Find next) */
    struct gui_task task[GUI_TASK_MAX];
    struct gui_memory mem;
    struct gui_machine machine;
    char status[GUI_STATUS];
};

struct gui_disks {
    u32 read_ms;
    bool read;
    int count, selected, top;           /* disk[] in lsblk's order; selected: its row, -1 none */
    int err;                            /* the reading's -errno, 0 */
    char name[16];                      /* the selected device's name, which the selection follows */
    bool thumb;                         /* the scroll bar's thumb is held, scroll_at pixels below its top */
    int scroll_at;
    u32 click_ms;                       /* the last click on a row, for a double-click */
    int click_row;
    struct gui_disk disk[GUI_DISK_MAX];
    char status[GUI_STATUS];
};
#endif
