/* Host tests for System Monitor and Disks (track SYS of docs/superpowers/plans/2026-10-17-gui-suite.md), against the
   in-memory file system and system of tests/host/gui_fake.c. Built and run by `make unittest` (unittest-gui).
   10/17/26: the Processes page's order and its selection, End task's question, both answers and the refusals, the
   pages, the Edit menu, Disks' tree, Mount and Eject (the fake file system's, watched here), a disk that goes away,
   a desktop without a system or files, and every window inside guard pixels at three sizes. */
#include "gui_fake.h"

static struct gui_state g;
static const int ids[] = { GUI_MONITOR, GUI_DISKS };
#define IDS (int)(sizeof(ids) / sizeof(ids[0]))

/**** helpers ****/

static void key(int k) { gui_key(&g, k); }
static void later(u32 ms) { g.now += ms; gui_tick(&g); }

/* s drawn at x, y in fg (its lit pixels; the cells' background is the row's) */
static bool shows(int width, int x, int y, const char *s, u32 fg) {
    int i, row, col;
    for(i = 0; s[i]; i++)
        for(row = 0; row < 16; row++)
            for(col = 0; col < 8; col++)
                if((font8x16[(u8)s[i]][row] & (0x80 >> col)) && fake_pixel(width, x + i * 8 + col, y + row) != fg)
                    return false;
    return true;
}

static bool clip_is(const char *s) { return g.clip_len == strlen(s) && !memcmp(g.clip, s, g.clip_len); }
static bool clip_has(const char *s) {
    u32 n = strlen(s), i;
    for(i = 0; i + n <= g.clip_len; i++) if(!memcmp(g.clip + i, s, n)) return true;
    return false;
}

/* System Monitor's row of a pid, -1 */
static int row_of(int pid) {
    int i;
    for(i = 0; i < g.monitor.ntasks; i++) if(g.monitor.task[i].pid == pid) return i;
    return -1;
}
static int selected_pid(void) { return g.monitor.selected >= 0 ? g.monitor.task[g.monitor.selected].pid : -1; }
static bool order_is(const int *pids, int n) {
    int i;
    if(g.monitor.ntasks != n) return false;
    for(i = 0; i < n; i++) if(g.monitor.task[i].pid != pids[i]) return false;
    return true;
}

/* a click in a window, window-relative */
static void click_in(int id, int x, int y) { fake_click(&g, g.win[id].x + x, g.win[id].y + y); }
#define HEAD_Y (TITLE_H + BAR_H + 3 + 10)      /* the middle of a list's heading row */
#define LIST_Y (TITLE_H + BAR_H + 3 + 20)      /* a list's first row */
#define TOOL_Y (TITLE_H + 4 + 11)              /* the middle of a toolbar's buttons */

/* the tasks of gui_fake.c (idle 0, init 1, net 2, surfsh 5 on F1, edit 9 on F2) and one in a terminal */
static void add_task(int pid, int state, int tty, u32 flags, const char *name) {
    struct gui_task *t = &fake_tasks[fake_ntasks++];
    memset(t, 0, sizeof(*t));
    t->pid = pid; t->ppid = 1; t->state = state; t->tty = tty; t->flags = flags;
    strlcpy(t->name, name, sizeof(t->name));
}
static void drop_task(int pid) {
    int i;
    for(i = 0; i < fake_ntasks && fake_tasks[i].pid != pid; i++) ;
    if(i == fake_ntasks) return;
    memmove(&fake_tasks[i], &fake_tasks[i + 1], (fake_ntasks - i - 1) * sizeof(fake_tasks[0]));
    fake_ntasks--;
}

/**** the disks: the kernel's on the test image (the initrd, the boot stick usb0, the data stick usb1) ****/

static struct gui_disk *add_disk(const char *name, const char *disk, const char *bus, const char *type, const char *label,
                                 const char *status, const char *size, u32 kb, bool part) {
    struct gui_disk *d = &fake_disks[fake_ndisks++];
    memset(d, 0, sizeof(*d));
    strlcpy(d->name, name, sizeof(d->name));
    strlcpy(d->disk, disk, sizeof(d->disk));
    strlcpy(d->bus, bus, sizeof(d->bus));
    strlcpy(d->type, type, sizeof(d->type));
    strlcpy(d->label, label, sizeof(d->label));
    strlcpy(d->status, status, sizeof(d->status));
    strlcpy(d->size, size, sizeof(d->size));
    d->size_kb = kb;
    d->part = part;
    d->removable = !strcmp(bus, "usb");
    return d;
}

static void disks_reset(void) {
    struct gui_disk *d;
    fake_ndisks = 0;
    d = add_disk("rd0", "rd0", "ram", "disk", "initrd.tar", "mounted", "7M", 7469, false);
    d->mounted = d->readonly = true;
    strlcpy(d->mount, "/initrd", sizeof(d->mount));
    strlcpy(d->fs, "tarfs", sizeof(d->fs));
    add_disk("usb0", "usb0", "usb", "disk gpt", "QEMU QEMU HARDDISK", "boot drive: protected", "256M", 262144, false);
    d = add_disk("usb0p1", "usb0", "usb", "part gpt EFI system", "SURFOSUSB", "boot drive: protected", "254M", 260096, true);
    d->readonly = true;
    add_disk("usb1", "usb1", "usb", "disk mbr", "QEMU QEMU HARDDISK", "", "64M", 65536, false);
    add_disk("usb1p1", "usb1", "usb", "part mbr FAT32 LBA", "", "", "63M", 64512, true);
}

