/*
SurfOS network manager
--------------------
File: netman.c  Date: 10/17/26
--------------------
The owner's settings and saved Wi-Fi networks, kept on the stick SurfOS was started from,
and the network they ask for (include/net/netman.h; the brief is
docs/superpowers/plans/2026-10-17-network-manager.md).

Where. GRUB names the FAT volume it was started from (bootvol=); driver/part.c finds it
(bdev_boot_vol()). Its folder surfos-config/ holds settings.conf and networks.conf (the text:
net/netcfg.c). The volume is mounted only for the moment a file is read or written, on
/.netman, or used where the owner has mounted it (that mount held meanwhile: it can neither go
nor be swapped for another); nothing stays mounted by itself. It is written only when a mount
of it may write (bdev_mount_readonly(): the one-stick volume); a flashed SURFOSUSB stick is
read, never written. Without bootvol=, or with the kernel word settings=off (GRUB's entry
"without the settings on the stick"), nothing is read at boot, nothing is joined and no host
name is sent; under settings=off a change is still saved, for the next start, and changes
nothing now.

A write goes to <name>.new, which is synced, renamed over the file and read back (through the
block cache: the check is of what the file system wrote). settings.conf is edited line by line
(the owner's lines and comments stay), networks.conf written anew, the blocks its reader could
not take kept as they were; but never a part with a password in it, which SurfOS would then be
writing (the change is refused), and never a file that would read as other networks than meant
(netcfg_reads_back(): a name written twice, whose first was just forgotten). Every change reads
the file from the stick first, so a change never loses what another computer added.

At boot (the netman thread, on F1): the files are read once the volume is found (20 seconds
at most), each line that is not understood said with its number; then
* wifi = on with saved networks: the Wi-Fi keeper (the wifiman thread) brings the card up,
  scans, joins the first saved network in range (a hidden one by a probe for its name) with
  its key, and asks for an address, and again (30 s, doubling to 5 minutes) for as long as
  that association has none. Afterwards it stays out of the way of the wifi command: while
  wlan0 is joined, joining or reconnecting it does nothing; after a wifi leave or wifi down
  it waits until the station gives a network up (its reconnects run out) or wifi connect hands
  the card back, then looks for a saved network every 30 seconds, every 2 minutes after 20
  tries, the repeats (the station's failures to join included) in the log only. A key refused
  three times in a row is set aside for 10 minutes. While a telnet server runs (any telnetd
  process counts) or is to start by itself (telnetd = on until it has started; autonet until
  it is done), it joins no open network, and looks again just before each: telnetd has no
  password. 10/24/26: nor while an FTP server runs (any ftpd process), which has none either
  and lets its clients write files (netman_ftp_exposed()).
* ethernet = dhcp: an Ethernet interface whose link comes up gets a lease (30 s, doubling to
  5 minutes, between tries without one), unless autonet is still at it.
* telnetd = on: telnetd on [port] on F1 once there is an address on Ethernet or on a WPA2
  Wi-Fi network (netman_trusted()), never because of an open network, as the settings read at
  boot say (a change acts at the next start). With the kernel word autonet, autonet starts it
  for an Ethernet address (kernel/autonet.c); when it has not, once it is done, the setting
  does as without it.
* Every lease, wherever it came from and in every start, is renewed: at half its time from
  the server that granted it, at seven eighths from any server, given up when it runs out or
  is refused (netcfg_lease_step(), dhcp_renew()). An Ethernet link that comes back while its
  interface kept the lease it had when the link went has the address confirmed first (RFC
  2131's INIT-REBOOT).
The setting hostname goes into the DHCP requests (dhcp_set_hostname()) once the settings are
read, or once the settings command sets it.

Keys. A saved network's key is the PMK (wifi save takes the one the station joined with,
wlan_station(); a password typed into networks.conf by hand is turned into it here). Keys are
kept in RAM for the keeper and wiped when a network is forgotten; nothing logs one, and
netman_networks() gives the commands everything but the key.

10/25/26 (track A, A10): surfsh's two files beside the settings take the same way (netman_file_read(),
netman_file_write(), for fs/devfs.c's /dev/config): surfshrc, the owner's startup file, only read, and
history, read, and written only while the history setting is on; no other file. Under settings=off
neither is read or written. A reader that comes while the boot still reads the settings waits for it, 30 s at most
(the kernel log says how long); with nothing to read (settings=off, no bootvol=, no netman thread) it never waits,
and while the stick is still looked for only with a USB controller, until 8 s from the start (LOOK_GRACE_MS).
10/25/26 (the kernel review of track A, 4): history has a generation, the count of SurfOS's writes of it
(history_gen, under stick_lock): a reader gets the one it read, and a write made from an older one is refused
(EAGAIN), so of two shells that read the file and then save it, the second reads it again and merges rather than
writing the first one's lines away. A write is refused at once while the stick may not be written (the flashed
SURFOSUSB stick: netman_can_save()).

Locks: stick_lock around a mount of the stick and what is read and written under it, the change
in RAM included (so the boot's read and a command's change never undo each other); nm_lock
around the settings and the networks in RAM (never held across a wait); connect_lock around a
connect (the keeper's or wifi connect's) and the keeper's DHCP for its association. stick_lock
and connect_lock come before nm_lock, never after it.
*/

#include <stdarg.h>
#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/kernel.h>
#include <surfos/task.h>
#include <surfos/sync.h>
#include <surfos/timer.h>
#include <surfos/ktimer.h>
#include <surfos/klog.h>
#include <surfos/multiboot.h>
#include <surfos/process.h>
#include <surfos/proc.h>
#include <mm/kalloc.h>
#include <sys/bdev.h>
#include <sys/driver.h>
#include <fs/vfs.h>
#include <net/net.h>
#include <net/dhcp.h>
#include <net/wlan.h>
#include <net/wlan_cmd.h>
#include <net/wpa.h>
#include <net/ieee80211.h>
#include <net/netcfg.h>
#include <net/netman.h>
#include <surfos/shutdown.h>
#include <blibc_common.h>

#define BOOTVOL_WAIT_MS  20000          /* how long the stick may take to be found at boot */
#define LOOK_GRACE_MS    8000           /* 10/25/26 (A10): how long from the start surfsh's files wait for it */
#define TELNETD_PATH     "/initrd/bin/telnetd"
#define TELNETD_DONE_MS  30000          /* telnetd on returns once its server runs (3 s at most) */
#define NM_MOUNT         "/.netman"     /* the short mount of the stick */
#define WIFI_SCAN_MS     10000
#define WIFI_RETRY_MS    30000          /* the keeper's next look when no saved network was joined */
#define WIFI_RETRY_LATE  120000         /* after WIFI_LATE_TRIES of them */
#define WIFI_LATE_TRIES  20
#define WIFI_BUSY_MS     5000           /* a command had the card: soon again */
#define WIFI_REFUSALS    3              /* a saved key refused so many times in a row ... */
#define WIFI_REFUSED_MS  600000         /* ... is not tried for so long */
#define BSS_MAX          48
#define DHCP_TRIES       3
#define DHCP_MS          8000
#define DHCP_PAUSE_MS    2000
#define DHCP_RETRY_MS    30000          /* a link without a lease (Ethernet, the keeper's Wi-Fi): the next DHCP, */
#define DHCP_RETRY_MAX   300000         /* doubling to this */
#define RENEW_MS         5000
#define MAX_IFS          8

/* what was read at boot */
#define WHERE_NONE       0              /* no bootvol= */
#define WHERE_OFF        1              /* settings=off */
#define WHERE_NOT_FOUND  2              /* the volume did not turn up */
#define WHERE_FAILED     3              /* found, but not read (why in where_why) */
#define WHERE_READ       4              /* read (a file may be missing: the defaults) */
#define WHERE_LOOKING    5              /* the boot still waits for the volume */

struct saved {
    struct netcfg_net net;              /* its key in net.psk */
    u_long refused_until;               /* ticks: its key was refused WIFI_REFUSALS times; 0 */
    u_int refusals;                     /* refused in a row so far */
    bool told;                          /* that it was set aside was said on F1 (once; later in the log) */
};

static mutex_t nm_lock = MUTEX_INIT("netman");
static struct netcfg_settings cfg;
static struct netcfg_settings boot_cfg; /* as the file said at boot (the defaults when not read): telnetd's start */
static u32 cmd_set;                     /* bit k: the settings command set key k (the boot's read leaves it) */
static struct saved saved[NETCFG_NETS_MAX];
static int nsaved, nkept;               /* nkept: parts of networks.conf not understood, kept there as they were */
static volatile int where_state;
static char where_vol[BDEV_NAME_LEN], where_why[96];
static bool had_settings, had_networks, where_readonly;
static u_int boot_notes;

static mutex_t stick_lock = MUTEX_INIT("netman-stick");
static mutex_t connect_lock = MUTEX_INIT("wifi-connect");
static mutex_t keeper_lock = MUTEX_INIT("wifiman");    /* start_keeper(): one keeper */
static surf_task *volatile wifiman;
static volatile bool keeper_connecting; /* a connect (the keeper's or wifi connect's) runs */
static volatile bool keeper_owns;       /* the association of the last connect is the manager's: */
static volatile u_int keeper_join;      /* the station's number of its join request */
static volatile u_long keeper_dhcp_next;    /* ticks: its next DHCP while it has no address (0: at once) */
static volatile u_int keeper_dhcp_fails;
static volatile bool keeper_at_boot;    /* the keeper joins at once (the boot's); else only once a network is lost */
static volatile bool boot_read_pending; /* 10/25/26: the boot has not read the settings yet (a file's reader waits) */
static volatile bool boot_looking;      /* 10/25/26: the boot looks for the stick still (found: it reads it) */
static u_int history_gen;               /* 10/25/26 (the kernel review, 4): history's writes so far, under stick_lock */
static volatile bool keeper_resume;     /* wifi connect hands the card back: the keeper looks by itself again */
static volatile u_int keeper_resume_stops;  /* 11/05/26 (Settings' review, M1): the station's leaves and downs when it
                                               was set: one since is the owner's, which the resume never undoes */
