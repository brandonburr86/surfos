/* Host tests for System Monitor and Disks (track SYS of docs/superpowers/plans/2026-10-17-gui-suite.md), against the
   in-memory file system and system of tests/host/gui_fake.c. Built and run by `make unittest` (unittest-gui).
   10/17/26: the Processes page's order and its selection, End task's question, both answers and the refusals, the
   pages, the Edit menu, Disks' tree, Mount and Eject (the fake file system's, watched here), a disk that goes away,
   a desktop without a system or files, and every window inside guard pixels at three sizes.
   10/18/26 (the review): the kernel's side itself (tests/host/guisys_host.c builds shell/guisys.c and guisys_disk.c
   with stand-ins): kill's refusals and words, the tasks' copy, Disks' rows (unplugged ones, their order, the
   32-row cap, statfs failures); and readings that miss a row once or fail, the pages at the smallest height, the
   task count's "of", the meters' edges, ties when reversed, Find around the end, Notepad's file on a mount whose
   name starts as another's or on an unplugged stick, and Mount scrolling to what it mounted.
   10/18/26 (round 2): End task's kill that waits for a call, then a reading without the task; Find with the followed
   task gone; Disks' M, E and Enter with nothing selected; kill of a task that is ending already. */
#include "guisys_host.c"                /* first: the kernel's headers before the desktop's */
#include "gui_fake.h"
#include "../../shell/gui_sys.h"

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
    bool gone = false;
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
    for(i = 0; i < fake_ndisks; i++) {                 /* an unplugged stick leaves the list once nothing holds it */
        if(strcmp(fake_disks[i].disk, dev) || !fake_disks[i].gone) continue;
        memmove(&fake_disks[i], &fake_disks[i + 1], (fake_ndisks - i - 1) * sizeof(fake_disks[0]));
        fake_ndisks--;
        i--;
        gone = true;
    }
    if(gone) snprintf(msg, n, "%s: unmounted (it was unplugged already)", dev);
    else snprintf(msg, n, "%s: safe to remove", dev);
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
    /* 10/18/26: a pid that is not in a reading stays followed: nothing is selected meanwhile, the status bar says
       so, and Down goes on from its row */
    drop_task(9);
    later(1000);
    CHECK(g.monitor.ntasks == 6 && g.monitor.selected == -1 && g.monitor.pid == 9 && g.monitor.hint == 5);
    gui_render(&g, &screen);
    CHECK(shows(960, g.win[GUI_MONITOR].x + 272 + 8, g.win[GUI_MONITOR].y + LIST_Y + 5 * 18 + 1, "pty 0", MUTED));
    CHECK(shows(960, g.win[GUI_MONITOR].x + 10, g.win[GUI_MONITOR].y + 360 - STATUS_H - 1 + 3, "pid 9 is not in the list now",
                INK));
    key(GUI_DOWN);
    CHECK(g.monitor.selected == 5 && selected_pid() == 12);
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
    CHECK(!strcmp(g.dialog.text[0], "End edit (pid 9) on F2?") &&
          !strcmp(g.dialog.text[1], "Ends now or after a system call; unsaved work is lost."));
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
    CHECK(!strcmp(g.monitor.status, "edit (pid 9) has ended"));      /* 10/18/26: the reading right after the kill */
    CHECK(g.monitor.selected == -1 && g.monitor.pid == 9);   /* 10/18/26: still followed, not listed */
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
          !strcmp(g.dialog.text[1], "Ends now or after a system call; init starts another."));
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
    struct fb_surface screen;
    struct gui_disk *d;
    desktop(960, 540);
    fake_screen(&screen, 960, 540);
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
    key('e');                                           /* then it is gone from the list: eject's words stay */
    CHECK(!strcmp(ejected_dev, "usb1") && g.disks.count == 3 && g.disks.selected == -1 && !strcmp(g.disks.name, "usb1p1"));
    CHECK(!strcmp(g.disks.status, "usb1: unmounted (it was unplugged already)"));
    /* plugged in again: still followed, so selected again */
    add_disk("usb1", "usb1", "usb", "disk mbr", "QEMU QEMU HARDDISK", "", "64M", 65536, false);
    add_disk("usb1p1", "usb1", "usb", "part mbr FAT32 LBA", "", "", "63M", 64512, true);
    later(2000);
    CHECK(g.disks.count == 5 && g.disks.selected == 4);
    /* pulled out (not mounted): it leaves the list at the next reading; still followed, nothing selected, and the
       status bar says it is not there; Down goes on from its row */
    g.disks.status[0] = 0;
    fake_ndisks = 3;
    later(2000);
    CHECK(g.disks.count == 3 && g.disks.selected == -1 && !strcmp(g.disks.name, "usb1p1") && !g.disks.status[0]);
    gui_render(&g, &screen);
    CHECK(shows(960, g.win[GUI_DISKS].x + 10, g.win[GUI_DISKS].y + 320 - STATUS_H - 1 + 3, "usb1p1 is not there any more",
                INK));
    key(GUI_DOWN);
    CHECK(g.disks.selected == 2 && !strcmp(g.disks.name, "usb0p1"));
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

/**** 10/18/26: the kernel's side (tests/host/guisys_host.c builds shell/guisys.c and guisys_disk.c) ****/

