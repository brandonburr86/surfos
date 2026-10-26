/*
SurfOS Shutdown
--------------------
File: shutdown.c    Date: 10/25/26 (robustness, track B: L22-L25)
--------------------
reboot reset the machine at once and nothing turned it off, so a volume mounted read-write
stayed marked not cleanly unmounted, and its last writes could be lost. Now reboot and poweroff
(kernel commands, shell/main.c, which surfsh reaches through SYS_kcmd) start the shutdown
kernel thread, and so does Ctrl+Alt+Del, through init, since an interrupt handler may not make
a task (audit T11). The thread:

  1. moves to "/": umount refuses a volume that any task's directory is on, its own too, and
     a new task starts in its creator's;
  2. makes init start no shell and spawn no program (-EAGAIN: shutdown_started());
  3. for poweroff, reads what soft off needs while there is memory for the DSDT;
  4. runs the hooks (track A's history, A10), which may still write the stick, then ends every
     process and ring-0 shell with the kill that waits for the end of the call each is in
     (kill_task_patient()), again on each pass of its wait, so that one a spawn under way
     makes meanwhile goes too; 10 s at most, naming any left;
  5. makes the network manager mount the stick no more (shutdown_unmounting()) and waits for
     what it is doing there to end (netman_stick_idle()), then writes out and unmounts the
     volumes mounted read-write, all in a worker it waits for 15 s at most, so a volume lock
     held for ever cannot hang it; a busy one is tried again meanwhile. One it could not
     unmount is written out (vfs_sync(), also in a worker) and left marked not cleanly
     unmounted;
  6. resets the machine (reboot), or turns it off with ACPI S5 (poweroff, kernel/acpi.c).
     When neither works, it says that the machine may be turned off and halts.

Ctrl+Alt+Del pressed again resets at once, as the first press used to; a key held down is one
press, however long the keyboard repeats it (del_key_event()), and during a shutdown begun
otherwise the first press only says so. reboot -f still resets at once.
10/25/26 (the review of B4): the kill pass on every wait, the stick open to the hooks, the
second press, and the list of mounts as long as it is.
*/

#include <surfos/types.h>
#include <surfos/shutdown.h>
#include <surfos/task.h>
#include <surfos/console.h>
#include <surfos/system.h>
#include <surfos/sync.h>
#include <surfos/ktimer.h>
#include <surfos/irq.h>
#include <surfos/acpi.h>
#include <fs/vfs.h>
#include <sys/bdev.h>
#include <mm/kalloc.h>
#include <net/netman.h>
#include <blibc_common.h>

#define END_WAIT_MS      10000          /* for the programs to end */
#define UNMOUNT_WAIT_MS  15000          /* for the volumes to be written out and unmounted */
#define UNMOUNT_TRY_MS   250            /* a busy one is tried again this often meanwhile */
#define SYNC_WAIT_MS     5000           /* for what could not be unmounted to be written out */
#define MAX_HOOKS        4
#define NETMAN_WAIT_MS   5000           /* for the network manager's work on the stick to end, within the 15 s */

static volatile int how_now;            /* 0, or SHUT_REBOOT or SHUT_POWEROFF once a shutdown has started */
static volatile bool unmounting;        /* the unmounts have begun: the network manager mounts nothing more */
static volatile bool key_pressed;       /* Ctrl+Alt+Del, once */
static void (*hooks[MAX_HOOKS])(void);

bool shutdown_started(void) {
    return how_now != 0;
}

bool shutdown_unmounting(void) {
    return unmounting;
}

int shutdown_hook(void (*fn)(void)) {
    u_int i;
    u_long flags = irq_save();
    for(i = 0; i < MAX_HOOKS && hooks[i]; i++) ;
    if(i < MAX_HOOKS) hooks[i] = fn;
    irq_restore(flags);
    return i < MAX_HOOKS ? 0 : -ENOMEM;
}

/**** work that may never end, waited for a while ****/

struct bounded {
    void (*fn)(void *arg);
    void *arg;
    event_t done;
};

static void bounded_main(void *arg) {
    struct bounded *b = (struct bounded *)arg;
    b->fn(b->arg);
    event_set(&b->done);                /* the last it touches: the waiter may free it now */
}

/* fn(arg) in a kernel thread of its own, waited for ms at most: true when it ended. One that did not keeps its record
   (and arg) for ever: it may still use them. With no memory for a thread, fn runs here. */
static bool run_bounded(const char *name, void (*fn)(void *arg), void *arg, u_long ms) {
    struct bounded *b = (struct bounded *)kcalloc(1, sizeof(*b));
    bool ended;
    if(!b) {
        fn(arg);
        return true;
    }
    b->fn = fn;
    b->arg = arg;
    event_init(&b->done);
    if(!kthread_create_on(name, &conArray[0], bounded_main, b, PL_HIGH, TF_DETACHED)) {
        kfree(b);
        fn(arg);
        return true;
    }
    ended = event_wait_timeout(&b->done, ms) == 0;
    if(ended) kfree(b);
    return ended;
}

/**** the steps ****/