static volatile bool telnetd_due;       /* telnetd = on: the server starts once there is a trusted address */

/* not optimised away */
static void wipe(void *p, size_t n) {
    volatile u8 *q = (volatile u8 *)p;
    while(n--) *q++ = 0;
}

/* a line for the kernel log only (dmesg), not the screen; one cut short still ends the line */
static void klogf(const char *fmt, ...) {
    char buf[320];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if(n >= (int)sizeof(buf)) buf[sizeof(buf) - 2] = '\n';
    klog_write(buf);
}

/* what a connect does, on the command's console (cmd), else "+NET:" on F1 and in the log, or in the log only (quiet:
   the keeper saying it again) */
static void say(bool cmd, bool quiet, const char *fmt, ...) {
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if(cmd) printf("    %s\n", buf);
    else if(quiet) klogf("+NET: %s\n", buf);
    else kprintf("+NET: %s\n", buf);
}

static const char *ssid_str(const u8 *ssid, u_int len, char *buf, size_t size) {
    return ieee80211_ssid_text(ssid, len, false, buf, size);
}

/* ticks from now, never 0 (0 means "none" below) */
static u_long ticks_after(u_long ms) {
    u_long t = getticks() + ms / (1000 / HZ);
    return t ? t : 1;
}

/**** the stick ****/

struct stick {
    char root[VFS_PATH_MAX];            /* where the volume is mounted */
    struct vnode *held;                 /* that mount's root, held while the files are worked on */
    bool ours;                          /* the mount on /.netman is this one's: it goes again */
};

/* the mount goes again (a command that has a file open there for a moment is waited for); the owner's own mount
   stays, its writes synced */
static void stick_close(struct stick *st) {
    int r = 0, tries;
    if(st->held) {
        vfs_release_mount(st->held);
        st->held = NULL;
    }
    if(!st->ours) {
        vfs_sync();
        return;
    }
    st->ours = false;
    for(tries = 0; tries < 40; tries++) {
        r = vfs_umount(NM_MOUNT);
        if(r != -EBUSY) break;
        sleep_ms(50);
    }
    if(r && r != -ENOENT)                               /* ENOENT: gone already (an eject meanwhile) */
        kprintf("+NET: the stick stays mounted on %s: %s (umount %s)\n", NM_MOUNT, strerror(r), NM_MOUNT);
}

/* the volume the settings are on, for a moment: where the owner mounted it, else on /.netman (read-only for a
   read), the mount held until stick_close(). 0, or -errno and why */
static int stick_open(bool write, struct stick *st, char *why, size_t wsize) {
    struct bdev *v = bdev_boot_vol();
    bool ro = false;
    int r = -EBUSY, tries;
    st->root[0] = 0;
    st->held = NULL;
    st->ours = false;
    if(shutdown_unmounting()) {                         /* 10/25/26 (L22): its volumes are being unmounted */
        snprintf(why, wsize, "SurfOS is shutting down");
        return -EAGAIN;
    }
    if(!v) {
        snprintf(why, wsize, "%s", bdev_boot_vol_named() ? "the stick SurfOS was started from is not there (lsblk)" :
                 "SurfOS was not started from a stick GRUB named (no bootvol= on the kernel command line)");
        return -ENODEV;
    }
    if(v->gone) {
        snprintf(why, wsize, "%s, the stick SurfOS was started from, is unplugged", v->name);
        return -ENODEV;
    }
    if(write && bdev_mount_readonly(v)) {
        snprintf(why, wsize, "SurfOS was started from the flashed SURFOSUSB stick (%s), which it only reads", v->name);
        return -EROFS;
    }
    if(write && (v->readonly || bdev_disk(v)->readonly)) {
        snprintf(why, wsize, "%s is write-protected (its switch)", v->name);
        return -EROFS;
    }
    for(tries = 0; tries < 25 && r == -EBUSY; tries++) {    /* EBUSY: a command mounts it meanwhile: that mount */
        if(tries) sleep_ms(200);                               /* once it is made (FAT may count for seconds) */
        if(vfs_hold_mount(v, st->root, sizeof(st->root), &st->held, &ro) == 0) {
            if(!write || !ro) return 0;
            snprintf(why, wsize, "%s is mounted read-only on %s: umount %s, then try again", v->name, st->root, st->root);
            vfs_release_mount(st->held);
            st->held = NULL;
            return -EROFS;
        }
        r = vfs_mount_flags(v, NM_MOUNT, NULL, MNT_QUIET | (write ? 0 : MNT_RDONLY));
        if(r == 0) {
            st->ours = true;
            if(vfs_hold_mount(v, st->root, sizeof(st->root), &st->held, NULL) == 0) return 0;
            stick_close(st);
            snprintf(why, wsize, "%s went while it was being mounted", v->name);
            return -ENODEV;
        }
    }
    snprintf(why, wsize, "%s could not be mounted: %s", v->name, strerror(r));
    return r;
}

/* root/surfos-config/name into out; false when that is longer than a path may be */
static bool path_of(char *out, size_t size, const char *root, const char *name) {
    return snprintf(out, size, "%s/%s/%s", root, NETCFG_DIR, name) < (int)size;
}

/* the file at path into buf (max bytes, then a NUL): 0 and *len; -ENOENT when there is none, -EFBIG when longer */
static int read_file(const char *path, char *buf, u_int max, u_int *len) {
    struct file *f;
    u_int total = 0;
    int r = vfs_open(path, O_RDONLY, &f), n;
    *len = 0;
    if(r) return r;
    for(;;) {
        char extra;
        n = total < max ? vfs_read(f, buf + total, max - total) : vfs_read(f, &extra, 1);
        if(n < 0) { r = n; break; }
        if(n == 0) break;
        if(total >= max) { r = -EFBIG; break; }
        total += (u_int)n;
    }
    vfs_close(f);
    buf[total < max ? total : max] = 0;
    *len = total;
    return r;
}

/* data (len bytes) as root/surfos-config/name: written as name.new, synced, renamed over name, read back. 0, or
   -errno and why */
static int write_file(const char *root, const char *name, const char *data, u_int len, char *why, size_t wsize) {
    char dir[VFS_PATH_MAX], path[VFS_PATH_MAX], tmp[VFS_PATH_MAX], *back;
    struct file *f;
    u_int done = 0, got;
    int r, n;
    if(snprintf(dir, sizeof(dir), "%s/%s", root, NETCFG_DIR) >= (int)sizeof(dir) ||
       snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path) ||
       snprintf(tmp, sizeof(tmp), "%s/%s.new", dir, name) >= (int)sizeof(tmp)) {
        snprintf(why, wsize, "%s/%s under %s would be a path too long", NETCFG_DIR, name, root);
        return -ENAMETOOLONG;
    }
    r = vfs_mkdir(dir);
    if(r && r != -EEXIST) {
        snprintf(why, wsize, "%s/ could not be made: %s", NETCFG_DIR, strerror(r));
        return r;
    }
    r = vfs_open(tmp, O_WRONLY | O_CREAT | O_TRUNC, &f);
    if(r) {
        snprintf(why, wsize, "%s/%s.new could not be made: %s", NETCFG_DIR, name, strerror(r));
        return r;
    }
    while(done < len) {
        n = vfs_write(f, data + done, len - done);
        if(n <= 0) { r = n < 0 ? n : -EIO; break; }
        done += (u_int)n;
    }
    if(!r) r = vfs_fsync(f);
    vfs_close(f);
    if(!r) r = vfs_rename(tmp, path);
    if(r) {
        vfs_unlink(tmp);
        snprintf(why, wsize, "%s/%s could not be written: %s", NETCFG_DIR, name, strerror(r));
        return r;
    }
    back = (char *)kalloc(len + 2);
    if(!back) {
        snprintf(why, wsize, "no memory to read %s/%s back", NETCFG_DIR, name);
        return -ENOMEM;
    }
    r = read_file(path, back, len + 1, &got);
    if(!r && (got != len || memcmp(back, data, len))) r = -EIO;
    wipe(back, len + 2);
    kfree(back);
    if(r) snprintf(why, wsize, "%s/%s read back is not what was written: %s", NETCFG_DIR, name, strerror(r));
    return r;
}

/**** the saved networks ****/

/* networks written by hand with a password: its key made, the password wiped. 0, or -1 when a password made no key
   (said through note; that network is left out of list) */
static int make_keys(struct netcfg_net *list, int *n, netcfg_note_fn note, void *arg) {
    int i, j, r = 0;
    for(i = j = 0; i < *n; i++) {
        struct netcfg_net *net = &list[i];
        if(net->has_password && !net->has_psk) {
            if(wpa_passphrase_to_pmk(net->password, net->ssid, net->ssid_len, net->psk) == 0) net->has_psk = true;
            else if(note) note(arg, 0, "a password that makes no key: that network is left out");
        }
        wipe(net->password, sizeof(net->password));
        net->has_password = false;
        if(net->security == NETCFG_SEC_WPA2 && !net->has_psk) {
            wipe(net, sizeof(*net));
            r = -1;
            continue;
        }
        if(i != j) {
            list[j] = *net;
            wipe(net, sizeof(*net));
        }
        j++;
    }
    *n = j;
    return r;
}

