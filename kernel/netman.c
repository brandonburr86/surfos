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
/.netman, or used where the owner has mounted it; nothing stays mounted by itself. It is
written only when it is the one-stick volume that may be written (bdev_boot_volume); a flashed
SURFOSUSB stick is read, never written. Without bootvol=, or with the kernel word
settings=off (GRUB's entry "without the settings on the stick"), nothing is read at boot.

A write goes to <name>.new, which is synced, renamed over the file and read back; settings.conf
is edited line by line (the owner's comments stay), networks.conf written anew. Every change
reads the file from the stick first, so a change never loses what another computer added.

At boot (the netman thread, on F1): the files are read once the volume is found (20 seconds
at most), each line that is not understood said with its number; then
* wifi = on with saved networks: the Wi-Fi keeper (the wifiman thread) brings the card up,
  scans, joins the first saved network in range (a hidden one by a probe for its name, when
  none is seen) with its key, and asks for an address. Afterwards it stays out of the way of
  the wifi command: while wlan0 is joined, joining or reconnecting it does nothing; wifi leave
  and wifi down are left alone; only when the station gives up a network (reconnects run out)
  does it look for a saved one again, every 30 seconds, every 2 minutes after 20 tries. A key
  the network refuses is not tried for 10 minutes. It asks for an address only for an
  association of its own (the station's join_requests at its join), and wifi connect does
  what the boot does on the command's console.
* ethernet = dhcp: an Ethernet interface whose link comes up gets a lease (30 s, then longer,
  between tries without one), unless autonet is still at it.
* telnetd = on: telnetd on [port] on F1 once an interface has an address, or after 60
  seconds without one; the kernel word autonet starts it itself (kernel/autonet.c).
* Every lease (wherever it came from) is renewed: at half its time from the server that
  granted it, at seven eighths from any server, given up when it runs out or is refused
  (netcfg_lease_step(), dhcp_renew()).
The setting hostname goes into every DHCP request (dhcp_set_hostname()).

Keys. A saved network's key is the PMK (wifi save takes the one the station joined with,
wlan_station(); a password typed into networks.conf by hand is turned into it here). Keys are
kept in RAM for the keeper and wiped when a network is forgotten; nothing logs one, and
netman_networks() gives the commands everything but the key.

Locks: nm_lock around the settings and the networks in RAM (never held across a wait),
stick_lock around a mount of the stick (one at a time), connect_lock around a connect (the
keeper's or wifi connect's).
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
#include <mm/kalloc.h>
#include <sys/bdev.h>
#include <fs/vfs.h>
#include <net/net.h>
#include <net/dhcp.h>
#include <net/wlan.h>
#include <net/wlan_cmd.h>
#include <net/wpa.h>
#include <net/ieee80211.h>
#include <net/netcfg.h>
#include <net/netman.h>
#include <blibc_common.h>

#define BOOTVOL_WAIT_MS  20000          /* how long the stick may take to be found at boot */
#define TELNETD_WAIT_MS  60000          /* telnetd = on: once there is an address, or after this */
#define TELNETD_PATH     "/initrd/bin/telnetd"
#define TELNETD_DONE_MS  30000          /* telnetd on returns once its server runs (3 s at most) */
#define NM_MOUNT         "/.netman"     /* the short mount of the stick */
#define VIEWS            16
#define WIFI_SCAN_MS     10000
#define WIFI_RETRY_MS    30000          /* the keeper's next look when no saved network was joined */
#define WIFI_RETRY_LATE  120000         /* after WIFI_LATE_TRIES of them */
#define WIFI_LATE_TRIES  20
#define WIFI_BUSY_MS     5000           /* a command had the card: soon again */
#define WIFI_REFUSED_MS  600000         /* a saved key the network refused: not tried for so long */
#define BSS_MAX          48
#define DHCP_TRIES       3
#define DHCP_MS          8000
#define DHCP_PAUSE_MS    2000
#define RENEW_MS         5000
#define ETH_RETRY_MS     30000          /* an Ethernet link without a lease: the next DHCP, doubling to ETH_RETRY_MAX */
#define ETH_RETRY_MAX    300000
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
    u_long refused_until;               /* ticks: its key was refused; 0 */
};

static mutex_t nm_lock = MUTEX_INIT("netman");
static struct netcfg_settings cfg;
static struct saved *saved;             /* NETCFG_NETS_MAX of them */
static int nsaved;
static int where_state;
static char where_vol[BDEV_NAME_LEN], where_why[96];
static bool had_settings, had_networks, where_readonly;
static u_int boot_notes;

static mutex_t stick_lock = MUTEX_INIT("netman-stick");
static mutex_t connect_lock = MUTEX_INIT("wifi-connect");
static mutex_t keeper_lock = MUTEX_INIT("wifiman");    /* start_keeper(): one keeper */
static surf_task *volatile wifiman;
static struct wlan_dev *volatile keeper_dev;
static volatile bool keeper_connecting, keeper_owns;
static volatile u_int keeper_requests;  /* the station's join_requests after the keeper's (or wifi connect's) join */
static volatile u_int keeper_dhcp_for;  /* the station's joins count whose address was last asked for */
static volatile bool keeper_at_boot;    /* the keeper joins at once (the boot's); else only once a network is lost */

/* not optimised away */
static void wipe(void *p, size_t n) {
    volatile u8 *q = (volatile u8 *)p;
    while(n--) *q++ = 0;
}

/* a line for the kernel log only (dmesg), not the screen */
static void klogf(const char *fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    klog_write(buf);
}

/* what a connect does, on the command's console (cmd), else "+NET:" on F1 and in the log */
static void say(bool cmd, const char *fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if(cmd) printf("    %s\n", buf);
    else kprintf("+NET: %s\n", buf);
}

static const char *ssid_str(const u8 *ssid, u_int len, char *buf, size_t size) {
    return ieee80211_ssid_text(ssid, len, false, buf, size);
}

/**** the stick ****/

/* the volume the settings are on, mounted for a moment (or where the owner mounted it): its path into root, and
   *ours when the mount is this one's. 0, or -errno and why */
static int stick_open(bool write, char *root, size_t rsize, bool *ours, char *why, size_t wsize) {
    struct bdev *v = bdev_boot_vol();
    struct vfs_mount_view *views;
    int n, i, r;
    *ours = false;
    if(!v) {
        snprintf(why, wsize, "%s", bdev_boot_vol_named() ? "the stick SurfOS was started from is not there (lsblk)" :
                 "SurfOS was not started from a stick GRUB named (no bootvol= on the kernel command line)");
        return -ENODEV;
    }
    if(v->gone) {
        snprintf(why, wsize, "%s, the stick SurfOS was started from, is unplugged", v->name);
        return -ENODEV;
    }
    if(write && v->protect != bdev_boot_volume) {
        snprintf(why, wsize, "SurfOS was started from the flashed SURFOSUSB stick (%s), which it only reads", v->name);
        return -EROFS;
    }
    if(write && (v->readonly || bdev_disk(v)->readonly)) {
        snprintf(why, wsize, "%s is write-protected (its switch)", v->name);
        return -EROFS;
    }
    views = (struct vfs_mount_view *)kalloc(VIEWS * sizeof(*views));
    if(!views) {
        snprintf(why, wsize, "no memory");
        return -ENOMEM;
    }
    n = vfs_mount_views(views, VIEWS);
    for(i = 0; i < n && i < VIEWS; i++) {
        if(views[i].dev != v) continue;
        if(write && views[i].readonly) {
            snprintf(why, wsize, "%s is mounted read-only on %s: umount %s, then try again", v->name, views[i].path,
                     views[i].path);
            kfree(views);
            return -EROFS;
        }
        strlcpy(root, views[i].path, rsize);
        kfree(views);
        return 0;
    }
    kfree(views);
    r = vfs_mount_flags(v, NM_MOUNT, NULL, MNT_QUIET | (write ? 0 : MNT_RDONLY));
    if(r) {
        snprintf(why, wsize, "%s could not be mounted: %s", v->name, strerror(r));
        return r;
    }
    strlcpy(root, NM_MOUNT, rsize);
    *ours = true;
    return 0;
}

/* the short mount goes again (a command that has a file open there for a moment is waited for); the owner's own
   mount stays, its writes synced */
static void stick_close(bool ours) {
    int r = 0, tries;
    if(!ours) {
        vfs_sync();
        return;
    }
    for(tries = 0; tries < 40; tries++) {
        r = vfs_umount(NM_MOUNT);
        if(r != -EBUSY) break;
        sleep_ms(50);
    }
    if(r && r != -ENOENT)                               /* ENOENT: gone already (an eject meanwhile) */
        kprintf("+NET: the stick stays mounted on %s: %s (umount %s)\n", NM_MOUNT, strerror(r), NM_MOUNT);
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
    snprintf(dir, sizeof(dir), "%s/%s", root, NETCFG_DIR);
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    snprintf(tmp, sizeof(tmp), "%s/%s.new", dir, name);
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

static void path_of(char *out, size_t size, const char *root, const char *name) {
    snprintf(out, size, "%s/%s/%s", root, NETCFG_DIR, name);
}

/**** the saved networks ****/

/* networks written by hand with a password: its key made, the password wiped; one that cannot be is left out (said
   through note) */
static int make_keys(struct netcfg_net *list, int n, netcfg_note_fn note, void *arg) {
    int i, j;
    for(i = j = 0; i < n; i++) {
        struct netcfg_net *net = &list[i];
        if(net->has_password && !net->has_psk) {
            if(wpa_passphrase_to_pmk(net->password, net->ssid, net->ssid_len, net->psk) == 0) net->has_psk = true;
            else if(note) note(arg, 0, "a password that makes no key: that network is left out");
        }
        wipe(net->password, sizeof(net->password));
        net->has_password = false;
        if(net->security == NETCFG_SEC_WPA2 && !net->has_psk) {
            wipe(net, sizeof(*net));
            continue;
        }
        if(i != j) {
            list[j] = *net;
            wipe(net, sizeof(*net));
        }
        j++;
    }
    return j;
}

/* the networks in RAM become list (n), the refusals of those still there kept */
static void set_saved(const struct netcfg_net *list, int n) {
    struct saved *old = (struct saved *)kcalloc(NETCFG_NETS_MAX, sizeof(struct saved));
    int i, k, nold;
    mutex_lock(&nm_lock);
    nold = nsaved;
    if(old) memcpy(old, saved, nold * sizeof(struct saved));
    for(i = 0; i < n && i < NETCFG_NETS_MAX; i++) {
        saved[i].net = list[i];
        saved[i].refused_until = 0;
        for(k = 0; old && k < nold; k++) {
            if(old[k].net.ssid_len == list[i].ssid_len && !memcmp(old[k].net.ssid, list[i].ssid, list[i].ssid_len) &&
               !memcmp(old[k].net.psk, list[i].psk, NETCFG_KEY_LEN))
                saved[i].refused_until = old[k].refused_until;
        }
    }
    for(k = i; k < NETCFG_NETS_MAX; k++) wipe(&saved[k], sizeof(saved[k]));
    nsaved = i;
    mutex_unlock(&nm_lock);
    if(old) {
        wipe(old, NETCFG_NETS_MAX * sizeof(struct saved));
        kfree(old);
    }
}

static void save_note(void *arg, u_int line, const char *why) {
    if(line) kprintf("    networks.conf line %u: %s (not kept)\n", line, why);
    else kprintf("    networks.conf: %s\n", why);
}

static void start_keeper(bool at_boot);

/* change networks.conf on the stick: read it, change (the list as read, its count; 0, or -errno and why), write it,
   and the networks in RAM become what was written */
static int change_networks(int (*change)(struct netcfg_net *list, int *n, void *arg, char *why, size_t wsize), void *arg,
                           char *why, size_t wsize) {
    struct netcfg_net *list = (struct netcfg_net *)kcalloc(NETCFG_NETS_MAX + 1, sizeof(struct netcfg_net));
    char *text = (char *)kalloc(NETCFG_NETWORKS_MAX + 1), root[VFS_PATH_MAX], path[VFS_PATH_MAX];
    u_int len = 0;
    bool ours = false, opened = false;
    int n = 0, r;
    if(!list || !text) {
        kfree(list);
        kfree(text);
        snprintf(why, wsize, "no memory");
        return -ENOMEM;
    }
    mutex_lock(&stick_lock);
    r = stick_open(true, root, sizeof(root), &ours, why, wsize);
    if(r) goto out;
    opened = true;
    path_of(path, sizeof(path), root, NETCFG_NETWORKS);
    r = read_file(path, text, NETCFG_NETWORKS_MAX, &len);
    if(r == -ENOENT) r = len = 0;
    if(r) {
        snprintf(why, wsize, "%s/%s could not be read: %s", NETCFG_DIR, NETCFG_NETWORKS,
                 r == -EFBIG ? "it is longer than 16 KB" : strerror(r));
        goto out;
    }
    n = netcfg_parse_networks(text, len, list, NETCFG_NETS_MAX, save_note, NULL);
    n = make_keys(list, n, save_note, NULL);
    r = change(list, &n, arg, why, wsize);
    if(r) goto out;
    r = netcfg_write_networks(list, n, text, NETCFG_NETWORKS_MAX + 1);
    if(r < 0) {
        snprintf(why, wsize, "%s/%s would be longer than 16 KB", NETCFG_DIR, NETCFG_NETWORKS);
        r = -EFBIG;
        goto out;
    }
    len = (u_int)r;
    r = write_file(root, NETCFG_NETWORKS, text, len, why, wsize);
    if(!r) set_saved(list, n);
out:
    if(opened) stick_close(ours);
    mutex_unlock(&stick_lock);
    wipe(text, NETCFG_NETWORKS_MAX + 1);
    wipe(list, (NETCFG_NETS_MAX + 1) * sizeof(struct netcfg_net));
    kfree(text);
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

int netman_save_network(const struct netcfg_net *net, int *had, char *why, size_t size) {
    struct save_arg a;
    int r;
    a.net = net;
    a.had = -1;
    if(!net->ssid_len || net->ssid_len > NETCFG_SSID_MAX || (net->security == NETCFG_SEC_WPA2 && !net->has_psk)) {
        snprintf(why, size, "a network to save needs its name, and its key for WPA2");
        return -EINVAL;
    }
    r = change_networks(put_first, &a, why, size);
    if(had) *had = a.had;
    if(!r) start_keeper(false);
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

int netman_forget_network(const u8 *ssid, u_int len, char *why, size_t size) {
    struct forget_arg a;
    a.ssid = ssid;
    a.len = len;
    return change_networks(take_out, &a, why, size);
}

int netman_networks(struct netman_net *out, int max) {
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
    mutex_unlock(&nm_lock);
    return i;
}

/**** the settings ****/

void netman_settings(struct netcfg_settings *out) {
    mutex_lock(&nm_lock);
    *out = cfg;
    mutex_unlock(&nm_lock);
}

void netman_where(char *buf, size_t size) {
    struct bdev *v = bdev_boot_vol();
    int state;
    mutex_lock(&nm_lock);
    state = where_state;
    switch(state) {
    case WHERE_OFF:
        snprintf(buf, size, "not read at this start (settings=off): the defaults until SurfOS restarts; a change "
                 "is saved on %s", v ? v->name : "the stick");
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

int netman_set(int k, const char *value, char *why, size_t size) {
    struct netcfg_settings s;
    char *text, *out, root[VFS_PATH_MAX], path[VFS_PATH_MAX];
    u_int len = 0;
    bool ours = false;
    int r, n;
    mutex_lock(&nm_lock);
    s = cfg;
    mutex_unlock(&nm_lock);
    if(netcfg_set(&s, k, value, why, size)) return -1;
    mutex_lock(&nm_lock);
    switch(k) {                                         /* only this key: another command may change another */
    case NETCFG_TELNETD: cfg.telnetd = s.telnetd; break;
    case NETCFG_TELNETD_PORT: cfg.telnetd_port = s.telnetd_port; break;
    case NETCFG_WIFI: cfg.wifi = s.wifi; break;
    case NETCFG_ETHERNET: cfg.ethernet_dhcp = s.ethernet_dhcp; break;
    case NETCFG_HOSTNAME: strlcpy(cfg.hostname, s.hostname, sizeof(cfg.hostname)); break;
    }
    cfg.set |= 1u << k;
    mutex_unlock(&nm_lock);
    if(k == NETCFG_HOSTNAME) dhcp_set_hostname(s.hostname);
    if(k == NETCFG_WIFI && s.wifi) start_keeper(false);

    text = (char *)kalloc(NETCFG_SETTINGS_MAX + 1);
    out = (char *)kalloc(NETCFG_SETTINGS_MAX + 1);
    if(!text || !out) {
        kfree(text);
        kfree(out);
        snprintf(why, size, "no memory");
        return 1;
    }
    root[0] = 0;
    mutex_lock(&stick_lock);
    r = stick_open(true, root, sizeof(root), &ours, why, size);
    if(!r) {
        path_of(path, sizeof(path), root, NETCFG_SETTINGS);
        r = read_file(path, text, NETCFG_SETTINGS_MAX, &len);
        if(r && r != -ENOENT)
            snprintf(why, size, "%s/%s could not be read: %s", NETCFG_DIR, NETCFG_SETTINGS,
                     r == -EFBIG ? "it is longer than 8 KB" : strerror(r));
        else {
            n = netcfg_edit_settings(r == -ENOENT ? NULL : text, len, k, value, out, NETCFG_SETTINGS_MAX + 1);
            if(n < 0) {
                snprintf(why, size, "%s/%s would be longer than 8 KB", NETCFG_DIR, NETCFG_SETTINGS);
                r = -EFBIG;
            } else {
                r = write_file(root, NETCFG_SETTINGS, out, (u_int)n, why, size);
                if(!r) {
                    mutex_lock(&nm_lock);
                    had_settings = true;
                    mutex_unlock(&nm_lock);
                }
            }
        }
        stick_close(ours);
    }
    mutex_unlock(&stick_lock);
    kfree(text);
    kfree(out);
    return r ? 1 : 0;
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
    struct bdev *v = NULL;
    char *text = NULL, root[VFS_PATH_MAX], path[VFS_PATH_MAX], why[96], a[8], b[8];
    u_int waited, len = 0;
    bool ours = false, have_s = false, have_n = false;
    int n = 0, r;

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
    for(waited = 0; !(v = bdev_boot_vol()) && waited < BOOTVOL_WAIT_MS; waited += 100) sleep_ms(100);
    if(!v) {
        where_state = WHERE_NOT_FOUND;
        klogf("+NET: the volume GRUB was started from (bootvol=) was not found within %u s: no settings read\n",
              BOOTVOL_WAIT_MS / 1000);
        return;
    }
    strlcpy(where_vol, v->name, sizeof(where_vol));
    where_readonly = v->protect != bdev_boot_volume;
    text = (char *)kalloc(NETCFG_NETWORKS_MAX + 1);
    list = (struct netcfg_net *)kcalloc(NETCFG_NETS_MAX, sizeof(struct netcfg_net));
    if(!text || !list) {
        strlcpy(where_why, "no memory", sizeof(where_why));
        where_state = WHERE_FAILED;
        goto out;
    }
    mutex_lock(&stick_lock);
    r = stick_open(false, root, sizeof(root), &ours, why, sizeof(why));
    if(r) {
        mutex_unlock(&stick_lock);
        strlcpy(where_why, why, sizeof(where_why));
        where_state = WHERE_FAILED;
        kprintf("+NET: the settings on %s were not read: %s\n", v->name, why);
        goto out;
    }
    netcfg_defaults(&s);
    path_of(path, sizeof(path), root, NETCFG_SETTINGS);
    r = read_file(path, text, NETCFG_SETTINGS_MAX, &len);
    if(r == 0) {
        have_s = true;
        netcfg_parse_settings(text, len, &s, boot_note, (void *)"settings.conf");
    } else if(r != -ENOENT) {
        kprintf("+NET: %s/%s on %s was not read: %s\n", NETCFG_DIR, NETCFG_SETTINGS, v->name,
                r == -EFBIG ? "it is longer than 8 KB" : strerror(r));
    }
    path_of(path, sizeof(path), root, NETCFG_NETWORKS);
    r = read_file(path, text, NETCFG_NETWORKS_MAX, &len);
    if(r == 0) {
        have_n = true;
        n = netcfg_parse_networks(text, len, list, NETCFG_NETS_MAX, boot_note, (void *)"networks.conf");
        n = make_keys(list, n, boot_note, (void *)"networks.conf");
    } else if(r != -ENOENT) {
        kprintf("+NET: %s/%s on %s was not read: %s\n", NETCFG_DIR, NETCFG_NETWORKS, v->name,
                r == -EFBIG ? "it is longer than 16 KB" : strerror(r));
    }
    stick_close(ours);
    mutex_unlock(&stick_lock);

    mutex_lock(&nm_lock);
    cfg = s;
    had_settings = have_s;
    had_networks = have_n;
    where_state = WHERE_READ;
    mutex_unlock(&nm_lock);
    set_saved(list, n);
    if(have_s || have_n)
        kprintf("+NET: settings from %s (%s/): telnetd %s%s%s, wifi %s, ethernet %s, hostname %s; %i saved Wi-Fi "
                "network%s%s\n", v->name, NETCFG_DIR, s.telnetd ? "on" : "off",
                s.telnetd && s.telnetd_port != 23 ? " port " : "",
                s.telnetd && s.telnetd_port != 23 ? netcfg_value(&s, NETCFG_TELNETD_PORT, a, sizeof(a)) : "",
                s.wifi ? "on" : "off", netcfg_value(&s, NETCFG_ETHERNET, b, sizeof(b)), s.hostname, n, n == 1 ? "" : "s",
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
}

/**** telnetd ****/

int netman_start_telnetd(u16 port) {
    char portword[8], *argv[] = { "telnetd", "on", portword, NULL };
    int argc = 2, pid, status = -1, waited, r;
    if(port && port != 23) {
        snprintf(portword, sizeof(portword), "%u", (u_int)port);
        argc = 3;
    }
    pid = process_spawn(TELNETD_PATH, argc, argv, &conArray[0], 0);
    if(pid < 0) {
        kprintf("+NET: %s: %s\n", TELNETD_PATH, strerror(pid));
        return -1;
    }
    for(waited = 0; waited < TELNETD_DONE_MS; waited += 100) {
        r = task_wait_opts((u_long)pid, &status, WNOHANG);
        if(r == pid) return status;
        if(r < 0) break;
        sleep_ms(100);
    }
    kprintf("+NET: telnetd on (pid %i) has not finished after %i s; not waiting for it\n", pid, TELNETD_DONE_MS / 1000);
    return -1;                          /* it is init's when this thread ends */
}

static bool an_address(void) {
    struct netdev *d;
    for(d = netdev_first(); d; d = d->next) if(d->ip && d->link_up) return true;
    return false;
}

/**** Ethernet and the leases (the netman thread's own: nothing else touches ifs) ****/

struct ifstate {
    struct netdev *dev;
    bool was_up;
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
        u_long wait;
        if(strncmp(d->name, "eth", 3) || !(st = state_of(d))) continue;
        if(up && !st->was_up) {                         /* a link came up: try at once */
            st->next_dhcp = now;
            st->fails = 0;
        }
        st->was_up = up;
        if(!on || !up || d->ip || autonet_busy() || (long)(now - st->next_dhcp) < 0) continue;
        kprintf("+NET: %s: link up; asking for an address (DHCP)\n", d->name);
        if(dhcp_configure(d, DHCP_MS) == 0) {
            st->fails = 0;
            continue;
        }
        st->fails++;
        wait = ETH_RETRY_MS << (st->fails > 4 ? 4 : st->fails - 1);
        if(wait > ETH_RETRY_MAX) wait = ETH_RETRY_MAX;
        st->next_dhcp = getticks() + wait / (1000 / HZ);
        kprintf("+NET: %s: no lease; the next try in %lu s\n", d->name, wait / 1000);
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
            dhcp_drop(d, "its lease ran out");
            continue;
        }
        if(!d->link_up) continue;                       /* nothing to ask through; when the link is back */
        st->renew_try = getticks() ? getticks() : 1;
        r = dhcp_renew(d, step == NETCFG_LEASE_REBIND, RENEW_MS);
        if(r == -2) dhcp_drop(d, "the DHCP server would not renew it");
        else if(r) klogf("+NET: %s: no answer to renewing its lease (%s); tried again later\n", d->name,
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

/* the station refused the key of the network named so: not tried for a while */
static void mark_refused(const u8 *ssid, u_int len) {
    int i;
    mutex_lock(&nm_lock);
    for(i = 0; i < nsaved; i++) {
        if(saved[i].net.ssid_len == len && !memcmp(saved[i].net.ssid, ssid, len)) {
            saved[i].refused_until = getticks() + WIFI_REFUSED_MS / (1000 / HZ);
            if(!saved[i].refused_until) saved[i].refused_until = 1;
        }
    }
    mutex_unlock(&nm_lock);
}

static int join_saved(struct wlan_dev *w, const struct saved *s, bool cmd) {
    char name[4 * NETCFG_SSID_MAX + 1];
    int r = wlan_join(w, s->net.ssid, s->net.ssid_len, NULL, s->net.security == NETCFG_SEC_WPA2 ? s->net.psk : NULL);
    ssid_str(s->net.ssid, s->net.ssid_len, name, sizeof(name));
    if(r == -EACCES) {
        mark_refused(s->net.ssid, s->net.ssid_len);
        say(cmd, "%s: %s refused its saved key: has its password changed? (wifi join %s, then wifi save; not tried "
            "again for %u minutes)", w->name, name, name, WIFI_REFUSED_MS / 60000);
    } else if(r && r != -ENOENT && r != -EINTR) {
        say(cmd, "%s: joining %s failed: %s", w->name, name, r == -ETIMEDOUT ? "no answer" : strerror(r));
    }
    return r;
}

/* the address for the network just joined */
static void get_address(struct wlan_dev *w, const char *name, bool cmd) {
    struct netdev *nd = w->netdev;
    int i;
    say(cmd, "%s: joined %s; asking for an address (DHCP)", w->name, name);
    for(i = 0; i < DHCP_TRIES; i++) {
        if(i) sleep_ms(DHCP_PAUSE_MS);
        if(cmd && task_interrupted()) return;
        if(!nd->link_up) {
            say(cmd, "%s: the link to %s went before an address came", w->name, name);
            return;
        }
        if(dhcp_configure(nd, DHCP_MS) == 0) return;
    }
    say(cmd, "%s: no address on %s after %u tries (dhcp %s asks again)", w->name, name, DHCP_TRIES, w->name);
}

/* join the first saved network in range and get its address: 0, -ENOENT none in range, or why not */
static int connect_saved(struct wlan_dev *w, bool cmd) {
    struct saved *list = (struct saved *)kcalloc(NETCFG_NETS_MAX, sizeof(struct saved));
    struct wlan_bss *bss = (struct wlan_bss *)kalloc(BSS_MAX * sizeof(struct wlan_bss));
    struct wlan_station_info si;
    char name[4 * NETCFG_SSID_MAX + 1];
    u_long now, scanned;
    int n, nb, i, j, r = -ENOMEM, pick = -1, tried = 0;
    if(!list || !bss) goto out;
    mutex_lock(&nm_lock);
    n = nsaved;
    memcpy(list, saved, n * sizeof(struct saved));
    mutex_unlock(&nm_lock);
    if(!n) {
        say(cmd, "no saved Wi-Fi networks (wifi save keeps the one wlan0 is joined to)");
        r = -ENOENT;
        goto out;
    }
    if(!w->up) {
        say(cmd, "%s: starting the Wi-Fi card to join a saved network", w->name);
        r = wlan_up(w);
        if(r) {
            if(r != -EBUSY && r != -EINTR) say(cmd, "%s: the card did not start: %s", w->name, strerror(r));
            goto out;
        }
    }
    scanned = getticks();
    r = wlan_scan(w, WIFI_SCAN_MS);
    if(r && r != -ETIMEDOUT) {
        if(r != -EBUSY && r != -EINTR) say(cmd, "%s: the scan failed: %s", w->name, strerror(r));
        goto out;
    }
    nb = (int)wlan_bss_list(w, bss, BSS_MAX);
    now = getticks();
    for(i = 0; i < n && pick < 0; i++) {                /* the saved ones in their order, as heard */
        if(refused(&list[i], now)) continue;
        for(j = 0; j < nb; j++) {                       /* heard in this scan: the list keeps a network for a minute */
            if(bss[j].ssid_len == list[i].net.ssid_len && !memcmp(bss[j].ssid, list[i].net.ssid, bss[j].ssid_len) &&
               sec_fits(list[i].net.security, bss[j].security) && (long)(bss[j].seen - scanned) >= 0)
                break;
        }
        if(j == nb) continue;
        ssid_str(list[i].net.ssid, list[i].net.ssid_len, name, sizeof(name));
        say(cmd, "%s: %s is in range (%d dBm): joining it", w->name, name, bss[j].rssi);
        tried++;
        r = join_saved(w, &list[i], cmd);
        if(r == 0) pick = i;
        if(r == -EINTR || r == -EBUSY) goto out;
    }
    for(i = 0; i < n && pick < 0; i++) {                /* then the hidden ones, by a probe for their names */
        if(!list[i].net.hidden || refused(&list[i], now)) continue;
        ssid_str(list[i].net.ssid, list[i].net.ssid_len, name, sizeof(name));
        say(cmd, "%s: looking for the hidden network %s", w->name, name);
        tried++;
        r = join_saved(w, &list[i], cmd);
        if(r == 0) pick = i;
        if(r == -EINTR || r == -EBUSY) goto out;
    }
    if(pick < 0) {
        if(!tried) say(cmd, "%s: none of the %i saved network%s is in range", w->name, n, n == 1 ? "" : "s");
        r = -ENOENT;
        goto out;
    }
    if(wlan_station(w, &si, false) == 0) {              /* the association is the manager's: its address too */
        keeper_requests = si.join_requests;
        keeper_dhcp_for = si.joins;
        keeper_owns = true;
    }
    ssid_str(list[pick].net.ssid, list[pick].net.ssid_len, name, sizeof(name));
    get_address(w, name, cmd);
    r = 0;
out:
    if(list) {
        wipe(list, NETCFG_NETS_MAX * sizeof(struct saved));
        kfree(list);
    }
    kfree(bss);
    return r;
}

int netman_wifi_connect(struct wlan_dev *w) {
    int r;
    if(!mutex_trylock(&connect_lock)) return -EBUSY;
    if(!keeper_dev) keeper_dev = w;                     /* autonet asks netman_wifi_owns() about it meanwhile */
    keeper_connecting = true;
    r = connect_saved(w, true);
    keeper_connecting = false;
    mutex_unlock(&connect_lock);
    return r;
}

bool netman_wifi_owns(struct netdev *dev) {
    struct wlan_dev *w = keeper_dev;
    struct wlan_station_info si;
    if(!w || !dev || dev != w->netdev) return false;
    if(keeper_connecting) return true;
    return keeper_owns && wlan_station(w, &si, false) == 0 && si.state == WLAN_STA_UP &&
           si.join_requests == keeper_requests;
}

/* the keeper: the boot's join, then a saved network again whenever the station gives one up */
static void wifiman_main(void *arg) {
    struct wlan_dev *w = (struct wlan_dev *)arg;
    struct wlan_station_info si;
    u_long next = getticks();
    u_int tries = 0, last_requests = 0;
    int last_state = -1, last_idle = -1, r;
    bool retrying = keeper_at_boot, on;                 /* a keeper started later (wifi save) waits for a loss */
    for(;;) {
        int n;
        mutex_lock(&nm_lock);
        on = cfg.wifi;
        n = nsaved;
        mutex_unlock(&nm_lock);
        if(wlan_station(w, &si, false)) break;
        if(!on || !n) retrying = false;
        else if(si.state == WLAN_STA_UP) {
            retrying = false;
            tries = 0;
            /* an association of the manager's own whose address went (its lease ran out, say): asked for once
               more; while it has one, a later loss is asked about again */
            if(w->netdev->ip) keeper_dhcp_for = 0;
            else if(keeper_owns && si.join_requests == keeper_requests && keeper_dhcp_for != si.joins &&
                    !keeper_connecting) {
                keeper_dhcp_for = si.joins;
                if(mutex_trylock(&connect_lock)) {
                    char name[4 * NETCFG_SSID_MAX + 1];
                    get_address(w, ssid_str(si.ssid, si.ssid_len, name, sizeof(name)), false);
                    mutex_unlock(&connect_lock);
                }
            }
        } else if((si.state == WLAN_STA_IDLE || si.state == WLAN_STA_DOWN) && si.idle == WLAN_IDLE_LEFT) {
            retrying = false;                           /* wifi leave or wifi down: the owner's */
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
                r = connect_saved(w, false);
                keeper_connecting = false;
                mutex_unlock(&connect_lock);
            } else {
                r = -EBUSY;
            }
            if(r == 0) {
                retrying = false;
                tries = 0;
            } else {
                u_long wait = r == -EBUSY ? WIFI_BUSY_MS : ++tries < WIFI_LATE_TRIES ? WIFI_RETRY_MS : WIFI_RETRY_LATE;
                if(tries == 1 && r != -EBUSY) kprintf("+NET: %s: no saved network joined; looking again every %lu s\n",
                                                      w->name, wait / 1000);
                next = getticks() + wait / (1000 / HZ);
            }
        }
        sleep_ms(1000);
    }
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
            keeper_dev = w;
            keeper_at_boot = at_boot;
            wifiman = kthread_create_on("wifiman", &conArray[0], wifiman_main, w, PL_NORMAL, TF_DETACHED);
            if(!wifiman) kprintf("+NET: no memory for the Wi-Fi keeper: wifi connect joins a saved network by hand\n");
        }
    }
    mutex_unlock(&keeper_lock);
}

/**** the thread ****/

static void netman_main(void *arg) {
    bool autonet = bootinfo_has_arg(&bootinfo, "autonet"), telnetd, started = false;
    char host[NETCFG_HOSTNAME_MAX + 1];
    u_long start = uptime_ms();
    u16 port;
    load_at_boot();
    mutex_lock(&nm_lock);
    telnetd = cfg.telnetd && !autonet;                  /* autonet starts it itself, on 23 */
    port = cfg.telnetd_port;
    strlcpy(host, cfg.hostname, sizeof(host));
    mutex_unlock(&nm_lock);
    dhcp_set_hostname(host);
    start_keeper(true);
    for(;;) {
        ethernet_step();
        lease_step();
        if(telnetd && !started && (an_address() || uptime_ms() - start >= TELNETD_WAIT_MS)) {
            started = true;
            kprintf("+NET: starting telnetd on F1 (the settings: telnetd = on)\n");
            netman_start_telnetd(port);
        }
        sleep_ms(telnetd && !started ? 250 : 1000);
    }
}

void init_netman(void) {
    netcfg_defaults(&cfg);
    dhcp_set_hostname(cfg.hostname);
    saved = (struct saved *)kcalloc(NETCFG_NETS_MAX, sizeof(struct saved));
    if(!saved || !kthread_create_on("netman", &conArray[0], netman_main, NULL, PL_NORMAL, TF_DETACHED))
        kprintf("+NET: no memory for the network manager: no settings from the stick\n");
}