/* kill's refusals and its words: idle, init, the desktop's own task and the shell above it, a kernel thread; a
   zombie; a program in its own code, in a system call, in a kernel command; a pid that is not there. 10/25/26 (L15):
   the rule is kill's (task_user_may_kill(), surfos/task.h): a process carries TF_PROCESS (and TF_KTHREAD) as the
   kernel's do, and one still being loaded, without TF_USER yet, may be ended too, as may a ring-0 shell; a task
   with neither TF_PROCESS nor TF_SHELL may not, whatever its other flags */
static void kernel_kill(void) {
    const struct gui_sys *sys = gui_kernel_sys();
    const u_int proc = TF_KTHREAD | TF_PROCESS | TF_USER;
    surf_task *init, *f1, *f2, *t;
    char msg[GUI_STATUS];
    nktasks = patient_kills = 0;
    patient_last = NULL;
    idle_task = ktask_add(0, "Kernel Idle Task", 0, NULL, 0);
    init_task_ptr = init = ktask_add(1, "init", TF_KTHREAD, NULL, 0);
    ktask_add(2, "usb", TF_KTHREAD | TF_DETACHED, init, 0);
    ktask_add(3, "odd", TF_KTHREAD | TF_DETACHED | TF_USER | TF_FOREGROUND | TF_LOADED, init, 0);  /* neither of the two */
    f1 = ktask_add(4, "surfsh", proc | TF_SHELL, init, TC_SYSCALL);            /* F1's shell, which ran another */
    f2 = ktask_add(5, "surfsh", proc | TF_SHELL, init, TC_SYSCALL | TC_COMMAND);   /* F2's, in a kernel command */
    ktask_add(6, "Shell 2", TF_KTHREAD | TF_SHELL, init, 0);                   /* a ring-0 shell, at its prompt */
    ktask_add(9, "edit", proc, f2, TC_SYSCALL);
    ktask_add(10, "cat", proc, f2, 0);
    ktask_add(11, "ls", proc, f2, 0)->state = TS_DEAD;
    curTask = ktask_add(12, "surfsh", proc, f1, TC_SYSCALL | TC_COMMAND);     /* the shell that ran gui */
    ktask_add(13, "more", proc, f2, TC_EXIT);                                 /* in task_exit(): its files close */
    ktask_add(14, "hello", TF_KTHREAD | TF_PROCESS, f2, TC_SYSCALL);           /* being loaded: no TF_USER yet */
    CHECK(sys->kill(sys->ctx, 0, msg, sizeof(msg)) == -EACCES &&
          !strcmp(msg, "Kernel Idle Task (pid 0) is the idle task: the kernel's own"));
    CHECK(sys->kill(sys->ctx, 1, msg, sizeof(msg)) == -EACCES &&
          !strcmp(msg, "init (pid 1) starts the consoles' shells: it is never ended"));
    CHECK(sys->kill(sys->ctx, 12, msg, sizeof(msg)) == -EACCES &&
          !strcmp(msg, "surfsh (pid 12) runs this desktop: Ctrl+Q leaves it"));
    CHECK(sys->kill(sys->ctx, 4, msg, sizeof(msg)) == -EACCES &&
          !strcmp(msg, "surfsh (pid 4) runs the shell this desktop runs in"));
    CHECK(sys->kill(sys->ctx, 2, msg, sizeof(msg)) == -EACCES &&
          !strcmp(msg, "usb (pid 2) is a kernel thread: End task ends programs"));
    CHECK(sys->kill(sys->ctx, 3, msg, sizeof(msg)) == -EACCES &&
          !strcmp(msg, "odd (pid 3) is a kernel thread: End task ends programs"));      /* TF_USER is not the rule */
    CHECK(patient_kills == 0);
    CHECK(sys->kill(sys->ctx, 11, msg, sizeof(msg)) == 0 && !strcmp(msg, "ls (pid 11) had ended already") &&
          patient_kills == 0);
    CHECK(sys->kill(sys->ctx, 10, msg, sizeof(msg)) == 0 && !strcmp(msg, "ended cat (pid 10)") && patient_kills == 1 &&
          patient_last->pid == 10 && patient_last->state == TS_DEAD);
    CHECK(sys->kill(sys->ctx, 9, msg, sizeof(msg)) == 0 && !strcmp(msg, "edit (pid 9) ends when its system call does"));
    t = task_find(9);
    CHECK(patient_kills == 2 && patient_last == t && (t->intr & (TI_KILL | TI_PATIENT)) == (TI_KILL | TI_PATIENT));
    CHECK(sys->kill(sys->ctx, 5, msg, sizeof(msg)) == 0 && !strcmp(msg, "surfsh (pid 5) ends when its kernel command does"));
    CHECK(sys->kill(sys->ctx, 77, msg, sizeof(msg)) == -ENOENT && !strcmp(msg, "no task with pid 77 any more"));
    CHECK(patient_kills == 3);
    CHECK(sys->kill(sys->ctx, 13, msg, sizeof(msg)) == 0 && !strcmp(msg, "more (pid 13) is ending already") &&
          patient_kills == 3 && !task_find(13)->intr);
    /* 10/25/26 (L15): a process still being loaded ends when its loading does; a ring-0 shell elsewhere at once */
    CHECK(sys->kill(sys->ctx, 14, msg, sizeof(msg)) == 0 && !strcmp(msg, "hello (pid 14) ends when its system call does") &&
          patient_kills == 4 && patient_last->pid == 14);
    CHECK(sys->kill(sys->ctx, 6, msg, sizeof(msg)) == 0 && !strcmp(msg, "ended Shell 2 (pid 6)") && patient_kills == 5 &&
          patient_last->pid == 6 && patient_last->state == TS_DEAD);
    /* killed already (the shell's kill, say): that kill's rule stands, and the words are the call's */
    t = task_find(9);
    t->intr = TI_KILL;
    CHECK(sys->kill(sys->ctx, 9, msg, sizeof(msg)) == 0 && !strcmp(msg, "edit (pid 9) ends when its system call does") &&
          patient_kills == 6 && t->intr == TI_KILL);
    curTask = idle_task = init_task_ptr = NULL;
}