/* the networks in RAM become list (n; kept: the parts of the file not understood), the refusals of those still
   there, with the same key, kept */
static void set_saved(const struct netcfg_net *list, int n, int kept) {
    struct saved *old = (struct saved *)kcalloc(NETCFG_NETS_MAX, sizeof(struct saved));
    int i, k, nold;
    mutex_lock(&nm_lock);
    nold = nsaved;
    if(old) memcpy(old, saved, nold * sizeof(struct saved));
    for(i = 0; i < n && i < NETCFG_NETS_MAX; i++) {
        saved[i].net = list[i];
        saved[i].refused_until = 0;
        saved[i].refusals = 0;
        saved[i].told = false;
        for(k = 0; old && k < nold; k++) {
            if(old[k].net.ssid_len == list[i].ssid_len && !memcmp(old[k].net.ssid, list[i].ssid, list[i].ssid_len) &&
               !memcmp(old[k].net.psk, list[i].psk, NETCFG_KEY_LEN)) {
                saved[i].refused_until = old[k].refused_until;
                saved[i].refusals = old[k].refusals;
                saved[i].told = old[k].told;
            }
        }
    }
    for(k = i; k < NETCFG_NETS_MAX; k++) wipe(&saved[k], sizeof(saved[k]));
    nsaved = i;
    nkept = kept;
    mutex_unlock(&nm_lock);
    if(old) {
        wipe(old, NETCFG_NETS_MAX * sizeof(struct saved));
        kfree(old);
    }
}

/* what the reader of a change said, on the command's console */
static void save_note(void *arg, u_int line, const char *why) {
    if(line) printf("    networks.conf line %u: %s\n", line, why);
    else printf("    networks.conf: %s\n", why);
}

static void start_keeper(bool at_boot);

/* change networks.conf on the stick: read it, change (the list as read, its count; 0, or -errno and why), write it
   (what was not understood kept as it was), and the networks in RAM become what was written (not under settings=off:
   they are for the next start). *count: how many are saved now */
static int change_networks(int (*change)(struct netcfg_net *list, int *n, void *arg, char *why, size_t wsize), void *arg,
                           int *count, char *why, size_t wsize) {
    struct netcfg_net *list = (struct netcfg_net *)kcalloc(NETCFG_NETS_MAX + 1, sizeof(struct netcfg_net));
    struct netcfg_net *scratch = (struct netcfg_net *)kcalloc(NETCFG_NETS_MAX, sizeof(struct netcfg_net));
    struct netcfg_span *kept = (struct netcfg_span *)kcalloc(NETCFG_SPANS_MAX, sizeof(struct netcfg_span));
    char *text = (char *)kalloc(NETCFG_NETWORKS_MAX + 1), *out = (char *)kalloc(NETCFG_NETWORKS_MAX + 1);
    char path[VFS_PATH_MAX];
    struct stick st;
    u_int len = 0;
    bool opened = false;
    int n = 0, nk = 0, i, r;
    if(count) *count = 0;
    if(!list || !scratch || !kept || !text || !out) {
        snprintf(why, wsize, "no memory");
        r = -ENOMEM;
        goto done;
    }
    mutex_lock(&stick_lock);
    r = stick_open(true, &st, why, wsize);
    if(r) goto out;
    opened = true;
    if(!path_of(path, sizeof(path), st.root, NETCFG_NETWORKS)) {
        snprintf(why, wsize, "the path of %s/%s is too long", NETCFG_DIR, NETCFG_NETWORKS);
        r = -ENAMETOOLONG;
        goto out;
    }
    r = read_file(path, text, NETCFG_NETWORKS_MAX, &len);
    if(r == -ENOENT) r = len = 0;
    if(r) {
        snprintf(why, wsize, "%s/%s could not be read: %s", NETCFG_DIR, NETCFG_NETWORKS,
                 r == -EFBIG ? "it is longer than 16 KB" : strerror(r));
        goto out;
    }
    n = netcfg_parse_networks(text, len, list, NETCFG_NETS_MAX, kept, &nk, save_note, NULL);
    if(nk >= NETCFG_SPANS_MAX) {                        /* more than can be kept: a write could lose one */
        snprintf(why, wsize, "%s/%s has too many parts SurfOS does not understand: correct or delete them first",
                 NETCFG_DIR, NETCFG_NETWORKS);
        r = -EINVAL;
        goto out;
    }
    for(i = 0; i < nk; i++) {
        if(!kept[i].password) continue;                 /* SurfOS would be writing that password: no */
        snprintf(why, wsize, "%s/%s line %u: a password in a part SurfOS does not understand, which it would keep as it "
                 "is (and SurfOS never writes a password): correct or delete that part first", NETCFG_DIR,
                 NETCFG_NETWORKS, kept[i].pw_line);
        r = -EINVAL;
        goto out;
    }
    if(make_keys(list, &n, save_note, NULL)) {
        snprintf(why, wsize, "a password in %s/%s makes no key: correct it first (the file is left as it is)",
                 NETCFG_DIR, NETCFG_NETWORKS);
        r = -EINVAL;
        goto out;
    }
    r = change(list, &n, arg, why, wsize);
    if(r) goto out;
    r = netcfg_write_networks(list, n, text, kept, nk, out, NETCFG_NETWORKS_MAX + 1);
    if(r < 0) {
        snprintf(why, wsize, "%s/%s would be longer than 16 KB", NETCFG_DIR, NETCFG_NETWORKS);
        r = -EFBIG;
        goto out;
    }
    len = (u_int)r;
    if(!netcfg_reads_back(out, len, list, n, scratch, &nk)) {  /* a part kept as it was would come back as a network */
        snprintf(why, wsize, "it would bring back a network written by hand that SurfOS had left out (a name written "
                 "twice, or a 17th network?): correct %s/%s first", NETCFG_DIR, NETCFG_NETWORKS);
        r = -EINVAL;
        goto out;
    }
    r = write_file(st.root, NETCFG_NETWORKS, out, len, why, wsize);
    if(!r) {
        if(where_state != WHERE_OFF) set_saved(list, n, nk);
        if(count) *count = n;
    }
out:
    if(opened) stick_close(&st);
    mutex_unlock(&stick_lock);
done:
    if(text) wipe(text, NETCFG_NETWORKS_MAX + 1);
    if(out) wipe(out, NETCFG_NETWORKS_MAX + 1);
    if(list) wipe(list, (NETCFG_NETS_MAX + 1) * sizeof(struct netcfg_net));
    kfree(text);
    kfree(out);
    kfree(kept);
    kfree(scratch);
    kfree(list);
    return r;
}

struct save_arg {
    const struct netcfg_net *net;
    int had;
};

static int put_first(struct netcfg_net *list, int *n, void *arg, char *why, size_t wsize) {
    struct save_arg *a = (struct save_arg *)arg;
    int i = netcfg_find_net(list, *n, a->net->ssid, a->net->ssid_len);
    a->had = i;
    if(i >= 0) {
        memmove(&list[1], &list[0], i * sizeof(list[0]));      /* [0, i) one down, over the old one */
    } else {
        if(*n >= NETCFG_NETS_MAX) {
            snprintf(why, wsize, "%u networks are saved already: wifi forget one first", NETCFG_NETS_MAX);
            return -ENOSPC;
        }
        memmove(&list[1], &list[0], *n * sizeof(list[0]));
        (*n)++;
    }
    list[0] = *a->net;
    list[0].has_password = false;
    wipe(list[0].password, sizeof(list[0].password));
    return 0;
}

int netman_save_network(const struct netcfg_net *net, int *had, int *count, char *why, size_t size) {
    struct save_arg a;
    int r;
    a.net = net;
    a.had = -1;
    if(!net->ssid_len || net->ssid_len > NETCFG_SSID_MAX || (net->security == NETCFG_SEC_WPA2 && !net->has_psk)) {
        snprintf(why, size, "a network to save needs its name, and its key for WPA2");
        return -EINVAL;
    }
    r = change_networks(put_first, &a, count, why, size);
    if(had) *had = a.had;
    if(!r && where_state != WHERE_OFF) start_keeper(false);
    return r;
}

struct forget_arg {
    const u8 *ssid;
    u_int len;
};

static int take_out(struct netcfg_net *list, int *n, void *arg, char *why, size_t wsize) {
    struct forget_arg *a = (struct forget_arg *)arg;
    int i = netcfg_find_net(list, *n, a->ssid, a->len);
    if(i < 0) {
        snprintf(why, wsize, "no saved network has that name (wifi saved lists them)");
        return -ENOENT;
    }
    wipe(&list[i], sizeof(list[i]));
    memmove(&list[i], &list[i + 1], (*n - i - 1) * sizeof(list[0]));
    (*n)--;
    wipe(&list[*n], sizeof(list[0]));
    return 0;
}

int netman_forget_network(const u8 *ssid, u_int len, int *count, char *why, size_t size) {
    struct forget_arg a;
    a.ssid = ssid;
    a.len = len;
    return change_networks(take_out, &a, count, why, size);
}

int netman_networks(struct netman_net *out, int max, int *kept) {
    u_long now = getticks();
    int i;
    mutex_lock(&nm_lock);
    for(i = 0; i < nsaved && i < max; i++) {
        memcpy(out[i].ssid, saved[i].net.ssid, NETCFG_SSID_MAX);
        out[i].ssid_len = saved[i].net.ssid_len;
        out[i].security = saved[i].net.security;
        out[i].hidden = saved[i].net.hidden;
        out[i].refused = saved[i].refused_until && (long)(saved[i].refused_until - now) > 0;
    }
    if(kept) *kept = nkept;
    mutex_unlock(&nm_lock);
    return i;
}