static int disk_row(const char *name) {
    int i;
    for(i = 0; i < g.disks.count; i++) if(!strcmp(g.disks.disk[i].name, name)) return i;
    return -1;
}
static struct gui_disk *fake_disk(const char *name) {
    int i;
    for(i = 0; i < fake_ndisks; i++) if(!strcmp(fake_disks[i].name, name)) return &fake_disks[i];
    return NULL;
}
static void select_disk(const char *name) {
    key(GUI_HOME);
    while(g.disks.selected >= 0 && strcmp(g.disks.disk[g.disks.selected].name, name) && g.disks.selected < g.disks.count - 1)
        key(GUI_DOWN);
}

/* the fake file system with a Mount and an Eject that do to fake_disks what the shell's do to the kernel's devices */
static struct gui_fs fs;
static char mounted_dev[16], ejected_dev[16];
static int mount_fails;

static int t_mount(void *ctx, const char *dev, char *msg, size_t n) {
    struct gui_disk *d = fake_disk(dev);
    char path[GUI_PATH_MAX];
    (void)ctx;
    strlcpy(mounted_dev, dev, sizeof(mounted_dev));
    if(!d || mount_fails) { snprintf(msg, n, "mount: %s: no file system found (mkfs makes one)", dev); return -1; }
    snprintf(path, sizeof(path), "/%s", dev);
    d->mounted = true;
    strlcpy(d->mount, path, sizeof(d->mount));
    strlcpy(d->status, "mounted", sizeof(d->status));
    strlcpy(d->fs, "fat32", sizeof(d->fs));
    strlcpy(d->volume, "APPSTICK", sizeof(d->volume));
    d->used_kb = 6200;
    d->free_kb = 57300;
    fake_dir(path);
    snprintf(msg, n, "%s on %s: fat32, label APPSTICK, 62 MB, 56 MB free, read-write", dev, path);
    return 0;
}

static int t_eject(void *ctx, const char *dev, char *msg, size_t n) {
    int i;
    (void)ctx;
    strlcpy(ejected_dev, dev, sizeof(ejected_dev));
    for(i = 0; i < fake_ndisks; i++) {
        if(strcmp(fake_disks[i].disk, dev) || !fake_disks[i].mounted) continue;
        fake_disks[i].mounted = false;
        fake_disks[i].mount[0] = fake_disks[i].fs[0] = fake_disks[i].volume[0] = 0;
        fake_disks[i].used_kb = fake_disks[i].free_kb = 0;
        fake_disks[i].status[0] = 0;
    }
    snprintf(msg, n, "%s: safe to remove", dev);
    return 0;
}

static void desktop(int width, int height) {
    fake_desktop(&g, width, height);
    fs = fake_fs;
    fs.mount = t_mount;
    fs.eject = t_eject;
    g.fs = &fs;
    mounted_dev[0] = ejected_dev[0] = 0;
    mount_fails = 0;
    disks_reset();
}

/**** every window, every page, inside the screen ****/

/* each window opens, ticks, draws inside the screen (each page; also maximized and hanging off the edge) and closes */
static void windows(int width, int height) {
    struct fb_surface screen;
    struct gui_disk *d;
    int i, k, p;
    desktop(width, height);
    d = add_disk("hda", "hda", "ata", "disk mbr", "QEMU HARDDISK WITH A MODEL NAME THAT IS LONG", "internal: never formatted",
                 "1023M", 1047552, false);
    d = add_disk("usb2p1", "usb2", "usb", "part mbr FAT16 LBA", "", "unplugged: eject it", "15G", 15728640, true);
    d->gone = d->mounted = true;
    strlcpy(d->mount, "/usb2p1", sizeof(d->mount));
    fake_screen(&screen, width, height);
    for(i = 0; i < IDS; i++) {
        gui_open(&g, ids[i]);
        CHECK(g.win[ids[i]].open && g.focus == ids[i]);
        for(k = 0; k < 3; k++) later(1000);
        for(p = 0; p < 3; p++) {                        /* the monitor's pages; Disks' rows */
            gui_render(&g, &screen);
            CHECK(fake_guards(width, height));
            key(GUI_RIGHT);
            key(GUI_DOWN);
        }
        gui_act(&g, ACT_MAXIMIZE);
        CHECK(g.win[ids[i]].w <= width && g.win[ids[i]].h <= height);
        for(p = 0; p < 3; p++) {
            gui_render(&g, &screen);
            CHECK(fake_guards(width, height));
            key(GUI_RIGHT);
            key(GUI_END);
        }
        gui_act(&g, ACT_MAXIMIZE);
        g.win[ids[i]].x = width - 10;
        g.win[ids[i]].y = height - 10;
        gui_render(&g, &screen);
        CHECK(fake_guards(width, height));
        g.win[ids[i]].w = gui_app_of(ids[i])->min_width();   /* as small as the corner grip makes it */
        g.win[ids[i]].h = 200;
        gui_place(&g, ids[i]);
        for(p = 0; p < 3; p++) {
            gui_render(&g, &screen);
            CHECK(fake_guards(width, height));
            key(GUI_RIGHT);
        }
    }
    gui_open(&g, GUI_MONITOR);
    key(GUI_END);                                       /* edit: a program, which End task asks about */
    key(GUI_DELETE);
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM);
    gui_render(&g, &screen);
    CHECK(fake_guards(width, height));
    key(27);
    gui_finish(&g);
    for(i = 0; i < IDS; i++) {
        gui_open(&g, ids[i]);
        gui_act(&g, ACT_CLOSE);
        CHECK(!g.win[ids[i]].open);
    }
}