/* the tasks as struct gui_task, and a reading that cannot get its memory: it fails and leaves the rows alone */
static void kernel_tasks(void) {
    const struct gui_sys *sys = gui_kernel_sys();
    static struct gui_task out[GUI_TASK_MAX];
    surf_task *init;
    nktasks = 0;
    ktask_add(0, "Kernel Idle Task", 0, NULL, 0);
    init = ktask_add(1, "init", 0, NULL, 0);
    ktask_add(7, "surfsh", TF_USER, init, 0);
    CHECK(sys->tasks(sys->ctx, out, GUI_TASK_MAX) == 3);
    CHECK(out[2].pid == 7 && out[2].ppid == 1 && out[2].flags == GUI_TASK_USER && !strcmp(out[2].name, "surfsh") &&
          out[0].ppid == -1 && !strcmp(out[0].name, "Kernel Idle Tas"));
    out[0].pid = 1234;
    kalloc_fails = 1;
    CHECK(sys->tasks(sys->ctx, out, GUI_TASK_MAX) == -ENOMEM && out[0].pid == 1234);
}

static struct bdev kd[KDEVS];

/* Disks' rows: lsblk's, then the unplugged ones (each disk, then its mounted volumes) in the order of the mounts; how
   full a mounted one is, a statfs that fails, the 32-row cap (which keeps the rows that only Eject can end), and a
   reading that cannot get its memory */
static void kernel_disks(void) {
    const struct gui_sys *sys = gui_kernel_sys();
    static struct gui_disk out[GUI_DISK_MAX];
    static const char *const order[] = { "rd0", "usb0", "usb0p1", "usb1", "usb1p1", "usb3", "usb2", "usb2p1" };
    struct bdev *rd0, *usb0, *usb1, *usb1p1, *usb2;
    char name[16];
    int n, i;
    nkdevs = nkviews = nkstats = 0;
    rd0 = kdev_add(&kd[0], "rd0", 14940, NULL, BDEV_BUS_RAM, true);
    strlcpy(rd0->model, "initrd.tar", sizeof(rd0->model));
    usb0 = kdev_add(&kd[1], "usb0", 524288, NULL, BDEV_BUS_USB, true);
    usb0->protect = "the SurfOS boot drive";
    kdev_add(&kd[2], "usb0p1", 520192, usb0, 0, true)->protect = usb0->protect;
    usb1 = kdev_add(&kd[3], "usb1", 131072, NULL, BDEV_BUS_USB, true);
    usb1p1 = kdev_add(&kd[4], "usb1p1", 129024, usb1, 0, true);
    usb2 = kdev_add(&kd[5], "usb2", 60817408, NULL, BDEV_BUS_USB, false);   /* unplugged while usb2p1 was mounted */
    kdev_add(&kd[6], "usb2p1", 60815360, usb2, 0, false);
    kdev_add(&kd[7], "usb3", 131072, NULL, BDEV_BUS_USB, false);           /* a volume without a table, unplugged */
    kmount("/initrd", "tarfs", rd0, true);
    kmount("/usb1p1", "fat", usb1p1, false);
    kmount("/usb3", "fat", &kd[7], false);
    kmount("/usb2p1", "fat", &kd[6], false);
    kstatfs("/usb1p1", 0, 126992, 118800, "fat32", "APPSTICK");
    n = sys->disks(sys->ctx, out, GUI_DISK_MAX);
    CHECK(n == 8);
    for(i = 0; i < n && i < 8; i++) CHECK(!strcmp(out[i].name, order[i]));
    CHECK(out[0].mounted && out[0].readonly && !strcmp(out[0].mount, "/initrd") && !strcmp(out[0].fs, "tarfs") &&
          !out[0].used_kb && !out[0].free_kb && !out[0].statfs_err && !out[0].gone);
    CHECK(!strcmp(out[1].status, "boot drive: protected") && out[2].part && out[2].readonly && !strcmp(out[2].disk, "usb0"));
    CHECK(out[4].mounted && !out[4].readonly && out[4].used_kb == 4096 && out[4].free_kb == 59400 &&
          !strcmp(out[4].fs, "fat32") && !strcmp(out[4].volume, "APPSTICK") && !strcmp(out[4].status, "mounted"));
    CHECK(out[5].gone && out[5].mounted && !strcmp(out[5].status, "unplugged: eject it") && !strcmp(out[5].mount, "/usb3") &&
          !out[5].used_kb && !out[5].statfs_err);
    CHECK(out[6].gone && !out[6].mounted && !strcmp(out[6].status, "unplugged") && !out[6].part);
    CHECK(out[7].gone && out[7].mounted && out[7].part && !strcmp(out[7].status, "unplugged: eject it") &&
          !strcmp(out[7].disk, "usb2"));
    /* a statfs that fails: told, and no sizes */
    kstat[0].err = -EIO;
    n = sys->disks(sys->ctx, out, GUI_DISK_MAX);
    CHECK(n == 8 && out[4].mounted && out[4].statfs_err == -EIO && !out[4].used_kb && !out[4].free_kb &&
          !strcmp(out[4].fs, "fat") && !out[4].volume[0]);
    /* more devices than rows: lsblk's are cut, the three that only Eject can end are kept, last */
    for(i = 8; i < KDEVS; i++) {
        snprintf(name, sizeof(name), "usb%d", i + 2);
        kdev_add(&kd[i], name, 131072, NULL, BDEV_BUS_USB, true);
    }
    n = sys->disks(sys->ctx, out, GUI_DISK_MAX);
    CHECK(n == GUI_DISK_MAX && !strcmp(out[0].name, "rd0") && !strcmp(out[GUI_DISK_MAX - 4].name, "usb33") &&
          !strcmp(out[GUI_DISK_MAX - 3].name, "usb3") && !strcmp(out[GUI_DISK_MAX - 2].name, "usb2") &&
          !strcmp(out[GUI_DISK_MAX - 1].name, "usb2p1") && out[GUI_DISK_MAX - 1].gone);
    /* no memory for the mounts' list: it fails and leaves the rows alone */
    out[0].size_kb = 4321;
    kalloc_fails = 1;
    CHECK(sys->disks(sys->ctx, out, GUI_DISK_MAX) == -ENOMEM && out[0].size_kb == 4321);
}