/**** the settings ****/

void netman_settings(struct netcfg_settings *out) {
    mutex_lock(&nm_lock);
    *out = cfg;
    mutex_unlock(&nm_lock);
}

bool netman_settings_off(void) {
    return where_state == WHERE_OFF;
}

/* 10/25/26 (the review of B4): the shutdown's, once shutdown_unmounting() refuses every new stick_open(): the stick's
   lock taken and let go, so a read or write under way has ended and the short mount of it is gone before the
   shutdown lists what to unmount. ms at most (a lock held for ever cannot hang the shutdown); false then */
bool netman_stick_idle(u_long ms) {
    u_long waited;
    for(waited = 0; !mutex_trylock(&stick_lock); waited += 50) {
        if(waited >= ms) return false;
        sleep_ms(50);
    }
    mutex_unlock(&stick_lock);
    return true;
}

bool netman_can_save(void) {
    struct bdev *v = bdev_boot_vol();
    return v && !v->gone && !bdev_mount_readonly(v) && !v->readonly && !bdev_disk(v)->readonly;
}

void netman_where(char *buf, size_t size) {
    struct bdev *v = bdev_boot_vol();
    mutex_lock(&nm_lock);
    switch(where_state) {
    case WHERE_OFF:
        snprintf(buf, size, "not read at this start (settings=off): the defaults until SurfOS restarts; a change "
                 "is saved on %s for the next start", v ? v->name : "the stick");
        break;
    case WHERE_NONE:
        snprintf(buf, size, "none: SurfOS was not started from a stick GRUB named (no bootvol=); nothing is saved");
        break;
    case WHERE_NOT_FOUND:
        snprintf(buf, size, "none read: the stick SurfOS was started from was not found at startup%s%s",
                 v ? "; a change is saved on " : "", v ? v->name : "");
        break;
    case WHERE_FAILED:
        snprintf(buf, size, "none read from %s at startup: %s", where_vol, where_why);
        break;
    case WHERE_LOOKING:
        snprintf(buf, size, "not read yet: the stick SurfOS was started from is being looked for (20 s at most)");
        break;
    default:
        snprintf(buf, size, "%s/%s on %s, the stick SurfOS was started from%s%s", NETCFG_DIR, NETCFG_SETTINGS,
                 where_vol, had_settings ? "" : " (no such file yet: the defaults)",
                 where_readonly ? " (the flashed SURFOSUSB stick: only read)" : "");
        break;
    }
    mutex_unlock(&nm_lock);
}

/* key k of from into to (with its "set" bit) */
static void copy_key(struct netcfg_settings *to, const struct netcfg_settings *from, int k) {
    switch(k) {
    case NETCFG_TELNETD:      to->telnetd = from->telnetd; break;
    case NETCFG_TELNETD_PORT: to->telnetd_port = from->telnetd_port; break;
    case NETCFG_WIFI:         to->wifi = from->wifi; break;
    case NETCFG_ETHERNET:     to->ethernet_dhcp = from->ethernet_dhcp; break;
    case NETCFG_HOSTNAME:     strlcpy(to->hostname, from->hostname, sizeof(to->hostname)); break;
    case NETCFG_HISTORY:      to->history = from->history; break;
    case NETCFG_THEME:        to->theme = from->theme; break;      /* 11/05/26 (track desk): the desktop's keys */
    case NETCFG_POINTER:      to->pointer = from->pointer; break;
    }
    to->set = (to->set & ~(1u << k)) | (from->set & (1u << k));
}

int netman_set(int k, const char *value, char *why, size_t size) {
    struct netcfg_settings s;
    struct stick st;
    char *text = NULL, *out = NULL, path[VFS_PATH_MAX];
    u_int len = 0;
    bool apply;
    int r, n;
    mutex_lock(&nm_lock);
    s = cfg;
    mutex_unlock(&nm_lock);
    if(netcfg_set(&s, k, value, why, size)) return -1;
    text = (char *)kalloc(NETCFG_SETTINGS_MAX + 1);
    out = (char *)kalloc(NETCFG_SETTINGS_MAX + 1);
    mutex_lock(&stick_lock);                            /* the boot's read, when it runs, first */
    if(!text || !out) {
        snprintf(why, size, "no memory");
        r = -ENOMEM;
    } else if((r = stick_open(true, &st, why, size)) == 0) {
        if(!path_of(path, sizeof(path), st.root, NETCFG_SETTINGS)) {
            snprintf(why, size, "the path of %s/%s is too long", NETCFG_DIR, NETCFG_SETTINGS);
            r = -ENAMETOOLONG;
        } else if((r = read_file(path, text, NETCFG_SETTINGS_MAX, &len)) && r != -ENOENT) {
            snprintf(why, size, "%s/%s could not be read: %s", NETCFG_DIR, NETCFG_SETTINGS,
                     r == -EFBIG ? "it is longer than 8 KB" : strerror(r));
        } else {
            n = netcfg_edit_settings(r == -ENOENT ? NULL : text, len, k, value, out, NETCFG_SETTINGS_MAX + 1);
            if(n < 0) {
                snprintf(why, size, "%s/%s would be longer than 8 KB", NETCFG_DIR, NETCFG_SETTINGS);
                r = -EFBIG;
            } else {
                r = write_file(st.root, NETCFG_SETTINGS, out, (u_int)n, why, size);
            }
        }
        stick_close(&st);
    }
    /* in force, saved or not, but for telnetd and its port, which act at a start (so a change not saved changes
       nothing), and nothing under settings=off (saved for the next start only). Only this key: another command may
       change another meanwhile */
    apply = where_state != WHERE_OFF && !(r && (k == NETCFG_TELNETD || k == NETCFG_TELNETD_PORT));
    mutex_lock(&nm_lock);
    if(apply) {
        copy_key(&cfg, &s, k);
        cmd_set |= 1u << k;
        if(k == NETCFG_HOSTNAME) dhcp_set_hostname(s.hostname);    /* under the lock: the boot's never comes after */
    }
    if(!r) had_settings = true;
    mutex_unlock(&nm_lock);
    mutex_unlock(&stick_lock);
    kfree(text);
    kfree(out);
    if(apply && k == NETCFG_WIFI && s.wifi) start_keeper(false);
    return r ? 1 : 0;
}

/* 11/05/26 (the desktop applications, track settings): key k = value in force now, the stick not written: for a
   setting a person steps through (Settings' theme and pointer speed, which the desktop takes from here each second), so
   that each step acts at once and only the last is written, by netman_set() once the steps stop. 0; 1 when this start
   does not use the settings (settings=off), nothing changed; -1 refused (a value k does not take), why says why */
int netman_apply(int k, const char *value, char *why, size_t size) {
    struct netcfg_settings s;
    mutex_lock(&nm_lock);
    s = cfg;
    mutex_unlock(&nm_lock);
    if(netcfg_set(&s, k, value, why, size)) return -1;
    if(where_state == WHERE_OFF) {
        snprintf(why, size, "this start does not use the settings (settings=off)");
        return 1;
    }
    mutex_lock(&nm_lock);
    copy_key(&cfg, &s, k);
    cmd_set |= 1u << k;                                 /* (the boot's read, if it still comes, leaves it) */
    mutex_unlock(&nm_lock);
    return 0;
}

/**** 10/25/26 (track A, A10): surfsh's files ****/

static bool file_allowed(const char *name, bool write) {
    return !strcmp(name, NETCFG_HISTORY_FILE) || (!write && !strcmp(name, NETCFG_SURFSHRC));
}

bool netman_wait_boot(void) {
    u_int waited = 0;
    bool usb = driver_status("usb") == 0, gave_up;
    if(!boot_read_pending) return false;                /* nothing to read (settings=off, no bootvol=), or read */
    /* while the stick is still looked for: without a USB controller no disk comes any more (the smoke test names a
       volume that only its self test makes): not there now is never; with one the wait ends LOOK_GRACE_MS from the
       start */
    gave_up = !usb && !bdev_boot_vol();
    while(!gave_up && boot_read_pending && waited < BOOTVOL_WAIT_MS + 10000) {
        if(boot_looking && (!usb || uptime_ms() >= LOOK_GRACE_MS)) {
            gave_up = true;
            break;
        }
        sleep_ms(100);
        waited += 100;
    }
    if(waited || gave_up)
        klogf("+NET: %s (pid %lu) waited %u ms for the settings to be read%s\n", curTask ? curTask->name : "?",
              curTask ? curTask->pid : 0UL, waited, gave_up ? ", and went on without them: the stick is not there yet" :
              boot_read_pending ? ", and went on without them" : "");
    return gave_up && usb;
}

bool netman_history_on(void) {
    bool on;
    mutex_lock(&nm_lock);
    on = cfg.history && where_state != WHERE_OFF;
    mutex_unlock(&nm_lock);
    return on;
}