/* the windows fit 640x400, maximized too, and are never narrower than their toolbars */
static void sizes(void) {
    int i;
    desktop(640, 400);
    for(i = 0; i < IDS; i++) {
        const struct gui_window *w = &g.win[ids[i]];
        gui_open(&g, ids[i]);
        CHECK(w->x >= 0 && w->y >= MENU_H && w->x + w->w <= 640 && w->y + w->h <= 400 - TASK_H);
        CHECK(gui_app_of(ids[i])->min_width() <= w->w);
        gui_act(&g, ACT_MAXIMIZE);
        CHECK(w->max && w->w == 640 && w->h == 400 - MENU_H - TASK_H);
        gui_act(&g, ACT_MAXIMIZE);
    }
}

/**** System Monitor ****/

static void monitor_order(void) {
    static const int by_pid[] = { 0, 1, 2, 5, 9 }, by_name[] = { 9, 0, 1, 2, 5 }, by_name_r[] = { 5, 2, 1, 0, 9 };
    static const int by_state[] = { 5, 0, 2, 1, 9 }, by_state_r[] = { 1, 9, 2, 0, 5 }, by_console[] = { 5, 9, 0, 1, 2 };
    static const int by_kind[] = { 1, 5, 9, 0, 2 };
    struct fb_surface screen;
    desktop(960, 540);
    gui_open(&g, GUI_MONITOR);
    CHECK(order_is(by_pid, 5) && g.monitor.selected == 0 && selected_pid() == 0);
    key(GUI_DOWN);
    key(GUI_DOWN);
    CHECK(selected_pid() == 2);
    key('s');                                           /* by name */
    CHECK(g.monitor.sort == GUI_MON_NAME && order_is(by_name, 5) && selected_pid() == 2 && g.monitor.selected == 3);
    key('r');
    CHECK(g.monitor.descending && order_is(by_name_r, 5) && selected_pid() == 2);
    click_in(GUI_MONITOR, 210, HEAD_Y);                 /* the State heading */
    CHECK(g.monitor.sort == GUI_MON_STATE && !g.monitor.descending && order_is(by_state, 5));
    click_in(GUI_MONITOR, 210, HEAD_Y);                 /* again: reversed */
    CHECK(g.monitor.descending && order_is(by_state_r, 5) && selected_pid() == 2);
    click_in(GUI_MONITOR, 290, HEAD_Y);
    CHECK(g.monitor.sort == GUI_MON_CONSOLE && order_is(by_console, 5));
    click_in(GUI_MONITOR, 360, HEAD_Y);
    CHECK(g.monitor.sort == GUI_MON_KIND && order_is(by_kind, 5) && selected_pid() == 2);
    /* drawn: the heading sorted by (with its arrow), the selected row */
    fake_screen(&screen, 960, 540);
    gui_render(&g, &screen);
    CHECK(shows(960, g.win[GUI_MONITOR].x + 344 + 8, g.win[GUI_MONITOR].y + HEAD_Y - 8, "Kind", INK));
    CHECK(shows(960, g.win[GUI_MONITOR].x + 62 + 8, g.win[GUI_MONITOR].y + LIST_Y + 1, "init", INK));
    CHECK(shows(960, g.win[GUI_MONITOR].x + 344 + 8, g.win[GUI_MONITOR].y + LIST_Y + 1, "program", INK));
    CHECK(shows(960, g.win[GUI_MONITOR].x + 272 + 8, g.win[GUI_MONITOR].y + LIST_Y + 18 + 1, "F1", MUTED));
    CHECK(fake_pixel(960, g.win[GUI_MONITOR].x + 300, g.win[GUI_MONITOR].y + LIST_Y + 4 * 18 + 8) == PICK);   /* net, row 4 */
    click_in(GUI_MONITOR, 20, HEAD_Y);                  /* by pid again */
    CHECK(g.monitor.sort == GUI_MON_PID && order_is(by_pid, 5));
    /* the selection follows its pid from one reading to the next */
    click_in(GUI_MONITOR, 100, LIST_Y + 4 * 18 + 5);    /* edit, by the mouse */
    CHECK(selected_pid() == 9);
    memmove(&fake_tasks[1], &fake_tasks[0], fake_ntasks * sizeof(fake_tasks[0]));
    fake_ntasks++;
    fake_tasks[0].pid = 3; strlcpy(fake_tasks[0].name, "usb", sizeof(fake_tasks[0].name));
    fake_tasks[0].flags = 0; fake_tasks[0].state = GUI_TASK_BLOCKED; fake_tasks[0].tty = 0;
    add_task(12, GUI_TASK_RUNNING, 16, GUI_TASK_USER | GUI_TASK_SHELL, "surfsh");
    later(500);
    CHECK(g.monitor.ntasks == 5);                       /* not yet: about once a second */
    later(500);
    CHECK(g.monitor.ntasks == 7 && selected_pid() == 9 && g.monitor.selected == 5);
    /* a pid that is gone leaves the selection on its row */
    drop_task(9);
    later(1000);
    CHECK(g.monitor.ntasks == 6 && g.monitor.selected == 5 && selected_pid() == 12);
    gui_render(&g, &screen);
    CHECK(shows(960, g.win[GUI_MONITOR].x + 272 + 8, g.win[GUI_MONITOR].y + LIST_Y + 5 * 18 + 1, "pty 0", MUTED));
    /* the keys: Home, End, Page Up and Down */
    key(GUI_HOME); CHECK(g.monitor.selected == 0);
    key(GUI_END); CHECK(g.monitor.selected == 5);
    key(GUI_PGUP); CHECK(g.monitor.selected == 0);
    key(GUI_PGDN); CHECK(g.monitor.selected == 5);
    /* the wheel and the scroll bar, with more tasks than fit */
    while(fake_ntasks < GUI_TASK_MAX) add_task(100 + fake_ntasks, GUI_TASK_SLEEPING, 1, GUI_TASK_USER, "cat");
    later(1000);
    CHECK(g.monitor.ntasks == GUI_TASK_MAX && g.monitor.top == 0);
    g.x = g.win[GUI_MONITOR].x + 100; g.y = g.win[GUI_MONITOR].y + 120;
    {
        struct pointer_event e;
        memset(&e, 0, sizeof(e));
        e.x = g.x; e.y = g.y; e.wheel = 1;              /* away from the user: up */
        gui_pointer(&g, &e);
        CHECK(g.monitor.top == 0);
        e.wheel = -2;
        gui_pointer(&g, &e);
        CHECK(g.monitor.top == 6);
    }
    key(GUI_END);
    CHECK(g.monitor.selected == GUI_TASK_MAX - 1 && g.monitor.top > 6);
    click_in(GUI_MONITOR, g.win[GUI_MONITOR].w - 8, LIST_Y + 4);   /* above the thumb: a page up */
    CHECK(g.monitor.top < GUI_TASK_MAX - 10);
}