/**** 10/18/26: the review's cases, in the model ****/

/* System Monitor at its smallest height: the Memory and System pages scroll, so all of each can be read */
static void monitor_short(void) {
    struct fb_surface screen;
    struct pointer_event e;
    struct gui_window *w;
    int top, status, bar;
    desktop(960, 540);
    gui_open(&g, GUI_MONITOR);
    w = &g.win[GUI_MONITOR];
    w->h = 200;                                         /* as small as the corner grip makes it */
    gui_place(&g, GUI_MONITOR);
    fake_screen(&screen, 960, 540);
    top = w->y + TITLE_H + BAR_H + 3 + 10;
    status = w->y + w->h - STATUS_H - 1;
    bar = w->x + w->w - 2 - SCROLL_W;
    key(GUI_RIGHT);                                     /* Memory: nine lines of 24, four shown */
    gui_render(&g, &screen);
    CHECK(shows(960, w->x + 16, top, "Physical memory", TEAL) && shows(960, w->x + 16, top + 44, "1.9 GB used", INK));
    CHECK(fake_pixel(960, bar + 4, top + 4) == MUTED);  /* its scroll bar's thumb, at the top */
    key(GUI_END);                                       /* lines 5 to 8: the DMA heap, whole */
    gui_render(&g, &screen);
    CHECK(shows(960, w->x + 16, top + 24, "DMA heap", TEAL) && shows(960, w->x + 16, top + 68, "1.0 MB used, 3.0 MB free", INK));
    CHECK(top + 68 + 16 <= status && fake_guards(960, 540));
    key(GUI_UP);
    key(GUI_UP);                                        /* lines 3 to 6: the kernel heap, whole */
    gui_render(&g, &screen);
    CHECK(shows(960, w->x + 16, top, "Kernel heap", TEAL) && shows(960, w->x + 16, top + 44, "3.0 MB used, 1.0 MB free", INK));
    CHECK(fake_pixel(960, w->x + 20, w->y + TITLE_H + BAR_H) == BAND);   /* nothing drawn over the toolbar */
    key(GUI_HOME);
    CHECK(g.monitor.scroll[GUI_MON_MEMORY] == 0);
    key(GUI_RIGHT);                                     /* System: eight lines, four shown */
    gui_render(&g, &screen);
    CHECK(shows(960, w->x + 16, top, "Processor", MUTED));
    key(GUI_END);
    gui_render(&g, &screen);
    CHECK(shows(960, w->x + 16, top, "Build", MUTED) && shows(960, w->x + 16, top + 72, "Timers", MUTED) &&
          shows(960, w->x + 16 + 104, top + 72, "3", INK) && shows(960, w->x + 16 + 104, top + 24, "1:02:05", INK));
    CHECK(top + 72 + 16 <= status);
    memset(&e, 0, sizeof(e));                           /* the wheel, away from the user: three lines up */
    e.x = w->x + 100; e.y = w->y + 120; e.wheel = 1;
    gui_pointer(&g, &e);
    CHECK(g.monitor.scroll[GUI_MON_SYSTEM] == 1);
    click_in(GUI_MONITOR, w->w - 8, w->h - STATUS_H - 4);   /* the bar below its thumb: a page down */
    CHECK(g.monitor.scroll[GUI_MON_SYSTEM] == 4);
    w->h = 360;                                         /* taller again: all of it, nothing scrolled */
    gui_place(&g, GUI_MONITOR);
    gui_render(&g, &screen);
    CHECK(shows(960, w->x + 16, top, "Processor", MUTED) && shows(960, w->x + 16, top + 7 * 24, "Timers", MUTED));
}