int netman_file_read(const char *name, char *buf, u_int max, u_int *len, u_int *gen, char *why, size_t wsize) {
    struct stick st;
    char path[VFS_PATH_MAX];
    int r;
    *len = 0;
    buf[0] = 0;
    if(gen) *gen = 0;
    if(!file_allowed(name, false)) {
        snprintf(why, wsize, "%s/%s is not one of surfsh's files", NETCFG_DIR, name);
        return -EACCES;
    }
    if(where_state == WHERE_OFF) {                      /* (the caller has waited for the boot: netman_wait_boot()) */
        snprintf(why, wsize, "this start does not use the stick's settings (settings=off)");
        return -ENOENT;
    }
    mutex_lock(&stick_lock);
    if(gen) *gen = history_gen;                         /* 10/25/26 (4): as this read finds it */
    r = stick_open(false, &st, why, wsize);
    if(!r) {
        if(!path_of(path, sizeof(path), st.root, name)) {
            snprintf(why, wsize, "the path of %s/%s is too long", NETCFG_DIR, name);
            r = -ENAMETOOLONG;
        } else if((r = read_file(path, buf, max, len)) != 0) {
            snprintf(why, wsize, "%s/%s could not be read: %s", NETCFG_DIR, name,
                     r == -EFBIG ? "it is too long" : strerror(r));
        }
        stick_close(&st);
    }
    mutex_unlock(&stick_lock);
    return r;
}

int netman_file_write(const char *name, const char *data, u_int len, u_int *gen, char *why, size_t wsize) {
    struct stick st;
    int r;
    if(!file_allowed(name, true)) {
        snprintf(why, wsize, "%s/%s is not written by SurfOS", NETCFG_DIR, name);
        return -EACCES;
    }
    if(!netman_history_on()) {
        snprintf(why, wsize, "the history setting is off (settings history on)");
        return -EACCES;
    }
    if(!netman_can_save()) {                            /* 10/25/26 (the programs' review, U4): not tried each time */
        snprintf(why, wsize, "the stick SurfOS was started from may not be written");
        return -EROFS;
    }
    mutex_lock(&stick_lock);
    if(gen && *gen != history_gen) {                    /* 10/25/26 (the kernel review, 4): saved since it was read */
        snprintf(why, wsize, "%s/%s was saved again after it was read", NETCFG_DIR, name);
        r = -EAGAIN;
    } else if(!(r = stick_open(true, &st, why, wsize))) {
        r = write_file(st.root, name, data, len, why, wsize);
        stick_close(&st);
        history_gen++;                                  /* written or not, what was read before may not be the file */
        if(!r && gen) *gen = history_gen;
    }
    mutex_unlock(&stick_lock);
    return r;
}

/**** the boot ****/

static void boot_note(void *arg, u_int line, const char *why) {
    if(line) kprintf("+NET: %s line %u: %s\n", (const char *)arg, line, why);
    else kprintf("+NET: %s: %s\n", (const char *)arg, why);
    boot_notes++;
}

static void load_at_boot(void) {
    struct netcfg_settings s;
    struct netcfg_net *list = NULL;
    struct netcfg_span *kept = NULL;
    struct bdev *v = NULL;
    struct stick st;
    char *text = NULL, path[VFS_PATH_MAX], why[96], a[8], b[8];
    u_int waited, len = 0;
    bool have_s = false, have_n = false;
    int n = 0, nk = 0, r, k;

    if(bootinfo_has_arg(&bootinfo, "settings=off")) {
        where_state = WHERE_OFF;
        kprintf("+NET: settings=off: the settings on the stick are not read at this start\n");
        return;
    }
    if(!bdev_boot_vol_named()) {
        where_state = WHERE_NONE;
        return;
    }
    where_state = WHERE_LOOKING;
    for(waited = 0; !(v = bdev_boot_vol()) && waited < BOOTVOL_WAIT_MS; waited += 100) {
        boot_looking = true;                            /* 10/25/26 (A10): not there at once (a USB stick comes later) */
        sleep_ms(100);
    }
    boot_looking = false;
    if(!v) {
        where_state = WHERE_NOT_FOUND;
        klogf("+NET: the volume GRUB was started from (bootvol=) was not found within %u s: no settings read\n",
              BOOTVOL_WAIT_MS / 1000);
        return;
    }
    text = (char *)kalloc(NETCFG_NETWORKS_MAX + 1);
    list = (struct netcfg_net *)kcalloc(NETCFG_NETS_MAX, sizeof(struct netcfg_net));
    kept = (struct netcfg_span *)kcalloc(NETCFG_SPANS_MAX, sizeof(struct netcfg_span));
    mutex_lock(&stick_lock);
    strlcpy(where_vol, v->name, sizeof(where_vol));
    where_readonly = bdev_mount_readonly(v);
    if(!text || !list || !kept) {
        strlcpy(where_why, "no memory", sizeof(where_why));
        where_state = WHERE_FAILED;
        mutex_unlock(&stick_lock);
        goto out;
    }
    r = stick_open(false, &st, why, sizeof(why));
    if(r) {
        strlcpy(where_why, why, sizeof(where_why));
        where_state = WHERE_FAILED;
        mutex_unlock(&stick_lock);
        kprintf("+NET: the settings on %s were not read: %s\n", v->name, why);
        goto out;
    }
    netcfg_defaults(&s);
    r = path_of(path, sizeof(path), st.root, NETCFG_SETTINGS) ? read_file(path, text, NETCFG_SETTINGS_MAX, &len) :
        -ENAMETOOLONG;
    if(r == 0) {
        have_s = true;
        netcfg_parse_settings(text, len, &s, boot_note, (void *)"settings.conf");
    } else if(r != -ENOENT) {
        kprintf("+NET: %s/%s on %s was not read: %s\n", NETCFG_DIR, NETCFG_SETTINGS, v->name,
                r == -EFBIG ? "it is longer than 8 KB" : strerror(r));
    }
    r = path_of(path, sizeof(path), st.root, NETCFG_NETWORKS) ? read_file(path, text, NETCFG_NETWORKS_MAX, &len) :
        -ENAMETOOLONG;
    if(r == 0) {
        have_n = true;
        n = netcfg_parse_networks(text, len, list, NETCFG_NETS_MAX, kept, &nk, boot_note, (void *)"networks.conf");
        make_keys(list, &n, boot_note, (void *)"networks.conf");
    } else if(r != -ENOENT) {
        kprintf("+NET: %s/%s on %s was not read: %s\n", NETCFG_DIR, NETCFG_NETWORKS, v->name,
                r == -EFBIG ? "it is longer than 16 KB" : strerror(r));
    }
    stick_close(&st);
    mutex_lock(&nm_lock);                               /* what a command set meanwhile stays */
    boot_cfg = s;
    for(k = 0; k < NETCFG_NKEYS; k++) if(!((cmd_set >> k) & 1)) copy_key(&cfg, &s, k);
    had_settings = have_s;
    had_networks = have_n;
    where_state = WHERE_READ;
    s = cfg;
    mutex_unlock(&nm_lock);
    set_saved(list, n, nk);
    mutex_unlock(&stick_lock);
    if(have_s || have_n)
        kprintf("+NET: settings from %s (%s/): telnetd %s%s%s, wifi %s, ethernet %s, hostname %s%s; %i saved Wi-Fi "
                "network%s%s\n", v->name, NETCFG_DIR, s.telnetd ? "on" : "off",
                s.telnetd && s.telnetd_port != 23 ? " port " : "",
                s.telnetd && s.telnetd_port != 23 ? netcfg_value(&s, NETCFG_TELNETD_PORT, a, sizeof(a)) : "",
                s.wifi ? "on" : "off", netcfg_value(&s, NETCFG_ETHERNET, b, sizeof(b)), s.hostname,
                s.history ? ", history on" : "", n, n == 1 ? "" : "s",       /* 10/25/26 (A10): said when it is on */
                boot_notes ? " (some lines were not understood: above)" : "");
    else
        klogf("+NET: no settings on %s (no %s/ folder there): the defaults\n", v->name, NETCFG_DIR);
out:
    if(text) {
        wipe(text, NETCFG_NETWORKS_MAX + 1);
        kfree(text);
    }
    if(list) {
        wipe(list, NETCFG_NETS_MAX * sizeof(struct netcfg_net));
        kfree(list);
    }
    kfree(kept);
}

/**** telnetd ****/

/* a user task named name that is not dead (its pid), or 0: with server, the running server only (detached, as
   telnetd itself finds it); else any, a telnetd on starting one or a telnetd status included. The list is read by
   index and a task collected meanwhile shifts the rest, so a search that finds none looks twice more, as telnetd
   does. 10/24/26: of any name (ftpd's too) */
static int task_running(const char *name, bool server) {
    struct procinfo pi;
    int i, k;
    for(k = 0; k < 3; k++)
        for(i = 0; task_info(i, &pi) == 0; i++)
            if((pi.flags & PROC_USER) && pi.state != PROC_DEAD && !strcmp(pi.name, name) &&
               (!server || (pi.flags & PROC_DETACHED)))
                return pi.pid;
    return 0;
}

static int telnetd_running(bool server) {
    return task_running("telnetd", server);
}

bool netman_telnet_exposed(void) {
    return telnetd_due || autonet_busy() || telnetd_running(false) > 0;    /* any telnetd: the safe side */
}

/* 10/24/26 (the network tools): ftpd has no logins either, and its clients write files: while one runs, the keeper
   joins no open network by itself, as for telnetd. Any ftpd process counts (the server, ftpd serve, an ftpd on
   starting one, an ftpd status, -r or not): the safe side */
bool netman_ftp_exposed(void) {
    return task_running("ftpd", false) > 0;
}

bool netman_trusted(struct netdev *dev) {
    struct wlan_station_info si;
    struct wlan_dev *w;
    if(!dev || !dev->ip || !dev->link_up) return false;
    if(!strncmp(dev->name, "eth", 3)) return true;
    w = wlan_find(dev->name);
    if(!w || w->netdev != dev || wlan_station(w, &si, false)) return false;
    return si.state == WLAN_STA_UP && (si.security == WLAN_SEC_WPA2_PSK || si.security == WLAN_SEC_WPA2_WPA3);
}