static void run_hooks(void *arg) {
    u_int i;
    for(i = 0; i < MAX_HOOKS && hooks[i]; i++) hooks[i]();
}

/* The kill on every pass, not once: a spawn that began before the shutdown makes its task only after its checks (the
   program read, the memory), and that task is ended too; one already killed is passed over (kill_waiting()) */
static void end_programs(void) {
    char left[160];
    u_long start = uptime_ms();
    u_int n = task_users_left(NULL, 0);
    for(;;) {
        task_end_users();
        if(!task_users_left(NULL, 0) || uptime_ms() - start >= END_WAIT_MS) break;
        sleep_ms(100);
    }
    if(task_users_left(left, sizeof(left))) kprintf("shutdown: still running after %u s: %s\n", END_WAIT_MS / 1000, left);
    else kprintf("shutdown: %u program%s and shell%s ended\n", n, n == 1 ? "" : "s", n == 1 ? "" : "s");
}

/* the mounts as they are, in a list as long as there are of them (and a few made meanwhile); NULL with no memory */
static struct vfs_mount_view *mount_list(int *n) {
    struct vfs_mount_view one, *v;
    int room = vfs_mount_views(&one, 1) + 4, got;
    v = (struct vfs_mount_view *)kcalloc((size_t)room, sizeof(*v));
    if(!v) return NULL;
    got = vfs_mount_views(v, room);
    *n = got < room ? got : room;
    return v;
}

struct unmounts {
    struct vfs_mount_view *m;           /* the read-write ones, set by the worker; NULL until then */
    bool *done;
    int n;
    volatile bool listed;
};

/* The network manager stopped at the stick first (its write under way ends, its own /.netman mount goes), then the
   volumes mounted read-write, the last mounted first, a busy one again UNMOUNT_TRY_MS later until all are done or
   UNMOUNT_WAIT_MS have gone by (the waiter stops waiting then) */
static void unmount_all(void *arg) {
    struct unmounts *u = (struct unmounts *)arg;
    struct vfs_mount_view *all;
    u_long start = uptime_ms();
    int i, n, r, busy;
    char why[96];
    u_int lost;
    unmounting = true;
    if(!netman_stick_idle(NETMAN_WAIT_MS))
        kprintf("shutdown: the network manager is still at the stick after %u s\n", NETMAN_WAIT_MS / 1000);
    all = mount_list(&n);
    if(all) {
        u->m = (struct vfs_mount_view *)kcalloc((size_t)(n ? n : 1), sizeof(*u->m));
        u->done = (bool *)kcalloc((size_t)(n ? n : 1), sizeof(*u->done));
        for(i = 0; i < n && u->m && u->done; i++) if(all[i].dev && !all[i].readonly) u->m[u->n++] = all[i];
        kfree(all);
    }
    u->listed = true;
    if(!u->m || !u->done) return;
    do {
        busy = 0;
        for(i = u->n - 1; i >= 0; i--) {
            if(u->done[i]) continue;
            r = vfs_busy(u->m[i].path, why, sizeof(why));
            if(r == -EBUSY) {
                busy++;
                continue;
            }
            lost = 0;
            r = r == -ENOENT ? -ENOENT : vfs_umount_lost(u->m[i].path, &lost);
            if(r == -EBUSY) {
                busy++;
                continue;
            }
            u->done[i] = true;
            if(r == -ENOENT) continue;                      /* unmounted meanwhile, by its own user */
            if(r) kprintf("shutdown: %s could not be unmounted: %s\n", u->m[i].path, strerror(r));
            else if(lost) kprintf("shutdown: %s unmounted, but %u changed block%s could not be written\n", u->m[i].path,
                                  lost, lost == 1 ? "" : "s");
            else kprintf("shutdown: %s written out and unmounted\n", u->m[i].path);
        }
        if(busy) sleep_ms(UNMOUNT_TRY_MS);
    } while(busy && uptime_ms() - start < UNMOUNT_WAIT_MS - 2 * UNMOUNT_TRY_MS);
}

static void sync_all(void *arg) {
    vfs_sync();
}

static void unmount_volumes(void) {
    struct unmounts *u = (struct unmounts *)kcalloc(1, sizeof(*u));
    int i, left = 0;
    char why[96];
    if(!u) {
        unmounting = true;
        kprintf("shutdown: no memory to list the mounts: everything is written out (sync), nothing unmounted\n");
        vfs_sync();
        return;
    }
    if(!run_bounded("unmount", unmount_all, u, UNMOUNT_WAIT_MS)) {
        kprintf("shutdown: the unmounts did not end within %u s\n", UNMOUNT_WAIT_MS / 1000);
        u = NULL;                                           /* the worker may still use it */
    } else if(!u->m || !u->done) {
        kprintf("shutdown: no memory to list the mounts: everything is written out (sync), nothing unmounted\n");
        left = 1;
    }
    for(i = 0; u && u->m && u->done && i < u->n; i++) {
        if(u->done[i]) continue;
        left++;
        if(vfs_busy(u->m[i].path, why, sizeof(why)) == -EBUSY) kprintf("shutdown: %s is busy: %s\n", u->m[i].path, why);
    }
    if(!u || left) {
        bool synced = run_bounded("sync", sync_all, NULL, SYNC_WAIT_MS);
        for(i = 0; u && u->m && u->done && i < u->n; i++) if(!u->done[i])
            kprintf("shutdown: %s %s and left marked not cleanly unmounted\n", u->m[i].path,
                    synced ? "written out" : "could not even be written out");
        if(!u || !u->m || !u->done)
            kprintf("shutdown: %s; what is still mounted is left marked not cleanly unmounted\n",
                    synced ? "everything is written out" : "the writing out did not end either");
    }
    if(u) {
        kfree(u->m);
        kfree(u->done);
        kfree(u);
    }
}