/* the task count says "of" only when the list is cut at GUI_TASK_MAX (the newest) */
static void monitor_count(void) {
    struct fb_surface screen;
    struct gui_window *w;
    desktop(960, 540);
    fake_machine.tasks = 6;                             /* one started between tasks() and machine() */
    gui_open(&g, GUI_MONITOR);
    w = &g.win[GUI_MONITOR];
    fake_screen(&screen, 960, 540);
    gui_render(&g, &screen);
    CHECK(shows(960, w->x + w->w - 24 - 7 * 8, w->y + w->h - STATUS_H - 1 + 3, "5 tasks", MUTED));
    while(fake_ntasks < GUI_TASK_MAX) add_task(100 + fake_ntasks, GUI_TASK_SLEEPING, 1, GUI_TASK_USER, "cat");
    fake_machine.tasks = 140;
    later(1000);
    gui_render(&g, &screen);
    CHECK(shows(960, w->x + w->w - 24 - 21 * 8, w->y + w->h - STATUS_H - 1 + 3, "the newest 128 of 140", MUTED));
}

/* the readings: one that misses the selected task once, ones that fail; never read: nothing to copy */
static int fail_tasks, fail_memory, fail_machine, fail_disks;
static int t_tasks(void *ctx, struct gui_task *out, int max) {
    return fail_tasks ? -ENOMEM : fake_sys.tasks(ctx, out, max);
}
static int t_memory(void *ctx, struct gui_memory *out) {
    if(fail_memory) { memset(out, 0xEE, sizeof(*out)); return -EIO; }   /* what a failing call wrote is not used */
    return fake_sys.memory(ctx, out);
}
static int t_machine(void *ctx, struct gui_machine *out) {
    if(fail_machine) { memset(out, 0xEE, sizeof(*out)); return -EIO; }
    return fake_sys.machine(ctx, out);
}
static int t_disks(void *ctx, struct gui_disk *out, int max) {
    return fail_disks ? -ENOMEM : fake_sys.disks(ctx, out, max);
}
static struct gui_sys tsys;
static void failing(void) {
    desktop(960, 540);
    tsys = fake_sys;
    tsys.tasks = t_tasks; tsys.memory = t_memory; tsys.machine = t_machine; tsys.disks = t_disks;
    g.sys = &tsys;
    fail_tasks = fail_memory = fail_machine = fail_disks = 0;
}
static void readings(void) {
    struct fb_surface screen;
    struct gui_task kept;
    struct gui_window *w;
    failing();
    fake_screen(&screen, 960, 540);
    gui_open(&g, GUI_MONITOR);
    w = &g.win[GUI_MONITOR];
    key(GUI_HOME); key(GUI_DOWN); key(GUI_DOWN); key(GUI_DOWN);   /* surfsh, pid 5, row 3 */
    CHECK(selected_pid() == 5);
    kept = fake_tasks[3];                               /* a reading that misses it (a task collected meanwhile) */
    memmove(&fake_tasks[3], &fake_tasks[4], sizeof(fake_tasks[0]));
    fake_ntasks = 4;
    later(1000);
    CHECK(g.monitor.selected == -1 && g.monitor.pid == 5);
    key(GUI_DELETE);
    CHECK(!g.dialog.kind);                              /* nothing selected: no question about another task */
    memmove(&fake_tasks[4], &fake_tasks[3], sizeof(fake_tasks[0]));
    fake_tasks[3] = kept;
    fake_ntasks = 5;
    later(1000);
    CHECK(g.monitor.selected == 3 && selected_pid() == 5);
    key(GUI_DELETE);
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM && !strcmp(g.dialog.text[0], "End surfsh (pid 5) on F1?"));
    key(27);
    fail_tasks = 1;                                     /* a reading that fails: the rows stay, and the selection */
    later(1000);
    CHECK(g.monitor.err_tasks == -ENOMEM && g.monitor.ntasks == 5 && selected_pid() == 5);
    g.monitor.status[0] = 0;
    gui_render(&g, &screen);
    CHECK(shows(960, w->x + 10, w->y + w->h - STATUS_H - 1 + 3, "The tasks could not be read now", INK));
    fail_tasks = 0;
    later(1000);
    CHECK(!g.monitor.err_tasks && selected_pid() == 5);
    key(GUI_RIGHT);                                     /* Memory: a reading that fails keeps the last numbers */
    fail_memory = 1;
    later(1000);
    CHECK(g.monitor.err_memory == -EIO && g.monitor.have_memory && g.monitor.mem.heap_used == 3u << 20);
    key(3);
    CHECK(clip_has("Kernel heap: 3.0 MB used, 1.0 MB free"));
    gui_render(&g, &screen);
    CHECK(shows(960, w->x + 10, w->y + w->h - STATUS_H - 1 + 3, "Copied the Memory page", INK));
    g.monitor.status[0] = 0;
    gui_render(&g, &screen);
    CHECK(shows(960, w->x + 10, w->y + w->h - STATUS_H - 1 + 3, "Memory could not be read now", INK));
    /* never read: the pages say so, and Copy copies nothing */
    gui_close(&g, GUI_MONITOR);
    g.monitor.have_memory = g.monitor.have_machine = false;
    fail_machine = 1;
    gui_open(&g, GUI_MONITOR);
    g.clip_len = 0;
    key(3);
    CHECK(!g.clip_len && !strcmp(g.monitor.status, "Nothing copied: memory could not be read"));
    gui_render(&g, &screen);
    CHECK(shows(960, w->x + 16, w->y + TITLE_H + BAR_H + 3 + 10, "Memory could not be read.", MUTED));
    key(GUI_RIGHT);
    key(3);
    CHECK(!g.clip_len && !strcmp(g.monitor.status, "Nothing copied: the machine could not be read"));
    fail_memory = fail_machine = 0;
    /* Disks: a reading that fails keeps the rows and the selection; one that misses the device, then has it again */
    gui_open(&g, GUI_DISKS);
    key(GUI_END);
    CHECK(!strcmp(g.disks.name, "usb1p1") && g.disks.selected == 4);
    fail_disks = 1;
    later(2000);
    CHECK(g.disks.err == -ENOMEM && g.disks.count == 5 && g.disks.selected == 4 && !strcmp(g.disks.name, "usb1p1"));
    gui_render(&g, &screen);
    CHECK(shows(960, g.win[GUI_DISKS].x + 10, g.win[GUI_DISKS].y + 320 - STATUS_H - 1 + 3,
                "The devices could not be read now", INK));
    fail_disks = 0;
    later(2000);
    CHECK(!g.disks.err && g.disks.selected == 4);
    fake_ndisks = 4;                                    /* usb1p1 missed once */
    later(2000);
    CHECK(g.disks.count == 4 && g.disks.selected == -1 && !strcmp(g.disks.name, "usb1p1"));
    fake_ndisks = 5;
    later(2000);
    CHECK(g.disks.selected == 4 && !strcmp(g.disks.name, "usb1p1") && !g.disks.status[0]);
}