/* the first interface with an address telnetd may be started for: Ethernet first, as the routes prefer it */
static struct netdev *trusted_address(void) {
    struct netdev *d;
    for(d = netdev_first(); d; d = d->next) if(!strncmp(d->name, "eth", 3) && netman_trusted(d)) return d;
    for(d = netdev_first(); d; d = d->next) if(netman_trusted(d)) return d;
    return NULL;
}

/* telnetd on [port] as a process on F1, its lines prefixed who: its status; -1 when it could not be started, or did
   not finish in time (then *child its pid, for the caller to collect; with child NULL init collects it once the
   caller's thread has ended) */
static int start_telnetd(u16 port, const char *who, int *child) {
    char portword[8], *argv[] = { "telnetd", "on", portword, NULL };
    int argc = 2, pid, status = -1, waited, r;
    if(port && port != 23) {
        snprintf(portword, sizeof(portword), "%u", (u_int)port);
        argc = 3;
    }
    pid = process_spawn(TELNETD_PATH, argc, argv, &conArray[0], 0);
    if(pid < 0) {
        kprintf("%s: %s: %s\n", who, TELNETD_PATH, strerror(pid));
        return -1;
    }
    for(waited = 0; waited < TELNETD_DONE_MS; waited += 100) {
        r = task_wait_opts((u_long)pid, &status, WNOHANG);
        if(r == pid) return status;
        if(r < 0) break;
        sleep_ms(100);
    }
    kprintf("%s: telnetd on (pid %i) has not finished after %i s; not waiting for it\n", who, pid,
            TELNETD_DONE_MS / 1000);
    if(child) *child = pid;
    return -1;
}

int netman_start_telnetd(u16 port) {
    return start_telnetd(port, "+AUTONET", NULL);       /* autonet's thread ends: init collects a slow one */
}

/**** Ethernet and the leases (the netman thread's own: nothing else touches ifs) ****/

struct ifstate {
    struct netdev *dev;
    bool seen, was_up, confirm;         /* confirm: its link came back with its lease: the address asked for again */
    u_long down_lease;                  /* the lease (its lease_at) it had when its link went; 0 */
    u_long next_dhcp;                   /* ticks */
    u_int fails;
    u_long renew_try, renew_for;        /* the last renewal tried, for the lease granted at renew_for */
};

static struct ifstate ifs[MAX_IFS];

static struct ifstate *state_of(struct netdev *d) {
    int i;
    for(i = 0; i < MAX_IFS; i++) if(ifs[i].dev == d) return &ifs[i];
    for(i = 0; i < MAX_IFS; i++) {
        if(!ifs[i].dev) {
            ifs[i].dev = d;
            return &ifs[i];
        }
    }
    return NULL;
}

/* the wait before DHCP try fails + 1: DHCP_RETRY_MS, doubling to DHCP_RETRY_MAX */
static u_long retry_wait(u_int fails) {
    u_long wait = DHCP_RETRY_MS << (fails > 5 ? 4 : fails ? fails - 1 : 0);
    return wait > DHCP_RETRY_MAX ? DHCP_RETRY_MAX : wait;
}

static void ethernet_step(void) {
    struct netdev *d;
    bool on;
    u_long now = getticks();
    mutex_lock(&nm_lock);
    on = cfg.ethernet_dhcp;
    mutex_unlock(&nm_lock);
    for(d = netdev_first(); d; d = d->next) {
        struct ifstate *st;
        bool up = d->link_up;
        int r;
        if(strncmp(d->name, "eth", 3) || !(st = state_of(d))) continue;
        if(!st->seen) {                                 /* first seen: no edge, a link that is up is tried at once */
            st->seen = true;
            st->was_up = up;
            st->next_dhcp = now;
        } else if(up && !st->was_up) {                  /* a link came up: try at once */
            st->next_dhcp = now;
            st->fails = 0;
            st->confirm = d->ip && d->lease_s && st->down_lease && d->lease_at == st->down_lease;   /* still ours? */
        } else if(!up && st->was_up) {
            st->down_lease = d->ip && d->lease_s ? d->lease_at : 0;
        }
        st->was_up = up;
        if(!up || autonet_busy()) continue;
        if(st->confirm) {
            st->confirm = false;
            r = dhcp_renew(d, DHCP_REBOOTING, RENEW_MS, d->lease_at, true);
            if(r == -1) klogf("+NET: %s: no answer to confirming its address; it is kept, and renewed as usual\n",
                              d->name);
            if(r == -2 && !on) kprintf("+NET: %s: its address was not this network's (dhcp %s gets one)\n", d->name,
                                       d->name);
        }
        if(!on || d->ip || (long)(now - st->next_dhcp) < 0) continue;
        if(st->fails) klogf("+NET: %s: asking for an address again (DHCP)\n", d->name);
        else kprintf("+NET: %s: link up; asking for an address (DHCP)\n", d->name);
        if(dhcp_configure_quiet(d, DHCP_MS, st->fails > 0) == 0) {
            st->fails = 0;
            continue;
        }
        st->fails++;
        st->next_dhcp = ticks_after(retry_wait(st->fails));
        if(st->fails == 1) kprintf("+NET: %s: no lease; asking again in %lu s, then less often\n", d->name,
                                   retry_wait(1) / 1000);
        else klogf("+NET: %s: still no lease; the next try in %lu s\n", d->name, retry_wait(st->fails) / 1000);
    }
}

static void lease_step(void) {
    struct netdev *d;
    for(d = netdev_first(); d; d = d->next) {
        struct ifstate *st;
        u_long at;
        u32 lease;
        ipaddr_t ip;
        int step, r;
        mutex_lock(&net_lock);
        lease = d->lease_s;
        at = d->lease_at;
        ip = d->ip;
        mutex_unlock(&net_lock);
        if(!lease || !ip || !(st = state_of(d))) continue;
        if(st->renew_for != at) {
            st->renew_for = at;
            st->renew_try = 0;
        }
        step = netcfg_lease_step(lease, at, st->renew_try, getticks(), HZ);
        if(step == NETCFG_LEASE_WAIT) continue;
        if(step == NETCFG_LEASE_EXPIRED) {
            dhcp_drop(d, "its lease ran out", at);
            continue;
        }
        if(!d->link_up) continue;                       /* nothing to ask through; when the link is back */
        st->renew_try = getticks() ? getticks() : 1;
        r = dhcp_renew(d, step == NETCFG_LEASE_REBIND ? DHCP_REBINDING : DHCP_RENEWING, RENEW_MS, at, true);
        if(r == -1) klogf("+NET: %s: no answer to renewing its lease (%s); tried again later\n", d->name,
                          step == NETCFG_LEASE_REBIND ? "any server" : "the server that granted it");
    }
}

/**** Wi-Fi ****/

static bool sec_fits(u8 saved_sec, u8 bss_sec) {
    if(saved_sec == NETCFG_SEC_OPEN) return bss_sec == WLAN_SEC_OPEN;
    return bss_sec == WLAN_SEC_WPA2_PSK || bss_sec == WLAN_SEC_WPA2_WPA3;
}

static bool refused(const struct saved *s, u_long now) {
    return s->refused_until && (long)(s->refused_until - now) > 0;
}

/* the saved network named so: refused once more (1: WIFI_REFUSALS times in a row, so it is set aside for a while, 2:
   and that is the first time it is said), or joined (refused false: the counts start again) */
static int note_refusal(const u8 *ssid, u_int len, bool refused) {
    int aside = 0, i;
    mutex_lock(&nm_lock);
    for(i = 0; i < nsaved; i++) {
        struct saved *s = &saved[i];
        if(s->net.ssid_len != len || memcmp(s->net.ssid, ssid, len)) continue;
        if(!refused) {
            s->refusals = 0;
            s->told = false;
            continue;
        }
        if(++s->refusals >= WIFI_REFUSALS) {
            s->refusals = 0;
            s->refused_until = ticks_after(WIFI_REFUSED_MS);
            aside = s->told ? 1 : 2;
            s->told = true;
        }
    }
    mutex_unlock(&nm_lock);
    return aside;
}

/* join the saved network s (cmd, quiet: say()), unless a wifi leave or down came since stops; its join request's
   number into *join */
static int join_saved(struct wlan_dev *w, const struct saved *s, bool cmd, bool quiet, u_int stops, u_int *join) {
    char name[4 * NETCFG_SSID_MAX + 1];
    int r = wlan_join_unless(w, s->net.ssid, s->net.ssid_len, s->net.security == NETCFG_SEC_WPA2 ? s->net.psk : NULL,
                             stops, quiet && !cmd, join), aside;
    ssid_str(s->net.ssid, s->net.ssid_len, name, sizeof(name));
    if(r == -EACCES) {
        if((aside = note_refusal(s->net.ssid, s->net.ssid_len, true)) != 0)
            say(cmd, aside == 1, "%s: %s refused its saved key %u times in a row: has its password changed, or "
                "was it mistyped when it was saved? (wifi join %s, then wifi save; it is not tried again for %u minutes)",
                w->name, name, WIFI_REFUSALS, name, WIFI_REFUSED_MS / 60000);
        else
            say(cmd, quiet, "%s: joining %s failed: its 4-way handshake did not finish (the key, or a frame lost)",
                w->name, name);
    } else if(r == 0) {
        note_refusal(s->net.ssid, s->net.ssid_len, false);
    } else if(r != -ENOENT && r != -EINTR && r != -EBUSY && r != -ECANCELED) {
        say(cmd, quiet, "%s: joining %s failed: %s", w->name, name, r == -ETIMEDOUT ? "no answer" : strerror(r));
    }
    return r;
}