static void monitor_end(void) {
    struct fb_surface screen;
    desktop(960, 540);
    gui_open(&g, GUI_MONITOR);
    /* Go to pid (Ctrl+L) */
    key(12);
    CHECK(g.dialog.kind == GUI_DLG_INPUT);
    fake_type(&g, "9\r");
    CHECK(!g.dialog.kind && selected_pid() == 9);
    /* End task asks first, Cancel the default: Enter, Esc and Cancel end nothing */
    key(GUI_DELETE);
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM && g.dialog.act == gui_app_act(GUI_MONITOR, 1) && g.dialog.choice == 2);
    CHECK(!strcmp(g.dialog.text[0], "End edit (pid 9) on F2?"));
    fake_screen(&screen, 960, 540);
    gui_render(&g, &screen);
    CHECK(fake_guards(960, 540));
    key('\r');
    CHECK(!g.dialog.kind && fake_killed == 0 && row_of(9) >= 0);
    key(GUI_DELETE);
    key(27);
    CHECK(!g.dialog.kind && fake_killed == 0);
    /* the End task button, then the question's own button */
    click_in(GUI_MONITOR, 230 + 39, TOOL_Y);
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM);
    key(9);                                             /* Tab: to End task */
    CHECK(g.dialog.choice == 0);
    key('\r');
    CHECK(!g.dialog.kind && fake_killed == 9 && row_of(9) < 0);
    CHECK(!strcmp(g.monitor.status, "ended edit (pid 9)"));
    CHECK(g.monitor.selected >= 0);
    /* kill's refusal goes to the status bar (the fake's: init, pid 1, a program here) */
    fake_killed = 0;
    key(GUI_HOME);
    key(GUI_DOWN);
    CHECK(selected_pid() == 1);
    key(GUI_DELETE);
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM);
    key(GUI_LEFT);                                      /* Left also reaches End task (there is no No) */
    CHECK(g.dialog.choice == 0);
    key(' ');
    CHECK(!g.dialog.kind && fake_killed == 1 && row_of(1) >= 0 && !strcmp(g.monitor.status, "not one to end"));
    /* a kernel thread: End task is off, and says why without asking */
    fake_killed = 0;
    key(GUI_DOWN);
    CHECK(selected_pid() == 2);
    key(GUI_DELETE);
    CHECK(!g.dialog.kind && fake_killed == 0 &&
          !strcmp(g.monitor.status, "net (pid 2) is a kernel thread: End task ends programs"));
    click_in(GUI_MONITOR, 230 + 39, TOOL_Y);
    CHECK(!g.dialog.kind && fake_killed == 0);
    /* a console's shell: the question says init starts a new one */
    key(GUI_DOWN);
    key(GUI_DELETE);
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM && !strcmp(g.dialog.text[0], "End surfsh (pid 5) on F1?") &&
          !strcmp(g.dialog.text[1], "A console's shell: init starts a new one there."));
    key(27);
    /* a task that ended while the question was open */
    key(GUI_DELETE);
    drop_task(5);
    key(9);
    key('\r');
    CHECK(fake_killed == 5 && !strcmp(g.monitor.status, "no task with pid 5 any more"));
    /* End task is greyed on the other pages: the tab is not a task's */
    key(GUI_RIGHT);
    CHECK(g.monitor.page == GUI_MON_MEMORY);
    key(GUI_DELETE);
    CHECK(!g.dialog.kind);
}