/* the meters: sizes and shares at their edges, and coral from exactly 90% */
static void meter_edges(void) {
    static const u32 kbs[] = { 0, 1023, 1024, 10239, 10240, 1048575, 1048576, 10485759, 1073741823, 1073741824,
                               0xFFFFFFFFu };
    static const char *const words[] = { "0 KB", "1023 KB", "1.0 MB", "9.9 MB", "10 MB", "1023 MB", "1.0 GB", "9.9 GB",
                                         "1023 GB", "1.0 TB", "3.9 TB" };
    struct fb_surface screen;
    struct gui_draw d;
    char b[32];
    u32 i, p;
    for(i = 0; i < sizeof(kbs) / sizeof(kbs[0]); i++) {
        sys_kb(kbs[i], b, sizeof(b));
        CHECK(!strcmp(b, words[i]));
    }
    CHECK(sys_part(9, 10, 100) == 90 && sys_part(90, 100, 100) == 90 && sys_part(0xFFFFFFFFu, 0xFFFFFFFFu, 1000) == 1000);
    CHECK(sys_part(5, 0, 100) == 0 && sys_part(7, 3, 100) == 100 && sys_part(0, 7, 100) == 0);
    p = sys_part(262143, 262144, 65535);                /* big totals lose a little, never past the scale */
    CHECK(p >= 65530 && p <= 65535);
    fake_screen(&screen, 960, 540);
    gd_init(&d, &screen, 960, 540);
    sys_bar(&d, 10, 10, 102, 10, 9, 10);                /* 90% exactly */
    CHECK(fake_pixel(960, 12, 14) == CORAL);
    sys_bar(&d, 10, 10, 102, 10, 899, 1000);
    CHECK(fake_pixel(960, 12, 14) == TEAL);
    sys_bar(&d, 10, 10, 102, 10, 235930, 262144);       /* a hair over 90% of a big total */
    CHECK(fake_pixel(960, 12, 14) == CORAL);
    sys_bar(&d, 10, 10, 102, 10, 235929, 262144);       /* a hair under */
    CHECK(fake_pixel(960, 12, 14) == TEAL);
    sys_bar(&d, 10, 10, 102, 10, 0, 0);                 /* nothing at all: no fill */
    CHECK(fake_pixel(960, 12, 14) == SYS_SHEET);
}

/* ties stay in pid order, also reversed; Find goes round past the end */
static void monitor_ties(void) {
    static const int by_name[] = { 9, 15, 20, 0, 1, 2, 5 }, reversed[] = { 5, 2, 1, 0, 9, 15, 20 };
    desktop(960, 540);
    add_task(20, GUI_TASK_SLEEPING, 1, GUI_TASK_USER, "edit");
    add_task(15, GUI_TASK_SLEEPING, 1, GUI_TASK_USER, "edit");
    gui_open(&g, GUI_MONITOR);
    key('s');
    CHECK(order_is(by_name, 7));
    key('r');
    CHECK(order_is(reversed, 7));
    key(GUI_END);                                       /* edit, pid 20, the last row */
    key(6);
    fake_type(&g, "ED\r");
    CHECK(selected_pid() == 9 && !strcmp(g.monitor.status, "Found edit (pid 9)"));
}