/* the address for the association just made (again: the keeper asking once more): 0 when there is one, -EINTR when
   the command was cut short */
static int get_address(struct wlan_dev *w, const char *name, bool cmd, bool again) {
    struct netdev *nd = w->netdev;
    int i;
    if(again) say(cmd, true, "%s: asking for an address on %s again (DHCP)", w->name, name);
    else say(cmd, false, "%s: joined %s; asking for an address (DHCP)", w->name, name);
    for(i = 0; i < DHCP_TRIES; i++) {
        if(i) sleep_ms(DHCP_PAUSE_MS);
        if(cmd && task_interrupted()) return -EINTR;
        if(!nd->link_up) {
            say(cmd, again, "%s: the link to %s went before an address came", w->name, name);
            return -1;
        }
        if(nd->ip) return 0;                            /* a dhcp typed meanwhile got one */
        if(dhcp_configure_quiet(nd, DHCP_MS, again) == 0) return 0;
    }
    return cmd && task_interrupted() ? -EINTR : -1;
}

/* DHCP for the manager's association failed (once more): the next try later, when the keeper is there to make it
   (wifi = on) */
static void address_later(struct wlan_dev *w, const char *name, bool cmd) {
    u_int fails = ++keeper_dhcp_fails;
    bool on;
    mutex_lock(&nm_lock);
    on = cfg.wifi;
    mutex_unlock(&nm_lock);
    keeper_dhcp_next = ticks_after(retry_wait(fails));
    if(!on)
        say(cmd, false, "%s: no address on %s after %u tries (dhcp %s asks again)", w->name, name, DHCP_TRIES, w->name);
    else if(fails == 1)
        say(cmd, false, "%s: no address on %s after %u tries; asking again in %lu s, then less often (dhcp %s asks "
            "now)", w->name, name, DHCP_TRIES, retry_wait(1) / 1000, w->name);
    else
        klogf("+NET: %s: still no address on %s; the next try in %lu s\n", w->name, name, retry_wait(fails) / 1000);
}

/* join the first saved network in range and get its address, said on the command's console (cmd) or by "+NET:" lines
   (quiet: a keeper's repeat, in the log only). stops: the station's count of wifi leave and wifi down when the connect
   began: one typed since calls it off (-ECANCELED). 0 joined (an address or not), -ENOENT none to join in range,
   -EBUSY, -EINTR, or why not */
static int connect_saved(struct wlan_dev *w, bool cmd, bool quiet, u_int stops) {
    struct saved *list = (struct saved *)kcalloc(NETCFG_NETS_MAX, sizeof(struct saved));
    struct wlan_bss *bss = (struct wlan_bss *)kalloc(BSS_MAX * sizeof(struct wlan_bss));
    bool *heard = (bool *)kcalloc(NETCFG_NETS_MAX, sizeof(bool)), exposed, ftp;
    char name[4 * NETCFG_SSID_MAX + 1];
    u_long now, scanned;
    u_int join = 0;
    int n, nb, i, j, r = -ENOMEM, pick = -1, tried = 0, skipped = 0;
    if(!list || !bss || !heard) goto out;
    mutex_lock(&nm_lock);
    n = nsaved;
    memcpy(list, saved, n * sizeof(struct saved));
    mutex_unlock(&nm_lock);
    if(!n) {
        if(where_state == WHERE_OFF) say(cmd, quiet, "the saved Wi-Fi networks are not read at this start (settings=off)");
        else say(cmd, quiet, "no saved Wi-Fi networks (wifi save keeps the one %s is joined to)", w->name);
        r = -ENOENT;
        goto out;
    }
    if(!w->up) {
        say(cmd, quiet, "%s: starting the Wi-Fi card to join a saved network", w->name);
        r = wlan_up_unless(w, stops);
        if(r) {
            if(r != -EBUSY && r != -EINTR && r != -ECANCELED) say(cmd, quiet, "%s: the card did not start: %s", w->name,
                                                                  strerror(r));
            goto out;
        }
    }
    scanned = getticks();
    r = wlan_scan(w, WIFI_SCAN_MS);
    if(r && r != -ETIMEDOUT) {
        struct wlan_station_info si;
        if(wlan_station(w, &si, false) == 0 && si.stops != stops) r = -ECANCELED;     /* a leave or down meanwhile */
        else if(r != -EBUSY && r != -EINTR) say(cmd, quiet, "%s: the scan failed: %s", w->name, strerror(r));
        goto out;
    }
    nb = (int)wlan_bss_list(w, bss, BSS_MAX);
    now = getticks();
    exposed = netman_telnet_exposed();
    ftp = netman_ftp_exposed();                         /* 10/24/26 */
    for(i = 0; i < n && pick < 0; i++) {                /* the saved ones in their order, as heard */
        for(j = 0; j < nb; j++) {                       /* heard in this scan: the list keeps a network for a minute */
            if(bss[j].ssid_len == list[i].net.ssid_len && !memcmp(bss[j].ssid, list[i].net.ssid, bss[j].ssid_len) &&
               sec_fits(list[i].net.security, bss[j].security) && (long)(bss[j].seen - scanned) >= 0)
                break;
        }
        if(j == nb) continue;
        heard[i] = true;
        ssid_str(list[i].net.ssid, list[i].net.ssid_len, name, sizeof(name));
        if(refused(&list[i], now)) {
            say(cmd, quiet, "%s: %s is in range, but its saved key was refused (not tried again for %lu more minutes; "
                "wifi join %s, then wifi save)", w->name, name,
                ((list[i].refused_until - now) / HZ + 59) / 60, name);
            skipped++;
            continue;
        }
        if(list[i].net.security == NETCFG_SEC_OPEN && (exposed || (exposed = netman_telnet_exposed()))) {
            say(cmd, quiet, "%s: %s is in range, but it is open and a telnet server runs or is to start: not joined "
                "by itself (wifi join %s joins it)", w->name, name, name);
            skipped++;
            continue;
        }
        if(list[i].net.security == NETCFG_SEC_OPEN && (ftp || (ftp = netman_ftp_exposed()))) {
            say(cmd, quiet, "%s: %s is in range, but it is open and an FTP server (ftpd) runs: not joined by itself "
                "(wifi join %s joins it)", w->name, name, name);
            skipped++;
            continue;
        }
        say(cmd, quiet, "%s: %s is in range (%d dBm): joining it", w->name, name, bss[j].rssi);
        tried++;
        r = join_saved(w, &list[i], cmd, quiet, stops, &join);
        if(r == 0) pick = i;
        if(r == -EINTR || r == -EBUSY || r == -ECANCELED) goto out;
    }
    for(i = 0; i < n && pick < 0; i++) {                /* then the hidden ones not heard, by a probe for their names */
        if(!list[i].net.hidden || heard[i]) continue;
        ssid_str(list[i].net.ssid, list[i].net.ssid_len, name, sizeof(name));
        if(refused(&list[i], now)) {
            say(cmd, quiet, "%s: the hidden network %s is not looked for: its saved key was refused (for %lu more "
                "minutes; wifi join %s, then wifi save)", w->name, name, ((list[i].refused_until - now) / HZ + 59) / 60,
                name);
            skipped++;
            continue;
        }
        if(list[i].net.security == NETCFG_SEC_OPEN && (exposed || (exposed = netman_telnet_exposed()))) {
            say(cmd, quiet, "%s: the hidden network %s is open and a telnet server runs or is to start: not looked for "
                "by itself (wifi join %s joins it)", w->name, name, name);
            skipped++;
            continue;
        }
        if(list[i].net.security == NETCFG_SEC_OPEN && (ftp || (ftp = netman_ftp_exposed()))) {
            say(cmd, quiet, "%s: the hidden network %s is open and an FTP server (ftpd) runs: not looked for by itself "
                "(wifi join %s joins it)", w->name, name, name);
            skipped++;
            continue;
        }
        say(cmd, quiet, "%s: looking for the hidden network %s", w->name, name);
        tried++;
        r = join_saved(w, &list[i], cmd, quiet, stops, &join);
        if(r == 0) pick = i;
        if(r == -EINTR || r == -EBUSY || r == -ECANCELED) goto out;
    }
    if(pick < 0) {
        if(!tried && !skipped) {
            if(n == 1) say(cmd, quiet, "%s: %s, the saved network, is not in range", w->name,
                           ssid_str(list[0].net.ssid, list[0].net.ssid_len, name, sizeof(name)));
            else say(cmd, quiet, "%s: none of the %i saved networks is in range", w->name, n);
        }
        r = -ENOENT;
        goto out;
    }
    keeper_join = join;                                 /* the association is the manager's: its address too */
    keeper_owns = true;
    keeper_dhcp_fails = 0;
    keeper_dhcp_next = 0;
    ssid_str(list[pick].net.ssid, list[pick].net.ssid_len, name, sizeof(name));
    if((r = get_address(w, name, cmd, false)) != 0 && r != -EINTR) address_later(w, name, cmd);
    r = 0;
out:
    if(list) {
        wipe(list, NETCFG_NETS_MAX * sizeof(struct saved));
        kfree(list);
    }
    kfree(bss);
    kfree(heard);
    return r;
}