static void monitor_pages(void) {
    struct fb_surface screen;
    struct gui_window *w;
    desktop(960, 540);
    gui_open(&g, GUI_MONITOR);
    w = &g.win[GUI_MONITOR];
    fake_screen(&screen, 960, 540);
    /* the tabs, by the mouse and by the keys */
    click_in(GUI_MONITOR, 8 + 90 + 30, TOOL_Y);
    CHECK(g.monitor.page == GUI_MON_MEMORY);
    click_in(GUI_MONITOR, 8 + 90 + 66 + 30, TOOL_Y);
    CHECK(g.monitor.page == GUI_MON_SYSTEM);
    key(GUI_RIGHT);
    CHECK(g.monitor.page == GUI_MON_PROCESSES);
    key(GUI_LEFT);
    CHECK(g.monitor.page == GUI_MON_SYSTEM);
    key(GUI_LEFT);
    CHECK(g.monitor.page == GUI_MON_MEMORY);
    /* Memory: three meters; Ctrl+C copies the page */
    key(3);
    CHECK(!g.quit && clip_is("Physical memory: 1.9 GB used, 5.7 GB free, of 7.6 GB in pages of 4 KB (RAM 8.0 GB)\n"
                             "Kernel heap: 3.0 MB used, 1.0 MB free (at most 64 MB)\n"
                             "DMA heap: 1.0 MB used, 3.0 MB free (at most 4.0 MB)\n"));
    gui_render(&g, &screen);
    CHECK(shows(960, w->x + 16, w->y + TITLE_H + BAR_H + 3 + 10, "Physical memory", TEAL));
    CHECK(fake_pixel(960, w->x + 16 + 2, w->y + TITLE_H + BAR_H + 3 + 10 + 22 + 8) == TEAL);       /* 25% used */
    CHECK(fake_pixel(960, w->x + w->w - 16 - 4, w->y + TITLE_H + BAR_H + 3 + 10 + 22 + 8) == 0xFFFDF5);
    CHECK(fake_pixel(960, w->x + w->w - 16 - 4, w->y + TITLE_H + BAR_H + 3 + 10 + 72 + 22 + 8) == 0xFFFDF5);   /* heap: 5% */
    /* nearly full: coral */
    fake_mem.dma_used = (4 << 20) - 100000;
    later(1000);
    gui_render(&g, &screen);
    CHECK(fake_pixel(960, w->x + 16 + 2, w->y + TITLE_H + BAR_H + 3 + 10 + 144 + 22 + 8) == CORAL);
    /* above 4 GB */
    fake_mem.high_kb = 4 * 1024 * 1024;
    later(1000);
    key(3);
    CHECK(clip_has("(RAM 8.0 GB (4.0 GB above 4 GB unused))"));
    /* System: the machine, the uptime as h:mm:ss, read again each second */
    key(GUI_RIGHT);
    key(3);
    CHECK(clip_is("Processor: Intel(R) Core(TM) i3-1005G1 CPU @ 1.20GHz\nFirmware: UEFI\nProfile: laptop\n"
                  "Display: 1920x1080x32 (the desktop: 960x540)\nBuild: 0efbade 2026-10-17 21:00\nUptime: 1:02:05\n"
                  "Tasks: 5\nTimers: 3\n"));
    fake_machine.uptime_s = 3726;
    later(1000);
    key(3);
    CHECK(clip_has("Uptime: 1:02:06\n"));
    gui_render(&g, &screen);
    CHECK(shows(960, w->x + 16, w->y + TITLE_H + BAR_H + 3 + 10 + 24, "Firmware", MUTED));
    CHECK(shows(960, w->x + 16 + 104, w->y + TITLE_H + BAR_H + 3 + 10 + 24, "UEFI", INK));
    CHECK(shows(960, w->x + 16 + 104, w->y + TITLE_H + BAR_H + 3 + 10 + 5 * 24, "1:02:06", INK));
    /* tucked away it waits; back, it reads at once */
    fake_machine.uptime_s = 4000;
    gui_act(&g, ACT_MINIMIZE);
    later(3000);
    CHECK(g.monitor.machine.uptime_s == 3726);
    gui_open(&g, GUI_MONITOR);
    later(1);
    CHECK(g.monitor.machine.uptime_s == 4000);
    /* no build id */
    fake_machine.build[0] = 0;
    later(1000);
    key(3);
    CHECK(clip_has("Build: none recorded"));
    /* the Edit menu is the monitor's: Copy, Find, Find next, Go to pid; the rest off */
    key(GUI_RIGHT);
    CHECK(gui_menu_on(&g, 3, ACT_COPY) && gui_menu_on(&g, 3, ACT_FIND) && gui_menu_on(&g, 3, ACT_GOTO));
    CHECK(!gui_menu_on(&g, 3, ACT_UNDO) && !gui_menu_on(&g, 3, ACT_PASTE) && !gui_menu_on(&g, 3, ACT_FIND_NEXT));
    key(6);                                             /* Ctrl+F */
    CHECK(g.dialog.kind == GUI_DLG_INPUT);
    fake_type(&g, "SUR\r");
    CHECK(selected_pid() == 5 && !strcmp(g.monitor.status, "Found surfsh (pid 5)"));
    CHECK(gui_menu_on(&g, 3, ACT_FIND_NEXT));
    gui_act(&g, ACT_FIND_NEXT);
    CHECK(selected_pid() == 5);
    key(3);
    CHECK(clip_is("    5  surfsh           running   F1       program"));
    key(6);
    fake_type(&g, "xyz\r");
    CHECK(selected_pid() == 5 && !strcmp(g.monitor.status, "No task's name has \"xyz\""));
    key(12);
    fake_type(&g, "77\r");
    CHECK(selected_pid() == 5 && !strcmp(g.monitor.status, "No task has pid 77"));
    /* Esc closes the window; the desktop's shortcuts stay the desktop's */
    key(5);                                             /* Ctrl+E: Notepad */
    CHECK(g.focus == GUI_NOTEPAD);
    gui_open(&g, GUI_MONITOR);
    key(27);
    CHECK(!g.win[GUI_MONITOR].open);
}