static void shutdown_main(void *arg) {
    int how = (int)(u_long)arg;
    struct acpi_s5 s5;
    strlcpy(curTask->cwd, "/", sizeof(curTask->cwd));
    kprintf("\nshutdown: %s: the programs end, the volumes are written out\n", how == SHUT_REBOOT ? "reboot" : "poweroff");
    memset(&s5, 0, sizeof(s5));
    if(how == SHUT_POWEROFF) acpi_s5_find(&s5);
    if(hooks[0] && !run_bounded("shuthook", run_hooks, NULL, END_WAIT_MS))
        kprintf("shutdown: the hooks did not end within %u s\n", END_WAIT_MS / 1000);
    end_programs();
    unmount_volumes();                                      /* the network manager stopped at the stick first */
    if(how == SHUT_REBOOT) {
        kprintf("shutdown: resetting the machine\n");
        reset();                                            /* it never returns: it halts when nothing resets */
    }
    if(s5.ok) {
        kprintf("shutdown: turning the machine off (ACPI S5)\n");
        acpi_s5_enter(&s5);
        kprintf("shutdown: the machine is still on 3 s after ACPI S5\n");
    } else kprintf("shutdown: soft off (ACPI S5) is not possible: %s\n", s5.why);
    kprintf("\nIt is now safe to turn off your computer.\n");
    halt();
}

int shutdown_start(int how) {
    u_long flags = irq_save();
    if(how_now) {
        irq_restore(flags);
        return -EBUSY;
    }
    how_now = how;
    irq_restore(flags);
    if(!kthread_create_on("shutdown", &conArray[0], shutdown_main, (void *)(u_long)how, PL_HIGH, TF_DETACHED)) {
        how_now = 0;
        return -ENOMEM;
    }
    return 0;
}

/**** poweroff -n ****/

int shutdown_dry_run(void) {
    struct vfs_mount_view *m;
    struct acpi_s5 s5;
    char list[256];
    int i, n = 0, rw = 0;
    u_int users = task_users_left(list, sizeof(list));
    printf("poweroff -n: what poweroff would do now\n");
    printf("    end %u program%s and shell%s%s%s\n", users, users == 1 ? "" : "s", users == 1 ? "" : "s",
           users ? ": " : "", list);
    m = mount_list(&n);
    if(!m) printf("    (no memory to list the mounts)\n");
    for(i = 0; m && i < n; i++) {
        if(!m[i].dev || m[i].readonly) continue;
        printf("    write out and unmount %s (%s on %s)\n", m[i].path, m[i].type, m[i].dev->name);
        rw++;
    }
    if(m && !rw) printf("    no volume is mounted read-write\n");
    kfree(m);
    acpi_s5_find(&s5);
    acpi_s5_print(&s5);
    if(!s5.ok) printf("    then say \"It is now safe to turn off your computer.\" and halt\n");
    return s5.ok ? 0 : 1;
}

/**** Ctrl+Alt+Del ****/

/* The Delete key, from the keyboard's interrupt handler: its make codes (make) and its break code, and whether Ctrl and
   Alt were down at a make. A make with no break since the last is the keyboard repeating a key held down: never a
   second press (the review of B4: a press held half a second started the shutdown, and the first repeat reset the
   machine in the middle of its unmounts). Pure: the self test drives it with a state of its own. */
int del_key_event(struct del_key *k, bool make, bool ctrl_alt) {
    bool repeat = k->down;
    if(!make) {
        k->down = false;
        return DEL_NOTHING;
    }
    k->down = true;
    if(ctrl_alt) return repeat ? DEL_NOTHING : DEL_PRESS;
    return DEL_DELETE;                                      /* the Delete key itself repeats, as editing wants */
}

/* From the keyboard's interrupt handler, which may not make a task: init is woken to start the shutdown (a reboot).
   Pressed again it resets the machine at once; the first press during a shutdown begun otherwise only says so. */
void shutdown_key(void) {
    if(key_pressed) {
        kprintf("\nCtrl+Alt+Del again: resetting the machine at once\n");
        reset();
    }
    key_pressed = true;
    if(how_now) {
        kprintf("\nCtrl+Alt+Del: SurfOS is shutting down; press it again to reset at once\n");
        return;
    }
    kprintf("\nCtrl+Alt+Del: SurfOS shuts down, then resets the machine (press it again to reset at once)\n");
    task_wake_init();
}

bool shutdown_key_take(void) {
    return key_pressed && !how_now;
}