/* Eject and Notepad's unsaved file: on /usb1p10 it is not on /usb1p1's stick; on an unplugged stick's mount it is */
static void disks_guard(void) {
    struct gui_disk *d;
    desktop(960, 540);
    gui_open(&g, GUI_DISKS);
    select_disk("usb1p1");
    key('m');
    g.note.modified = true;
    strlcpy(g.note.path, "/usb1p10/notes.txt", sizeof(g.note.path));
    ejected_dev[0] = 0;
    key('e');
    CHECK(!strcmp(ejected_dev, "usb1") && !strcmp(g.disks.status, "usb1: safe to remove"));
    d = add_disk("usb2", "usb2", "usb", "disk mbr", "USB Disk 2.0", "unplugged", "29G", 30408704, false);
    d->gone = true;
    d = add_disk("usb2p1", "usb2", "usb", "part mbr FAT32 LBA", "", "unplugged: eject it", "29G", 30407680, true);
    d->gone = d->mounted = true;
    strlcpy(d->mount, "/usb2p1", sizeof(d->mount));
    later(2000);
    select_disk("usb2p1");
    strlcpy(g.note.path, "/usb2p1/a.txt", sizeof(g.note.path));
    ejected_dev[0] = 0;
    key('e');
    CHECK(!ejected_dev[0] && !strcmp(g.disks.status, "Not ejected: Notepad has unsaved changes to a file on usb2"));
    g.note.modified = false;
    g.note.path[0] = 0;
}

/* Mount from a disk's row selects the partition it mounted, and shows it, also when it was below the rows shown */
static void disks_mount_view(void) {
    char name[16];
    int i, rows;
    desktop(960, 540);
    fake_ndisks = 0;
    add_disk("rd0", "rd0", "ram", "disk", "initrd.tar", "", "7M", 7469, false);
    for(i = 2; i < 6; i++) {
        snprintf(name, sizeof(name), "usb%d", i);
        add_disk(name, name, "usb", "disk mbr", "USB Disk 2.0", "", "29G", 30408704, false);
        snprintf(name, sizeof(name), "usb%dp1", i);
        add_disk(name, fake_disks[fake_ndisks - 1].name, "usb", "part mbr FAT32 LBA", "", "", "29G", 30407680, true);
    }
    gui_open(&g, GUI_DISKS);
    g.win[GUI_DISKS].h = 200;
    gui_place(&g, GUI_DISKS);
    rows = (200 - STATUS_H - 1 - 66 - LIST_Y) / ROW_H;
    select_disk("usb3");
    CHECK(rows == 1 && g.disks.selected == 3 && g.disks.top == 3);
    key('m');
    CHECK(!strcmp(mounted_dev, "usb3p1") && g.disks.selected == 4 && g.disks.top <= 4 && 4 < g.disks.top + rows);
}

/**** 10/18/26 (round 2 of the review) ****/

/* a kill that waits for the call: the task stays in the list until the call ends */
static struct gui_sys waiting_sys;
static int waiting_kill(void *ctx, int pid, char *msg, size_t n) {
    (void)ctx;
    fake_killed = pid;
    snprintf(msg, n, "edit (pid %d) ends when its system call does", pid);
    return 0;
}
/* ... and one whose call ends, and whose task is collected, before the desktop reads the list again */
static int quick_kill(void *ctx, int pid, char *msg, size_t n) {
    waiting_kill(ctx, pid, msg, n);
    drop_task(pid);
    return 0;
}

/* End task's kill waits: the status bar says so until a reading is without the task, then that it has ended; not if
   the user has moved on, nor while the list is cut to the newest (the task may only be off it). A task gone by the
   reading right after the kill (ended at once, or its call ended meanwhile) has ended at once; a refusal ends nothing */