/**** Disks ****/

static void disks_tree(void) {
    struct fb_surface screen;
    struct gui_window *w;
    desktop(960, 540);
    gui_open(&g, GUI_DISKS);
    w = &g.win[GUI_DISKS];
    CHECK(g.disks.count == 5 && g.disks.selected == 0);
    CHECK(disk_row("rd0") == 0 && disk_row("usb0") == 1 && disk_row("usb0p1") == 2 && disk_row("usb1") == 3 &&
          disk_row("usb1p1") == 4);
    fake_screen(&screen, 960, 540);
    gui_render(&g, &screen);
    CHECK(fake_guards(960, 540));
    /* the partitions under their disks, a cell further in; lsblk's columns */
    CHECK(shows(960, w->x + 28, w->y + LIST_Y + 18 + 1, "usb0", INK));
    CHECK(shows(960, w->x + 28 + 16, w->y + LIST_Y + 2 * 18 + 1, "usb0p1", INK));
    CHECK(shows(960, w->x + 28 + 16, w->y + LIST_Y + 4 * 18 + 1, "usb1p1", INK));
    /* name 8 cells (usb0p1 two in), size 4: 28 + 64 + 8 + 32 = 132; bus at 140; type at 172 */
    CHECK(shows(960, w->x + 132 - 32, w->y + LIST_Y + 2 * 18 + 1, "254M", MUTED));
    CHECK(shows(960, w->x + 140, w->y + LIST_Y + 2 * 18 + 1, "usb", MUTED));
    CHECK(shows(960, w->x + 172, w->y + LIST_Y + 2 * 18 + 1, "part gpt EFI system", MUTED));
    CHECK(shows(960, w->x + 172, w->y + LIST_Y + 4 * 18 + 1, "part mbr FAT32 LBA", MUTED));
    CHECK(shows(960, w->x + 566 - 6 - 21 * 8, w->y + LIST_Y + 18 + 1, "boot drive: protected", MUTED));
    CHECK(shows(960, w->x + 566 - 6 - 21 * 8, w->y + LIST_Y + 1, "mounted", TEAL));
    CHECK(shows(960, w->x + 332, w->y + LIST_Y + 18 + 1, "QEMU \xF9", MUTED));    /* the model gives way: 6 cells */
    /* maximized, the model has room: the status follows it, as in lsblk */
    gui_act(&g, ACT_MAXIMIZE);
    gui_render(&g, &screen);
    CHECK(shows(960, w->x + 332, w->y + LIST_Y + 18 + 1, "QEMU QEMU HARDDISK", MUTED));
    CHECK(shows(960, w->x + 332 + 18 * 8 + 8, w->y + LIST_Y + 18 + 1, "boot drive: protected", MUTED));
    gui_act(&g, ACT_MAXIMIZE);
    gui_render(&g, &screen);
    /* the details: the initrd is mounted, with no sizes */
    CHECK(shows(960, w->x + 10, w->y + w->h - STATUS_H - 1 - 66 + 8, "rd0", TEAL));
    CHECK(shows(960, w->x + 10, w->y + w->h - STATUS_H - 1 - 66 + 26, "Mounted on /initrd: tarfs, read-only", INK));
    /* the selection follows the device's name; a new stick appears at the next reading (about every 2 s) */
    key(GUI_END);
    CHECK(g.disks.selected == 4 && !strcmp(g.disks.name, "usb1p1"));
    {                                                   /* two sticks more, listed before usb0 */
        struct gui_disk old[5];
        memcpy(old, fake_disks, sizeof(old));
        fake_ndisks = 1;
        add_disk("usb2", "usb2", "usb", "disk", "USB Disk 2.0", "", "29G", 30408704, false);
        add_disk("usb3", "usb3", "usb", "disk", "USB Disk 2.0", "", "29G", 30408704, false);
        memcpy(&fake_disks[3], &old[1], 4 * sizeof(old[0]));
        fake_ndisks = 7;
    }
    later(1000);
    CHECK(g.disks.count == 5);
    later(1000);
    CHECK(g.disks.count == 7 && g.disks.selected == 6 && disk_row("usb2") == 1 && disk_row("usb1p1") == 6);
    /* Ctrl+C: its line */
    key(3);
    CHECK(!g.quit && clip_is("usb1p1  63M  usb  part mbr FAT32 LBA"));
    CHECK(gui_menu_on(&g, 3, ACT_COPY) && !gui_menu_on(&g, 3, ACT_FIND) && !gui_menu_on(&g, 3, ACT_GOTO));
    /* the mouse: a row, the wheel */
    click_in(GUI_DISKS, 60, LIST_Y + 18 + 5);
    CHECK(g.disks.selected == 1 && !strcmp(g.disks.name, "usb2"));
    gui_render(&g, &screen);
    CHECK(shows(960, w->x + 10, w->y + w->h - STATUS_H - 1 - 66 + 8, "usb2", TEAL));
    CHECK(shows(960, w->x + 10 + 32, w->y + w->h - STATUS_H - 1 - 66 + 8, ": USB disk, 29G, removable, USB Disk 2.0", INK));
    CHECK(shows(960, w->x + 10, w->y + w->h - STATUS_H - 1 - 66 + 26, "No partition table", INK));
}