int netman_wifi_connect(struct wlan_dev *w) {
    struct wlan_station_info si;
    int r;
    if(wlan_station(w, &si, false)) return -ENODEV;
    if(!mutex_trylock(&connect_lock)) return -EBUSY;
    keeper_connecting = true;
    r = connect_saved(w, true, false, si.stops);
    keeper_connecting = false;
    mutex_unlock(&connect_lock);
    if(r != -EINTR && r != -EBUSY && r != -ECANCELED && where_state != WHERE_OFF) {
        keeper_resume_stops = si.stops;                 /* before keeper_resume, which the keeper reads first */
        keeper_resume = true;                           /* what the start does: the keeper looks by itself again */
        start_keeper(false);
    }
    return r;
}

/* 11/05/26 (the desktop applications, track settings): the same for a program's job (kernel/netctl.c), whose thread has
   no console of its own: what happens is said as the keeper says it, "+NET:" lines in the kernel log (the address's
   first line on F1 too, as the keeper's), never as a command's lines on the caller's console */
int netman_wifi_connect_quiet(struct wlan_dev *w) {
    struct wlan_station_info si;
    int r;
    if(wlan_station(w, &si, false)) return -ENODEV;
    if(!mutex_trylock(&connect_lock)) return -EBUSY;
    keeper_connecting = true;
    r = connect_saved(w, false, true, si.stops);
    keeper_connecting = false;
    mutex_unlock(&connect_lock);
    if(r != -EINTR && r != -EBUSY && r != -ECANCELED && where_state != WHERE_OFF) {
        keeper_resume_stops = si.stops;
        keeper_resume = true;
        start_keeper(false);
    }
    return r;
}

/* the manager's association, without an address: DHCP again when it is time (keeper_dhcp_next, 0: at once) */
static void keep_address(struct wlan_dev *w, const struct wlan_station_info *si) {
    char name[4 * NETCFG_SSID_MAX + 1];
    u_long next = keeper_dhcp_next;
    if(!keeper_owns || si->join_requests != keeper_join || keeper_connecting) return;
    if(w->netdev->ip) {
        keeper_dhcp_fails = 0;
        keeper_dhcp_next = 0;
        return;
    }
    if(next && (long)(getticks() - next) < 0) return;
    if(!mutex_trylock(&connect_lock)) return;
    ssid_str(si->ssid, si->ssid_len, name, sizeof(name));
    if(get_address(w, name, false, true)) address_later(w, name, false);
    else keeper_dhcp_fails = 0;
    mutex_unlock(&connect_lock);
}

/* the keeper: the boot's join, then a saved network again whenever the station gives one up */
static void wifiman_main(void *arg) {
    struct wlan_dev *w = (struct wlan_dev *)arg;
    struct wlan_station_info si;
    u_long next = getticks(), wait;
    u_int tries = 0, last_requests, last_stops;
    int last_state = -1, last_idle = -1, n, r;
    bool retrying = keeper_at_boot, on;                 /* a keeper started later (wifi save) waits for a loss */
    if(wlan_station(w, &si, false)) goto gone;
    last_requests = si.join_requests;
    last_stops = si.stops;
    for(;;) {
        mutex_lock(&nm_lock);
        on = cfg.wifi;
        n = nsaved;
        mutex_unlock(&nm_lock);
        if(wlan_station(w, &si, false)) break;
        if(si.stops != last_stops) {                    /* wifi leave or wifi down: the owner's, until a loss */
            retrying = false;
            last_stops = si.stops;
        }
        if(keeper_resume) {                             /* wifi connect: the keeper's again */
            keeper_resume = false;
            /* 11/05/26 (Settings' review, M1): unless a wifi leave or down came since the connect (the leave right
               after a card's start from a window, say), before this look: that is the owner's, as above */
            if(si.stops == keeper_resume_stops && si.state != WLAN_STA_UP && si.state != WLAN_STA_JOINING &&
               si.state != WLAN_STA_RECONNECTING) {
                retrying = true;
                tries = 1;
                next = ticks_after(WIFI_RETRY_MS);
            }
        }
        if(!on || !n) {
            retrying = false;
        } else if(si.state == WLAN_STA_UP) {
            retrying = false;
            tries = 0;
            keep_address(w, &si);
        } else if(si.state == WLAN_STA_IDLE && si.idle == WLAN_IDLE_GAVE_UP &&
                  (last_state != WLAN_STA_IDLE || last_idle != WLAN_IDLE_GAVE_UP || si.join_requests != last_requests)) {
            if(!retrying) {
                char name[4 * NETCFG_SSID_MAX + 1];
                kprintf("+NET: %s: %s is gone: looking for a saved network\n", w->name,
                        ssid_str(si.ssid, si.ssid_len, name, sizeof(name)));
            }
            retrying = true;
            tries = 0;
            next = getticks();
        }
        last_state = si.state;
        last_idle = si.idle;
        last_requests = si.join_requests;
        if(retrying && (long)(getticks() - next) >= 0) {
            if(mutex_trylock(&connect_lock)) {
                keeper_connecting = true;
                r = connect_saved(w, false, tries > 0, si.stops);
                keeper_connecting = false;
                mutex_unlock(&connect_lock);
            } else {
                r = -EBUSY;
            }
            if(r == 0) {
                retrying = false;
                tries = 0;
            } else if(r == -ECANCELED) {
                retrying = false;                       /* the owner's wifi leave or down came meanwhile */
            } else {
                if(r != -EBUSY) tries++;
                wait = r == -EBUSY ? WIFI_BUSY_MS : tries < WIFI_LATE_TRIES ? WIFI_RETRY_MS : WIFI_RETRY_LATE;
                if(tries == 1 && r != -EBUSY)
                    kprintf("+NET: %s: no saved network joined; looking again every %lu s (in the log: dmesg)\n",
                            w->name, wait / 1000);
                else if(tries == WIFI_LATE_TRIES && r != -EBUSY)
                    klogf("+NET: %s: no saved network joined after %u tries; looking every %lu s from now on\n",
                          w->name, WIFI_LATE_TRIES, (u_long)(WIFI_RETRY_LATE / 1000));
                next = ticks_after(wait);
            }
        }
        sleep_ms(1000);
    }
gone:
    kprintf("+NET: %s: the Wi-Fi keeper stops (the device is gone)\n", w->name);
    wifiman = NULL;
}

/* the keeper, once there is something to keep: wifi = on, a saved network, a Wi-Fi device. at_boot: it joins a
   saved network at once; started later (the first wifi save, settings wifi on), it does what it does after the boot */
static void start_keeper(bool at_boot) {
    struct wlan_dev *w;
    bool on;
    int n;
    mutex_lock(&nm_lock);
    on = cfg.wifi;
    n = nsaved;
    mutex_unlock(&nm_lock);
    if(!on || !n) return;
    mutex_lock(&keeper_lock);
    if(!wifiman) {
        w = wlan_find(NULL);
        if(!w) {
            klogf("+NET: no Wi-Fi device: the saved networks are not joined\n");
        } else {
            keeper_at_boot = at_boot;
            wifiman = kthread_create_on("wifiman", &conArray[0], wifiman_main, w, PL_NORMAL, TF_DETACHED);
            if(!wifiman) kprintf("+NET: no memory for the Wi-Fi keeper: wifi connect joins a saved network by hand\n");
        }
    }
    mutex_unlock(&keeper_lock);
}

/**** the thread ****/

static void netman_main(void *arg) {
    struct netdev *d;
    char a[16];
    int child = 0, status;
    bool read;
    u16 port;
    netcfg_defaults(&boot_cfg);
    load_at_boot();
    boot_read_pending = false;                          /* 10/25/26 (A10): surfsh's files may be read now */
    mutex_lock(&nm_lock);
    read = where_state == WHERE_READ;
    telnetd_due = read && boot_cfg.telnetd;             /* as the file said at boot: a later change is for the next */
    port = boot_cfg.telnetd_port;
    if(read && !(cmd_set & (1u << NETCFG_HOSTNAME))) dhcp_set_hostname(cfg.hostname);  /* under the lock, as a command's */
    mutex_unlock(&nm_lock);
    if(where_state != WHERE_OFF) start_keeper(true);
    for(;;) {
        ethernet_step();
        lease_step();
        if(telnetd_due && !autonet_busy()) {            /* autonet first: it starts one on 23 for Ethernet */
            if(telnetd_running(true)) {
                telnetd_due = false;                    /* a server runs already (autonet's, or typed) */
            } else if(telnetd_running(false)) {
                ;                                       /* a telnetd command at work (on, status): look again */
            } else if((d = trusted_address()) != NULL) {
                kprintf("+NET: %s: %s; starting telnetd on F1 (the settings: telnetd = on)\n", d->name, ipfmt(d->ip, a));
                start_telnetd(port, "+NET", &child);
                telnetd_due = false;                    /* only now: until the server runs, it is due */
            }
        }
        if(child && task_wait_opts((u_long)child, &status, WNOHANG) != 0) child = 0;   /* a slow one, collected */
        sleep_ms(telnetd_due ? 250 : 1000);
    }
}

void init_netman(void) {
    netcfg_defaults(&cfg);
    /* 10/25/26 (A10): before any shell can ask, and only when there is something to read: a reader of surfsh's files
       then waits for the boot's read (netman_wait_boot()), else never */
    boot_read_pending = !bootinfo_has_arg(&bootinfo, "settings=off") && bdev_boot_vol_named();
    if(!kthread_create_on("netman", &conArray[0], netman_main, NULL, PL_NORMAL, TF_DETACHED)) {
        boot_read_pending = false;
        kprintf("+NET: no memory for the network manager: no settings from the stick\n");
    }
}