static void monitor_ending(void) {
    int pid;
    desktop(960, 540);
    waiting_sys = fake_sys;
    waiting_sys.kill = waiting_kill;
    g.sys = &waiting_sys;
    gui_open(&g, GUI_MONITOR);
    key(GUI_END);                                       /* edit, pid 9 */
    key(GUI_DELETE); key(9); key('\r');
    CHECK(fake_killed == 9 && selected_pid() == 9 && g.monitor.ending == 9 &&
          !strcmp(g.monitor.status, "edit (pid 9) ends when its system call does"));
    later(1000);                                        /* still in its call */
    CHECK(g.monitor.ending == 9 && !strcmp(g.monitor.status, "edit (pid 9) ends when its system call does"));
    drop_task(9);                                       /* the call ended */
    later(1000);
    CHECK(g.monitor.selected == -1 && g.monitor.pid == 9 && g.monitor.ending == -1 &&
          !strcmp(g.monitor.status, "edit (pid 9) has ended"));
    /* moved on before it ended: nothing is said */
    add_task(9, GUI_TASK_SLEEPING, 1, GUI_TASK_USER, "edit");
    later(1000);
    CHECK(selected_pid() == 9);
    key(GUI_DELETE); key(9); key('\r');
    key(GUI_HOME);
    drop_task(9);
    later(1000);
    CHECK(g.monitor.ending == -1 && selected_pid() == 0 && !g.monitor.status[0]);
    /* the list cut to the newest: not yet; then the whole list without it */
    add_task(9, GUI_TASK_SLEEPING, 1, GUI_TASK_USER, "edit");
    later(1000);
    key(GUI_END);
    key(GUI_DELETE); key(9); key('\r');
    CHECK(g.monitor.ending == 9);
    drop_task(9);
    for(pid = 100; fake_ntasks < GUI_TASK_MAX; pid++) add_task(pid, GUI_TASK_SLEEPING, -1, 0, "worker");
    later(1000);
    CHECK(g.monitor.ending == 9 && !strcmp(g.monitor.status, "edit (pid 9) ends when its system call does"));
    drop_task(100);
    later(1000);
    CHECK(g.monitor.ending == -1 && !strcmp(g.monitor.status, "edit (pid 9) has ended"));
    /* gone by the reading right after the kill (its call ended meanwhile): it has ended, at once */
    fake_sys_reset();
    waiting_sys.kill = quick_kill;
    gui_close(&g, GUI_MONITOR);
    gui_open(&g, GUI_MONITOR);
    key(GUI_END);
    key(GUI_DELETE); key(9); key('\r');
    CHECK(fake_killed == 9 && row_of(9) < 0 && g.monitor.ending == -1 && !strcmp(g.monitor.status, "edit (pid 9) has ended"));
    /* ended at once (the fake's kill): the same */
    g.sys = &fake_sys;
    fake_sys_reset();
    gui_close(&g, GUI_MONITOR);
    gui_open(&g, GUI_MONITOR);
    key(GUI_END);
    key(GUI_DELETE); key(9); key('\r');
    CHECK(fake_killed == 9 && g.monitor.ending == -1 && !strcmp(g.monitor.status, "edit (pid 9) has ended"));
    /* a refusal says only that: nothing has ended */
    key(GUI_HOME); key(GUI_DOWN);                       /* init, pid 1: the fake refuses it */
    key(GUI_DELETE); key(9); key('\r');
    CHECK(fake_killed == 1 && g.monitor.ending == -1 && !strcmp(g.monitor.status, "not one to end"));
}

/* Find next with the followed task gone: from the row it was on (the task there now first), as Down goes on */
static void monitor_find_gone(void) {
    desktop(960, 540);
    add_task(20, GUI_TASK_SLEEPING, 1, GUI_TASK_USER, "edit");
    gui_open(&g, GUI_MONITOR);
    key(GUI_END); key(GUI_UP);                          /* edit, pid 9, row 4 */
    CHECK(selected_pid() == 9 && g.monitor.selected == 4);
    drop_task(9);
    later(1000);
    CHECK(g.monitor.selected == -1 && g.monitor.hint == 4);
    key(6);                                             /* "e": idle (row 0) has one too */
    fake_type(&g, "e\r");
    CHECK(selected_pid() == 20 && !strcmp(g.monitor.status, "Found edit (pid 20)"));
    /* gone from the last row: around to the top */
    drop_task(20);
    later(1000);
    CHECK(g.monitor.selected == -1 && g.monitor.hint == 4 && g.monitor.ntasks == 4);
    key(6);
    fake_type(&g, "e\r");
    CHECK(selected_pid() == 0 && !strcmp(g.monitor.status, "Found idle (pid 0)"));
}

/* Disks' M, E and Enter with nothing selected: the status bar says so (and why, when the device followed went) */
static void disks_none(void) {
    desktop(960, 540);
    gui_open(&g, GUI_DISKS);
    select_disk("usb1p1");
    fake_ndisks = 3;                                    /* the stick pulled out */
    later(2000);
    CHECK(g.disks.selected == -1 && !strcmp(g.disks.name, "usb1p1"));
    key('m');
    CHECK(!mounted_dev[0] && !strcmp(g.disks.status, "Mount: nothing is selected (usb1p1 is not there any more)"));
    g.disks.status[0] = 0;
    key('e');
    CHECK(!ejected_dev[0] && !strcmp(g.disks.status, "Eject: nothing is selected (usb1p1 is not there any more)"));
    g.disks.status[0] = 0;
    key('\r');
    CHECK(!mounted_dev[0] && !strcmp(g.disks.status, "Mount: nothing is selected (usb1p1 is not there any more)"));
    fake_ndisks = 0;                                    /* no device at all: none followed */
    gui_close(&g, GUI_DISKS);
    gui_open(&g, GUI_DISKS);
    key('e');
    CHECK(g.disks.selected == -1 && !strcmp(g.disks.status, "Eject: nothing is selected"));
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
    kernel_kill();
    kernel_tasks();
    kernel_disks();
    monitor_short();
    monitor_count();
    readings();
    meter_edges();
    monitor_ties();
    disks_guard();
    disks_mount_view();
    monitor_ending();
    monitor_find_gone();
    disks_none();
    return fake_done("test_guisys");
}