static void disks_mount_eject(void) {
    struct fb_surface screen;
    struct gui_window *w;
    char path[GUI_PATH_MAX];
    int detail_y;
    desktop(960, 540);
    gui_open(&g, GUI_DISKS);
    w = &g.win[GUI_DISKS];
    detail_y = w->h - STATUS_H - 1 - 66;
    fake_screen(&screen, 960, 540);
    /* Mount (M) the data stick's partition: the shell's mount, its line in the status bar */
    select_disk("usb1p1");
    key('m');
    CHECK(!strcmp(mounted_dev, "usb1p1") && g.disks.disk[disk_row("usb1p1")].mounted);
    CHECK(!strcmp(g.disks.status, "usb1p1 on /usb1p1: fat32, label APPSTICK, 62 MB, 56 MB free, read-write"));
    CHECK(!strcmp(g.disks.name, "usb1p1"));
    gui_render(&g, &screen);
    CHECK(shows(960, w->x + 10, w->y + detail_y + 26, "Mounted on /usb1p1: fat32, read-write, volume APPSTICK", INK));
    CHECK(fake_pixel(960, w->x + 10 + 3, w->y + detail_y + 46 + 7) == TEAL);          /* the bar: 9% used */
    CHECK(fake_pixel(960, w->x + 10 + (w->w - 20) / 3 - 4, w->y + detail_y + 46 + 7) == 0xFFFDF5);
    CHECK(shows(960, w->x + 10 + (w->w - 20) / 3 + 10, w->y + detail_y + 45, "6.0 MB used, 55 MB free, of 62 MB (9%)",
                MUTED));
    /* Mount again: it is mounted already */
    mounted_dev[0] = 0;
    key('m');
    CHECK(!mounted_dev[0] && !strcmp(g.disks.status, "usb1p1 is mounted already, on /usb1p1"));
    /* Enter on a mounted volume: Files shows it */
    key('\r');
    CHECK(g.focus == GUI_FILES && files_path(&g, g.files.selected, path, sizeof(path)) && !strcmp(path, "/usb1p1"));
    gui_open(&g, GUI_DISKS);
    /* Eject (E) from the disk's row: every volume of it */
    select_disk("usb1");
    CHECK(g.disks.selected == disk_row("usb1"));
    /* not while Notepad has unsaved changes to a file there */
    g.note.modified = true;
    strlcpy(g.note.path, "/usb1p1/notes.txt", sizeof(g.note.path));
    key('e');
    CHECK(!ejected_dev[0] && !strcmp(g.disks.status, "Not ejected: Notepad has unsaved changes to a file on usb1"));
    g.note.modified = false;
    key('e');
    CHECK(!strcmp(ejected_dev, "usb1") && !strcmp(g.disks.status, "usb1: safe to remove"));
    CHECK(!g.disks.disk[disk_row("usb1p1")].mounted && g.win[GUI_FILES].open);
    /* Eject with nothing mounted: nothing to do */
    ejected_dev[0] = 0;
    key('e');
    CHECK(!ejected_dev[0] && !strcmp(g.disks.status, "Nothing of usb1 is mounted"));
    /* Mount on a disk's row mounts its partition; the toolbar's buttons */
    click_in(GUI_DISKS, 8 + 27, TOOL_Y);                /* Mount */
    CHECK(!strcmp(mounted_dev, "usb1p1") && !strcmp(g.disks.name, "usb1p1"));
    click_in(GUI_DISKS, 8 + 58 + 27, TOOL_Y);           /* Eject */
    CHECK(!strcmp(ejected_dev, "usb1") && !g.disks.disk[disk_row("usb1p1")].mounted);
    /* a double-click mounts */
    mounted_dev[0] = 0;
    click_in(GUI_DISKS, 60, LIST_Y + disk_row("usb1p1") * 18 + 5);
    g.now += 100;
    click_in(GUI_DISKS, 60, LIST_Y + disk_row("usb1p1") * 18 + 5);
    CHECK(!strcmp(mounted_dev, "usb1p1"));
    key('e');
    /* a mount that fails: the shell's words */
    mount_fails = 1;
    key('m');
    CHECK(!strcmp(g.disks.status, "mount: usb1p1: no file system found (mkfs makes one)") &&
          !g.disks.disk[disk_row("usb1p1")].mounted);
    mount_fails = 0;
    /* the boot stick's volume: Mount is the shell's (which mounts it read-only); the initrd is no USB volume */
    mounted_dev[0] = ejected_dev[0] = 0;
    select_disk("rd0");
    key('m');
    CHECK(!mounted_dev[0] && !strcmp(g.disks.status, "rd0 is not on a USB drive: Disks mounts USB volumes"));
    key('e');
    CHECK(!ejected_dev[0] && !strcmp(g.disks.status, "rd0 is not on a USB drive: Disks ejects USB sticks"));
    select_disk("usb0p1");
    gui_render(&g, &screen);
    CHECK(shows(960, w->x + 10, w->y + detail_y + 26, "Named SURFOSUSB; boot drive: protected", INK));
    CHECK(shows(960, w->x + 10, w->y + detail_y + 45, "Not mounted: Mount (or Enter) opens it read-only at /usb0p1", MUTED));
    key('m');
    CHECK(!strcmp(mounted_dev, "usb0p1"));
    /* Ctrl+R reads the devices again */
    fake_disk("usb0p1")->mounted = false;
    key(18);
    CHECK(!g.disks.disk[disk_row("usb0p1")].mounted && !g.disks.status[0]);
    /* Show in Files from the toolbar */
    select_disk("rd0");
    click_in(GUI_DISKS, 8 + 58 + 58 + 74 + 59, TOOL_Y);
    CHECK(g.focus == GUI_FILES);
    /* without the file systems, Mount and Eject are off and say so */
    gui_open(&g, GUI_DISKS);
    g.fs = NULL;
    select_disk("usb1p1");
    mounted_dev[0] = 0;
    key('m');
    CHECK(!mounted_dev[0] && !strcmp(g.disks.status, "Mount needs the file systems: none here"));
    gui_render(&g, &screen);
    CHECK(fake_guards(960, 540));
}

static void disks_gone(void) {
    struct gui_disk *d;
    desktop(960, 540);
    gui_open(&g, GUI_DISKS);
    select_disk("usb1p1");
    key('m');
    /* the stick is pulled out: it stays as unplugged while it is mounted, and Eject unmounts it */
    fake_ndisks = 3;
    d = add_disk("usb1", "usb1", "usb", "disk mbr", "QEMU QEMU HARDDISK", "unplugged", "64M", 65536, false);
    d->gone = true;
    d = add_disk("usb1p1", "usb1", "usb", "part mbr FAT32 LBA", "", "unplugged: eject it", "63M", 64512, true);
    d->gone = d->mounted = true;
    strlcpy(d->mount, "/usb1p1", sizeof(d->mount));
    later(2000);
    CHECK(g.disks.count == 5 && !strcmp(g.disks.name, "usb1p1") && g.disks.disk[g.disks.selected].gone);
    mounted_dev[0] = 0;
    key('m');
    CHECK(!mounted_dev[0] && !strcmp(g.disks.status, "usb1p1 was unplugged"));
    key('e');
    CHECK(!strcmp(ejected_dev, "usb1"));
    /* then it is gone from the list: the selection stays where it was */
    fake_ndisks = 3;
    later(2000);
    CHECK(g.disks.count == 3 && g.disks.selected == 2 && !strcmp(g.disks.name, "usb0p1"));
    CHECK(!strcmp(g.disks.status, "usb1p1 is not there any more"));
    /* every device gone */
    fake_ndisks = 0;
    later(2000);
    CHECK(g.disks.count == 0 && g.disks.selected == -1);
    key(GUI_DOWN);
    key('m');
    key('e');
    key(3);
    CHECK(g.disks.selected == -1 && !g.quit);
}

/* the desktop of tests/host/test_gui.c: no system and no files. The windows say so, and nothing breaks */
static void nothing(void) {
    struct fb_surface screen;
    int i;
    fake_desktop(&g, 960, 540);
    g.sys = NULL;
    g.fs = NULL;
    fake_screen(&screen, 960, 540);
    for(i = 0; i < IDS; i++) {
        gui_open(&g, ids[i]);
        later(5000);
        key(GUI_DOWN); key(GUI_DELETE); key('m'); key('e'); key('s'); key(18); key(3);
        CHECK(!g.dialog.kind && !g.quit);
        gui_render(&g, &screen);
        CHECK(fake_guards(960, 540));
        key(GUI_RIGHT);
        gui_render(&g, &screen);
        key(GUI_RIGHT);
        gui_render(&g, &screen);
        CHECK(fake_guards(960, 540));
    }
    gui_open(&g, GUI_MONITOR);
    gui_render(&g, &screen);
    CHECK(shows(960, g.win[GUI_MONITOR].x + 16, g.win[GUI_MONITOR].y + TITLE_H + BAR_H + 3 + 10,
                "The system cannot be read here.", MUTED));
    gui_open(&g, GUI_DISKS);
    gui_render(&g, &screen);
    CHECK(shows(960, g.win[GUI_DISKS].x + 16, g.win[GUI_DISKS].y + LIST_Y + 8, "The block devices cannot be read here.",
                MUTED));
}

int main(void) {
    windows(640, 400);
    windows(960, 540);
    windows(1280, 800);
    sizes();
    monitor_order();
    monitor_end();
    monitor_pages();
    disks_tree();
    disks_mount_eject();
    disks_gone();
    nothing();
    return fake_done("test_guisys");
}
