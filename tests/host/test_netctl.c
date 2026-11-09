/* Host tests for SYS_netctl (kernel/netctl.c, include/surfos/netctl.h; the desktop applications, track settings,
   11/05/26). kernel/netctl.c is built here as it is, with stand-ins for the rest of the kernel, as tests/host/test_win.c
   builds kernel/win.c: a simulated Wi-Fi station (its state, its scan list, what each request answers, what it was
   asked and with which key), the network manager's calls (its settings and saved networks, what a change or a save
   was given), DHCP that fails as often as told, interfaces, the program's memory as the test's own with a range it
   calls unreadable, an allocator that counts what is alive and can fail at the nth call, locks that count, a thread
   creation the test runs when it wants (or that fails), and a kernel log the test reads. net/netcfg.c is linked as it
   is (the settings' table is the real one), and so are the supplicant's wpa_passphrase_to_pmk() (net/wlan/wpa.c,
   lib/crypto/) and the SSID's text (net/wlan/ieee80211.c): the key that reaches the station is checked against the
   test vector of IEEE 802.11i-2004 annex H.4.1.
   The scenarios: every op's size and pointer refused before anything is touched, and a read writing its structure and
   not a byte past it; the reads mirroring the station, the scan list, the saved networks, the table and the
   interfaces; jobs one at a time, their stages, results, flags and numbers (wrapping), and one log line each; a join's
   security decided as wifi join decides it; the password wiped from the program's memory whatever the call answers,
   the PMK made at once, handed to the station and to a save, and wiped at the job's end, never in a read, the job's
   words or the log; DHCP after a join while the station keeps it; a save refused when another join came meanwhile;
   the card on with the network manager's connect, and off; leave, save, forget and set; out of memory at every
   allocation and a thread that cannot start. Every scenario ends with nothing allocated and no lock held.
   Built and run by `make unittest` (unittest-netctl). */
#include <stdio.h>
#include <string.h>
#include <surfos/types.h>
#include <surfos/task.h>
#include <surfos/console.h>
#include <surfos/sync.h>
#include <surfos/sysimpl.h>
#include <surfos/timer.h>
#include <mm/kalloc.h>
#include <fs/vfs.h>
#include <net/net.h>
#include <net/wlan.h>
#include <net/wlan_cmd.h>
#include <net/netcfg.h>
#include <net/netman.h>
#include <crypto/crypto.h>

void *malloc(size_t n);                 /* the host's */
void free(void *p);

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

/**** stand-ins: tasks, threads, locks, the clock ****/

surf_task *curTask;
static surf_task program, thread_task;
surf_console conArray[4];
static kthread_fn thread_fn;
static void *thread_arg;
static int threads_made, thread_fail;

surf_task *kthread_create_on(const char *name, surf_console *con, kthread_fn fn, void *arg, prio_level prio, u_int flags) {
    (void)name; (void)con; (void)prio; (void)flags;
    if(thread_fail) return NULL;
    thread_fn = fn;
    thread_arg = arg;
    threads_made++;
    return &thread_task;
}

/* the job's thread runs now, to its end */
static void run_thread(void) {
    kthread_fn f = thread_fn;
    surf_task *was = curTask;
    thread_fn = NULL;
    if(!f) return;
    curTask = &thread_task;
    f(thread_arg);
    curTask = was;
}

static int held;                        /* locks held now */
void mutex_lock(mutex_t *m) { m->locked++; held++; }
void mutex_unlock(mutex_t *m) { m->locked--; held--; }
mutex_t net_lock;

static u_long now = 100000;
u_long getticks(void) { return now; }
void sleep_ms(u_long ms) { now += ms / (1000 / HZ); }

static char klog[4096];
void klog_write(const char *s) {
    size_t n = strlen(klog), m = strlen(s);
    if(n + m < sizeof(klog)) memcpy(klog + n, s, m + 1);
}
void kprintf(const char *fmt, ...) { (void)fmt; }               /* the supplicant's lines: none in these tests */
void crypto_random(void *buf, size_t len) { memset(buf, 0x5a, len); }

const char *strerror(int err) {
    switch(err) {
    case -EBUSY: return "busy";
    case -EACCES: return "permission denied";
    case -ENOENT: return "not found";
    case -ENOMEM: return "out of memory";
    case -ETIMEDOUT: return "timed out";
    default: return "error";
    }
}

/**** stand-ins: memory ****/

static int live, fail_in, allocs;
void *kalloc(size_t n) {
    allocs++;
    if(fail_in && --fail_in == 0) return NULL;
    live++;
    return malloc(n ? n : 1);
}
void *kcalloc(size_t count, size_t size) {
    char *p = (char *)kalloc(count * size);
    if(p) memset(p, 0, count * size);
    return p;
}
void kfree(void *p) {
    if(!p) return;
    live--;
    free(p);
}

/* the program's memory: all of the test's but [bad_lo, bad_hi) and the first page; [ro_lo, ro_hi) it may read but
   not write (11/05/26, the review: so that a call that writes without asking user_buf() to check for it fails) */
static const char *bad_lo, *bad_hi, *ro_lo, *ro_hi;
bool user_buf(const void *p, u_long len, bool write) {
    u_long a = (u_long)p, end = a + len;
    if(a < 4096 || end < a) return false;
    if(bad_lo && a < (u_long)bad_hi && end > (u_long)bad_lo) return false;
    if(write && ro_lo && a < (u_long)ro_hi && end > (u_long)ro_lo) return false;
    return true;
}

/**** stand-ins: the station ****/

static struct wlan_dev wdev = { "wlan0" };
static struct netdev wnd = { "wlan0" }, eth = { "eth0" };
static bool have_wifi;

static struct {
    int state, idle;
    u8 ssid[WLAN_SSID_MAX];
    u_int ssid_len;
    u8 security;
    bool hidden, has_key;
    u8 key[32];
    u_int join_requests, stops;
    struct wlan_bss list[16];
    u_int nlist;
    int up_r, down_r, scan_r, leave_r, join_r;  /* what the requests answer */
    int ups, downs, scans, leaves, joins;       /* how often they were made */
    u8 join_pmk[32];
    bool join_had_pmk, join_quiet;
    u8 join_ssid[WLAN_SSID_MAX];
    u_int join_ssid_len, join_stops;
    bool other_join;                    /* another join comes right after ours (a command's) */
    bool no_link;                       /* a join whose link goes again before an address comes */
    const char *describe;
    struct wlan_bss heard;              /* a network the next scan hears (heard_by_scan) */
    bool heard_by_scan;
    int join_stage;                     /* the job's stage when the station was asked to join */
} sta;
static int (*stage_now)(void);          /* the job's stage (kernel/netctl.c's record, included further down) */

struct wlan_dev *wlan_find(const char *name) {
    if(!have_wifi) return NULL;
    if(name && strcmp(name, wdev.name)) return NULL;
    return &wdev;
}

int wlan_station(struct wlan_dev *w, struct wlan_station_info *out, bool want_key) {
    memset(out, 0, sizeof(*out));
    if(w != &wdev) return -ENODEV;
    out->state = sta.state;
    out->idle = sta.idle;
    memcpy(out->ssid, sta.ssid, sta.ssid_len);
    out->ssid_len = (u8)sta.ssid_len;
    out->security = sta.security;
    out->hidden = sta.hidden;
    out->join_requests = sta.join_requests;
    out->stops = sta.stops;
    if(want_key && sta.has_key) {
        memcpy(out->key, sta.key, 32);
        out->has_key = true;
    }
    return 0;
}

void wlan_describe(struct wlan_dev *w, char *buf, size_t size) {
    (void)w;
    snprintf(buf, size, "%s", sta.describe ? sta.describe : "up, not associated");
}

u_int wlan_bss_list(struct wlan_dev *w, struct wlan_bss *out, u_int max) {
    u_int i;
    (void)w;
    for(i = 0; i < sta.nlist && i < max; i++) out[i] = sta.list[i];
    return i;
}

int wlan_up(struct wlan_dev *w) {
    (void)w;
    sta.ups++;
    if(!sta.up_r) {
        w->up = true;
        if(sta.state == WLAN_STA_DOWN) sta.state = WLAN_STA_IDLE;
    }
    return sta.up_r;
}

int wlan_down_wait(struct wlan_dev *w) {
    sta.downs++;
    if(!sta.down_r) {
        w->up = false;
        sta.state = WLAN_STA_DOWN;
        sta.stops++;
    }
    return sta.down_r;
}

int wlan_scan(struct wlan_dev *w, u_int timeout_ms) {
    (void)w; (void)timeout_ms;
    sta.scans++;
    if(sta.heard_by_scan && sta.nlist < sizeof(sta.list) / sizeof(sta.list[0])) {
        sta.list[sta.nlist++] = sta.heard;
        sta.heard_by_scan = false;
    }
    return sta.scan_r;
}

int wlan_leave_wait(struct wlan_dev *w) {
    (void)w;
    sta.leaves++;
    if(!sta.leave_r) {
        sta.state = WLAN_STA_IDLE;
        sta.idle = WLAN_IDLE_LEFT;
        sta.stops++;
        wnd.link_up = false;
    }
    return sta.leave_r;
}

int wlan_join_unless(struct wlan_dev *w, const u8 *ssid, u_int ssid_len, const u8 *pmk, u_int stops, bool quiet,
                     u_int *join) {
    (void)w;
    sta.joins++;
    sta.join_stage = stage_now ? stage_now() : -1;
    sta.join_had_pmk = pmk != NULL;
    memset(sta.join_pmk, 0, 32);
    if(pmk) memcpy(sta.join_pmk, pmk, 32);
    memcpy(sta.join_ssid, ssid, ssid_len);
    sta.join_ssid_len = ssid_len;
    sta.join_stops = stops;
    sta.join_quiet = quiet;
    if(stops != sta.stops) return -ECANCELED;
    if(join) *join = ++sta.join_requests;
    if(sta.join_r) {
        sta.state = WLAN_STA_IDLE;
        sta.idle = WLAN_IDLE_JOIN_FAILED;
        return sta.join_r;
    }
    sta.state = WLAN_STA_UP;
    memcpy(sta.ssid, ssid, ssid_len);
    sta.ssid_len = ssid_len;
    sta.security = pmk ? WLAN_SEC_WPA2_PSK : WLAN_SEC_OPEN;
    sta.has_key = pmk != NULL;
    if(pmk) memcpy(sta.key, pmk, 32);
    wnd.link_up = !sta.no_link;
    if(sta.other_join) sta.join_requests++;
    return 0;
}

static void add_bss(const char *ssid, u8 security, i8 rssi, u8 channel, bool hidden) {
    struct wlan_bss *b = &sta.list[sta.nlist++];
    memset(b, 0, sizeof(*b));
    b->ssid_len = (u8)strlen(ssid);
    memcpy(b->ssid, ssid, b->ssid_len);
    b->security = security;
    b->rssi = rssi;
    b->channel = channel;
    b->band = channel > 14 ? WLAN_BAND_5GHZ : WLAN_BAND_2GHZ;
    b->bssid[0] = 0x02;
    b->bssid[5] = (u8)sta.nlist;
    b->hidden = hidden;
    b->seen = now - 3 * HZ;
}

/**** stand-ins: interfaces, DHCP ****/

static int dhcp_fail, dhcp_calls;
struct netdev *netdev_first(void) { return &eth; }
int dhcp_configure_quiet(struct netdev *dev, u_long timeout_ms, bool quiet) {
    (void)timeout_ms; (void)quiet;
    dhcp_calls++;
    if(dhcp_fail > 0) {
        dhcp_fail--;
        return -1;
    }
    dev->ip = 0x0f02000a;               /* 10.0.2.15 */
    dev->netmask = 0x00ffffff;
    dev->gateway = 0x0202000a;
    return 0;
}
void dhcp_hostname(char *out, size_t size) { snprintf(out, size, "%s", "surfos"); }

/**** stand-ins: the network manager ****/

static struct netcfg_settings cfg;
static struct netman_net nets[NETCFG_NETS_MAX];
static int nnets, nkept;
static bool settings_off, can_save = true, telnet_exposed, ftp_exposed;
static int set_r, set_calls, set_k;
static char set_value[64];
static int save_r, save_calls;
static struct netcfg_net saved_net;
static int forget_r, forget_calls;
static u8 forget_ssid[WLAN_SSID_MAX];
static u_int forget_len;
static int connect_r, connect_calls;

void netman_settings(struct netcfg_settings *out) { *out = cfg; }
bool netman_settings_off(void) { return settings_off; }
bool netman_can_save(void) { return can_save; }
void netman_where(char *buf, size_t size) { snprintf(buf, size, "%s", "surfos-config/settings.conf on usb1"); }
bool netman_telnet_exposed(void) { return telnet_exposed; }
bool netman_ftp_exposed(void) { return ftp_exposed; }

int netman_networks(struct netman_net *out, int max, int *kept) {
    int i;
    for(i = 0; i < nnets && i < max; i++) out[i] = nets[i];
    if(kept) *kept = nkept;
    return i;
}

int netman_set(int k, const char *value, char *why, size_t size) {
    set_calls++;
    set_k = k;
    snprintf(set_value, sizeof(set_value), "%s", value);
    if(set_r < 0) {
        snprintf(why, size, "%s", "refused by the network manager");
        return -1;
    }
    netcfg_set(&cfg, k, value, why, size);
    if(set_r > 0) snprintf(why, size, "%s", "the stick could not be written");
    return set_r;
}

static int apply_calls;
int netman_apply(int k, const char *value, char *why, size_t size) {
    struct netcfg_settings s = cfg;
    apply_calls++;
    if(netcfg_set(&s, k, value, why, size)) return -1;
    if(settings_off) return 1;
    cfg = s;
    return 0;
}

int netman_save_network(const struct netcfg_net *net, int *had, int *count, char *why, size_t size) {
    save_calls++;
    saved_net = *net;
    if(had) *had = -1;
    if(count) *count = 1;
    if(save_r) snprintf(why, size, "%s", "the stick may not be written");
    return save_r;
}

int netman_forget_network(const u8 *ssid, u_int len, int *count, char *why, size_t size) {
    forget_calls++;
    memcpy(forget_ssid, ssid, len);
    forget_len = len;
    if(count) *count = 0;
    if(forget_r && forget_r != -ENOENT) snprintf(why, size, "%s", "the stick may not be written");
    return forget_r;
}

int netman_wifi_connect_quiet(struct wlan_dev *w) {
    (void)w;
    connect_calls++;
    if(!connect_r) {
        sta.state = WLAN_STA_UP;
        memcpy(sta.ssid, "SurfSim", 7);
        sta.ssid_len = 7;
        wnd.ip = 0x0f02000a;
    }
    return connect_r;
}

#include "../../kernel/netctl.c"

/**** the scenarios ****/

/* the test vector of IEEE 802.11i-2004 annex H.4.1: the passphrase "password" and the SSID "IEEE" */
static const u8 ieee_pmk[32] = { 0xf4, 0x2c, 0x6f, 0xc5, 0x2d, 0xf0, 0xeb, 0xef, 0x9e, 0xbb, 0x4b, 0x90, 0xb3, 0x8a,
                                 0x5f, 0x90, 0x2e, 0x83, 0xfe, 0x1b, 0x13, 0x5a, 0x70, 0xe2, 0x3a, 0xed, 0x76, 0x2e,
                                 0x97, 0x10, 0xa1, 0x2e };

static int job_stage_now(void) {
    return job.stage;
}

/* a fresh machine: a Wi-Fi card that is on, idle, with networks in range; Ethernet with a lease; the defaults */
static void start(void) {
    stage_now = job_stage_now;
    memset(&sta, 0, sizeof(sta));
    memset(&wnd, 0, sizeof(wnd));
    memset(&eth, 0, sizeof(eth));
    strcpy(wnd.name, "wlan0");
    strcpy(eth.name, "eth0");
    eth.next = &wnd;
    eth.mac[0] = 0x52;
    eth.link_up = true;
    eth.ip = 0x0f02000a;
    eth.netmask = 0x00ffffff;
    eth.dhcp_server = 0x0202000a;
    eth.lease_s = 86400;
    eth.lease_at = now - 100 * HZ;
    wnd.mac[0] = 0x02;
    wdev.netdev = &wnd;
    wdev.up = true;
    have_wifi = true;
    sta.state = WLAN_STA_IDLE;
    add_bss("SurfSimWPA", WLAN_SEC_WPA2_PSK, -52, 36, false);
    add_bss("SurfSim", WLAN_SEC_OPEN, -55, 6, false);
    add_bss("IEEE", WLAN_SEC_WPA2_WPA3, -60, 1, false);
    add_bss("SurfSimWEP", WLAN_SEC_WEP, -71, 1, false);
    add_bss("SurfSimSAE", WLAN_SEC_WPA3_SAE, -72, 149, false);
    add_bss("Twins", WLAN_SEC_OPEN, -40, 6, false);    /* an open and a WPA2 network of one name: WPA2 wins */
    add_bss("Twins", WLAN_SEC_WPA2_PSK, -80, 11, false);
    netcfg_defaults(&cfg);
    nnets = nkept = 0;
    settings_off = telnet_exposed = ftp_exposed = false;
    can_save = true;
    set_r = set_calls = save_r = save_calls = forget_r = forget_calls = connect_r = connect_calls = 0;
    memset(&saved_net, 0, sizeof(saved_net));
    dhcp_fail = dhcp_calls = 0;
    threads_made = thread_fail = 0;
    thread_fn = NULL;
    fail_in = 0;
    bad_lo = bad_hi = ro_lo = ro_hi = NULL;
    klog[0] = 0;
    memset(&program, 0, sizeof(program));
    strcpy(program.name, "wsettings");
    program.pid = 9;
    memset(&thread_task, 0, sizeof(thread_task));
    strcpy(thread_task.name, "netctl");
    curTask = &program;
}

/* nothing allocated, no lock held, no job's PMK left */
static void finish(void) {
    static const u8 zero[32];
    CHECK(live == 0);
    CHECK(held == 0);
    CHECK(job_lock.locked == 0 && net_lock.locked == 0);
    CHECK(!memcmp(req.pmk, zero, 32) || job.state == NETCTL_JOB_RUNNING);
}

/* is the byte string s (n bytes) anywhere in the n2 bytes at p? */
static bool has_bytes(const void *p, size_t n2, const void *s, size_t n) {
    const u8 *a = (const u8 *)p;
    size_t i;
    for(i = 0; i + n <= n2; i++) if(!memcmp(a + i, s, n)) return true;
    return false;
}

static struct netctl_job job_now(void) {
    struct netctl_job j;
    memset(&j, 0xee, sizeof(j));
    CHECK(sys_netctl(NETCTL_JOB, &j, sizeof(j)) == 0);
    return j;
}

static int join(const char *ssid, const char *pass, unsigned flags) {
    struct netctl_join j;
    memset(&j, 0, sizeof(j));
    memcpy(j.ssid, ssid, strlen(ssid));
    j.ssid_len = (unsigned)strlen(ssid);
    j.flags = flags;
    strcpy(j.passphrase, pass);
    return sys_netctl(NETCTL_JOIN, &j, sizeof(j));
}

static int request(int op, unsigned on, const char *ssid, const char *key, const char *value) {
    struct netctl_req r;
    memset(&r, 0, sizeof(r));
    r.on = on;
    if(ssid) {
        memcpy(r.ssid, ssid, strlen(ssid));
        r.ssid_len = (unsigned)strlen(ssid);
    }
    if(key) snprintf(r.key, sizeof(r.key), "%s", key);
    if(value) snprintf(r.value, sizeof(r.value), "%s", value);
    return sys_netctl(op, &r, sizeof(r));
}

static void sizes_and_pointers(void) {
    static const int reads[] = { NETCTL_WIFI, NETCTL_NETWORKS, NETCTL_SAVED, NETCTL_SETTINGS, NETCTL_IFACES, NETCTL_JOB };
    static const unsigned sizes[] = { sizeof(struct netctl_wifi), sizeof(struct netctl_bss_list),
                                      sizeof(struct netctl_saved), sizeof(struct netctl_settings),
                                      sizeof(struct netctl_ifaces), sizeof(struct netctl_job) };
    static char big[4096 + 64];
    struct netctl_req r;
    int i;
    start();
    CHECK(sizeof(struct netctl_wifi) == 388 && sizeof(struct netctl_bss_list) == 2308 &&
          sizeof(struct netctl_saved) == 584 && sizeof(struct netctl_settings) == 2064 &&
          sizeof(struct netctl_ifaces) == 392 && sizeof(struct netctl_job) == 260 && sizeof(struct netctl_join) == 104 &&
          sizeof(struct netctl_req) == 112);
    CHECK(sys_netctl(0, big, 16) == -EINVAL && sys_netctl(7, big, 16) == -EINVAL && sys_netctl(23, big, 16) == -EINVAL);
    CHECK(sys_netctl(-1, big, 16) == -EINVAL);
    for(i = 0; i < 6; i++) {
        memset(big, 0x77, sizeof(big));
        CHECK(sys_netctl(reads[i], big, sizes[i] - 1) == -EINVAL);   /* sizes are exact: a version check */
        CHECK(sys_netctl(reads[i], big, sizes[i] + 1) == -EINVAL);
        CHECK(sys_netctl(reads[i], big, 0) == -EINVAL);
        CHECK(sys_netctl(reads[i], NULL, sizes[i]) == -EFAULT);
        CHECK(sys_netctl(reads[i], (void *)16, sizes[i]) == -EFAULT);
        bad_lo = big + 100;
        bad_hi = big + 101;
        CHECK(sys_netctl(reads[i], big, sizes[i]) == -EFAULT);      /* one byte inside that is not the program's */
        CHECK((u8)big[0] == 0x77);                                   /* and nothing written */
        bad_lo = bad_hi = NULL;
        CHECK(sys_netctl(reads[i], big, sizes[i]) == 0);
        CHECK((u8)big[sizes[i]] == 0x77 && (u8)big[sizes[i] + 63] == 0x77);   /* not a byte past it */
    }
    /* the jobs' arguments */
    memset(&r, 0, sizeof(r));
    CHECK(sys_netctl(NETCTL_CARD, &r, sizeof(r) - 1) == -EINVAL);
    CHECK(sys_netctl(NETCTL_CARD, NULL, sizeof(r)) == -EFAULT);
    CHECK(sys_netctl(NETCTL_FORGET, (void *)8, sizeof(r)) == -EFAULT);
    CHECK(sys_netctl(NETCTL_SET, &r, 0) == -EINVAL);
    CHECK(sys_netctl(NETCTL_SCAN, &r, 4) == -EINVAL);               /* no argument: size 0 */
    CHECK(sys_netctl(NETCTL_JOIN, &r, sizeof(r)) == -EINVAL);
    CHECK(sys_netctl(NETCTL_JOIN, NULL, sizeof(struct netctl_join)) == -EFAULT);
    r.on = 2;
    CHECK(sys_netctl(NETCTL_CARD, &r, sizeof(r)) == -EINVAL);
    CHECK(threads_made == 0);
    finish();
}

static void reads(void) {
    struct netctl_wifi w;
    struct netctl_bss_list l;
    struct netctl_saved s;
    struct netctl_settings st;
    struct netctl_ifaces f;
    int i;
    start();
    have_wifi = false;
    memset(&w, 0x77, sizeof(w));
    CHECK(sys_netctl(NETCTL_WIFI, &w, sizeof(w)) == 0 && !w.present && !w.up && !w.ssid_len && !w.name[0]);
    memset(&l, 0x77, sizeof(l));
    CHECK(sys_netctl(NETCTL_NETWORKS, &l, sizeof(l)) == 0 && l.count == 0);
    have_wifi = true;
    sta.state = WLAN_STA_UP;
    memcpy(sta.ssid, "SurfSimWPA", 10);
    sta.ssid_len = 10;
    sta.security = WLAN_SEC_WPA2_PSK;
    sta.has_key = true;
    memset(sta.key, 0xab, 32);
    wnd.link_up = true;
    wnd.ip = 0x0f02000a;
    sta.describe = "associated with \"SurfSimWPA\" (02:00:00:00:00:01), channel 36, 5 GHz, signal -52 dBm, WPA2-Personal\n"
                   "last: 4-way handshake: a MIC did not check (the password may be wrong)\ndriver: wlansim\nframes: 1";
    telnet_exposed = true;
    CHECK(sys_netctl(NETCTL_WIFI, &w, sizeof(w)) == 0);
    CHECK(w.present && w.up && w.state == NETCTL_STA_UP && w.ssid_len == 10 && !memcmp(w.ssid, "SurfSimWPA", 10));
    CHECK(w.security == NETCTL_SEC_WPA2 && w.link_up && w.ip == 0x0f02000a && !strcmp(w.name, "wlan0"));
    CHECK(w.exposed == NETCTL_EXPOSED_TELNET);
    CHECK(!strncmp(w.state_text, "associated with \"SurfSimWPA\"", 28) && !strchr(w.state_text, '\n'));
    CHECK(!strcmp(w.last, "4-way handshake: a MIC did not check (the password may be wrong)"));
    CHECK(!has_bytes(&w, sizeof(w), sta.key, 32));                 /* a read never holds the station's key */
    sta.describe = "up, not associated\ndriver: last: not this one";
    CHECK(sys_netctl(NETCTL_WIFI, &w, sizeof(w)) == 0 && !strcmp(w.state_text, "up, not associated") && !w.last[0]);

    memset(&l, 0x77, sizeof(l));
    sta.list[0].ssid_len = 40;                                      /* never more than 32 bytes */
    sta.list[1].seen = now - 200000 * HZ;
    CHECK(sys_netctl(NETCTL_NETWORKS, &l, sizeof(l)) == 0 && l.count == sta.nlist);
    CHECK(l.bss[0].ssid_len == 32 && l.bss[0].channel == 36 && l.bss[0].band == 1 && l.bss[0].rssi == -52);
    CHECK(l.bss[0].security == NETCTL_SEC_WPA2 && l.bss[0].bssid[0] == 0x02 && l.bss[0].bssid[5] == 1);
    CHECK(l.bss[1].age_s == 0xffff && l.bss[2].age_s == 3 && l.bss[2].security == NETCTL_SEC_WPA2_WPA3);
    CHECK(l.bss[sta.nlist].ssid_len == 0 && l.bss[sta.nlist].rssi == 0);   /* the rest zero, not the program's bytes */

    nnets = 2;
    memcpy(nets[0].ssid, "SurfSimWPA", 10);
    nets[0].ssid_len = 10;
    nets[0].security = NETCFG_SEC_WPA2;
    nets[0].refused = true;
    memcpy(nets[1].ssid, "Hidden", 6);
    nets[1].ssid_len = 6;
    nets[1].hidden = true;
    nkept = 3;
    memset(&s, 0x77, sizeof(s));
    CHECK(sys_netctl(NETCTL_SAVED, &s, sizeof(s)) == 0 && s.count == 2 && s.kept == 3);
    CHECK(s.net[0].ssid_len == 10 && s.net[0].security == NETCTL_SAVED_WPA2 && s.net[0].refused && !s.net[0].hidden);
    CHECK(s.net[1].hidden && s.net[1].security == NETCTL_SAVED_OPEN && s.net[2].ssid_len == 0);

    netcfg_set(&cfg, NETCFG_HOSTNAME, "laptop", st.where, sizeof(st.where));
    netcfg_set(&cfg, NETCFG_ETHERNET, "dhcp", st.where, sizeof(st.where));
    settings_off = true;
    can_save = false;
    ftp_exposed = true;
    memset(&st, 0x77, sizeof(st));
    CHECK(sys_netctl(NETCTL_SETTINGS, &st, sizeof(st)) == 0 && st.count == NETCFG_NKEYS);
    CHECK(st.off && !st.can_save && st.exposed == (NETCTL_EXPOSED_TELNET | NETCTL_EXPOSED_FTP));
    CHECK(!strcmp(st.where, "surfos-config/settings.conf on usb1"));
    for(i = 0; i < (int)st.count; i++) CHECK(!strcmp(st.key[i].name, netcfg_key_name(i)));
    CHECK(!strcmp(st.key[NETCFG_HOSTNAME].value, "laptop") && st.key[NETCFG_HOSTNAME].set);
    CHECK(!strcmp(st.key[NETCFG_ETHERNET].value, "dhcp") && !strcmp(st.key[NETCFG_ETHERNET].values, "dhcp | off"));
    CHECK(!strcmp(st.key[NETCFG_WIFI].value, "on") && !st.key[NETCFG_WIFI].set);
    CHECK(!strcmp(st.key[NETCFG_TELNETD_PORT].value, "23") && !strcmp(st.key[NETCFG_TELNETD_PORT].values, "1-65535"));
    CHECK(st.key[NETCFG_NKEYS].name[0] == 0);

    memset(&f, 0x77, sizeof(f));
    CHECK(sys_netctl(NETCTL_IFACES, &f, sizeof(f)) == 0 && f.count == 2 && !strcmp(f.hostname, "surfos"));
    CHECK(!strcmp(f.iface[0].name, "eth0") && !f.iface[0].wifi && f.iface[0].link_up && f.iface[0].ip == 0x0f02000a);
    CHECK(f.iface[0].lease_s == 86400 && f.iface[0].lease_left == 86300 && f.iface[0].dhcp_server == 0x0202000a);
    CHECK(!strcmp(f.iface[1].name, "wlan0") && f.iface[1].wifi && f.iface[1].lease_s == 0 && f.iface[1].mac[0] == 2);
    eth.lease_at = now - 90000 * HZ;                                /* run out: none left, never a wrap */
    CHECK(sys_netctl(NETCTL_IFACES, &f, sizeof(f)) == 0 && f.iface[0].lease_left == 0);
    finish();
}

static void reads_without_memory(void) {
    struct netctl_wifi w;
    struct netctl_bss_list l;
    struct netctl_saved s;
    start();
    memset(&l, 0x77, sizeof(l));
    fail_in = 1;                                                    /* the kernel's copy */
    CHECK(sys_netctl(NETCTL_NETWORKS, &l, sizeof(l)) == -ENOMEM && (u8)((char *)&l)[0] == 0x77);
    fail_in = 2;                                                    /* the station's list */
    CHECK(sys_netctl(NETCTL_NETWORKS, &l, sizeof(l)) == -ENOMEM && (u8)((char *)&l)[0] == 0x77);
    fail_in = 2;
    memset(&s, 0x77, sizeof(s));
    CHECK(sys_netctl(NETCTL_SAVED, &s, sizeof(s)) == -ENOMEM && (u8)((char *)&s)[0] == 0x77);
    fail_in = 2;                                                    /* wifi status's text: the rest is there */
    CHECK(sys_netctl(NETCTL_WIFI, &w, sizeof(w)) == 0 && w.present && !w.state_text[0]);
    fail_in = 0;
    finish();
}

static void jobs_one_at_a_time(void) {
    struct netctl_job j;
    start();
    j = job_now();
    CHECK(j.seq == 0 || j.state == NETCTL_JOB_DONE);                /* nothing runs */
    CHECK(sys_netctl(NETCTL_SCAN, NULL, 0) > 0);
    j = job_now();
    CHECK(j.state == NETCTL_JOB_RUNNING && j.op == NETCTL_SCAN && j.stage == NETCTL_STAGE_SCANNING && threads_made == 1);
    CHECK(sys_netctl(NETCTL_SCAN, NULL, 0) == -EBUSY);             /* one at a time, whoever asks */
    CHECK(request(NETCTL_CARD, 0, NULL, NULL, NULL) == -EBUSY && request(NETCTL_SET, 0, NULL, "wifi", "off") == -EBUSY);
    CHECK(join("SurfSimWPA", "surfsim-wpa2-test-passphrase", 0) == -EBUSY);
    CHECK(sta.scans == 0 && threads_made == 1);
    run_thread();
    j = job_now();
    CHECK(j.state == NETCTL_JOB_DONE && j.result == 0 && !j.flags && sta.scans == 1 && !j.stage);
    CHECK(strstr(klog, "+NET: wsettings (pid 9): scan: done\n") != NULL);
    sta.scan_r = -ETIMEDOUT;                                        /* what it heard until then */
    CHECK(sys_netctl(NETCTL_SCAN, NULL, 0) == (int)j.seq + 1);
    run_thread();
    j = job_now();
    CHECK(j.result == 0 && j.flags == NETCTL_DONE_PARTIAL);
    sta.scan_r = -EBUSY;                                            /* a wifi command had the station */
    CHECK(sys_netctl(NETCTL_SCAN, NULL, 0) > 0);
    run_thread();
    j = job_now();
    CHECK(j.result == -EBUSY && strstr(klog, "scan: busy\n"));
    wdev.up = false;                                                /* the card off: wifi up first */
    CHECK(sys_netctl(NETCTL_SCAN, NULL, 0) == -ENODEV && sys_netctl(NETCTL_LEAVE, NULL, 0) == -ENODEV);
    have_wifi = false;
    CHECK(sys_netctl(NETCTL_SAVE, NULL, 0) == -ENODEV && request(NETCTL_CARD, 1, NULL, NULL, NULL) == -ENODEV);
    job_seq = SEQ_MAX;                                              /* numbers wrap to 1, never 0 or negative */
    have_wifi = true;
    wdev.up = true;
    sta.scan_r = 0;
    CHECK(sys_netctl(NETCTL_SCAN, NULL, 0) == 1);
    run_thread();
    thread_fail = 1;                                                /* no thread: the job ends at once */
    CHECK(sys_netctl(NETCTL_SCAN, NULL, 0) == -ENOMEM);
    j = job_now();
    CHECK(j.state == NETCTL_JOB_DONE && j.result == -ENOMEM && !strcmp(j.why, "no memory for the job's thread"));
    CHECK(sys_netctl(NETCTL_SCAN, NULL, 0) == -ENOMEM);            /* and the next start is not busy */
    finish();
}

static void join_wpa2(void) {
    struct netctl_join k;
    struct netctl_job j;
    struct netctl_wifi w;
    static const u8 zero[64];
    int seq;
    start();
    memset(&k, 0, sizeof(k));
    memcpy(k.ssid, "IEEE", 4);
    k.ssid_len = 4;
    k.flags = NETCTL_JOIN_SAVE;
    strcpy(k.passphrase, "password");
    seq = sys_netctl(NETCTL_JOIN, &k, sizeof(k));
    CHECK(seq > 0);
    CHECK(!memcmp(k.passphrase, zero, sizeof(k.passphrase)));     /* wiped in the program's memory at once */
    CHECK(!memcmp(req.pmk, ieee_pmk, 32) && req.has_pmk && req.save && !req.hidden);     /* the PMK, made at once */
    j = job_now();
    CHECK(j.state == NETCTL_JOB_RUNNING && j.stage == NETCTL_STAGE_JOINING && j.ssid_len == 4 && !memcmp(j.ssid, "IEEE", 4));
    CHECK(!has_bytes(&j, sizeof(j), ieee_pmk, 32) && !has_bytes(&j, sizeof(j), "password", 8));
    dhcp_fail = 2;                                                  /* the third try gets an address */
    run_thread();
    CHECK(sta.joins == 1 && sta.join_had_pmk && !memcmp(sta.join_pmk, ieee_pmk, 32));   /* the station got the key */
    CHECK(sta.join_quiet && sta.join_stops == 0 && sta.join_ssid_len == 4 && !memcmp(sta.join_ssid, "IEEE", 4));
    CHECK(save_calls == 1 && saved_net.security == NETCFG_SEC_WPA2 && saved_net.has_psk && !saved_net.has_password);
    CHECK(!memcmp(saved_net.psk, ieee_pmk, 32) && saved_net.ssid_len == 4 && !memcmp(saved_net.ssid, "IEEE", 4));
    CHECK(!memcmp(saved_net.password, zero, sizeof(saved_net.password)));
    CHECK(dhcp_calls == 3 && wnd.ip == 0x0f02000a);
    j = job_now();
    CHECK(j.seq == (unsigned)seq && j.state == NETCTL_JOB_DONE && j.result == 0);
    CHECK(j.flags == (NETCTL_DONE_SAVED | NETCTL_DONE_ADDRESS) && !j.why[0]);
    CHECK(!memcmp(req.pmk, zero, 32) && !req.has_pmk);             /* wiped at the job's end */
    CHECK(strstr(klog, "+NET: wsettings (pid 9): join \"IEEE\": done, an address, saved\n") != NULL);
    CHECK(!strstr(klog, "password") && !has_bytes(klog, sizeof(klog), ieee_pmk, 32));
    CHECK(sys_netctl(NETCTL_WIFI, &w, sizeof(w)) == 0 && !has_bytes(&w, sizeof(w), ieee_pmk, 32));

    /* the test network of tools/qemu-run.py, not saved; no address comes */
    wnd.ip = 0;
    memset(&k, 0, sizeof(k));
    memcpy(k.ssid, "SurfSimWPA", 10);
    k.ssid_len = 10;
    strcpy(k.passphrase, "surfsim-wpa2-test-passphrase");
    save_calls = dhcp_calls = 0;
    dhcp_fail = 5;
    CHECK(sys_netctl(NETCTL_JOIN, &k, sizeof(k)) > 0 && !memcmp(k.passphrase, zero, sizeof(k.passphrase)));
    run_thread();
    j = job_now();
    CHECK(j.result == 0 && j.flags == 0 && save_calls == 0 && dhcp_calls == 3);
    CHECK(sta.join_had_pmk && memcmp(sta.join_pmk, zero, 32) && memcmp(sta.join_pmk, ieee_pmk, 32));   /* its own key */
    finish();
}

static void join_refusals(void) {
    struct netctl_join k;
    static const u8 zero[64];
    start();
    CHECK(join("SurfSimWPA", "", 0) == -EACCES);                   /* WPA2 takes a password */
    CHECK(join("Twins", "", 0) == -EACCES);                        /* WPA2 first: never sent in the clear */
    CHECK(join("SurfSim", "a password", 0) == -EINVAL);            /* an open network takes none */
    CHECK(join("SurfSimWEP", "", 0) == -ENOSYS && join("SurfSimWEP", "abcdefgh", 0) == -ENOSYS);
    CHECK(join("SurfSimSAE", "abcdefgh", 0) == -ENOSYS);
    CHECK(join("SurfSimWPA", "short", 0) == -EINVAL);              /* 8 to 63 characters */
    CHECK(join("SurfSimWPA", "has a \x01 control", 0) == -EINVAL);
    CHECK(join("SurfSimWPA", "surfsim-wpa2-test-passphrase", 4) == -EINVAL);      /* flags that are none */
    memset(&k, 0, sizeof(k));
    memcpy(k.ssid, "SurfSimWPA", 10);
    k.ssid_len = 10;
    memset(k.passphrase, 'a', sizeof(k.passphrase));               /* no NUL: 64 characters */
    CHECK(sys_netctl(NETCTL_JOIN, &k, sizeof(k)) == -EINVAL && !memcmp(k.passphrase, zero, sizeof(k.passphrase)));
    memset(k.passphrase, 'a', 63);                                  /* 63 and a NUL: the most there is */
    k.passphrase[63] = 0;
    k.ssid_len = 0;
    CHECK(sys_netctl(NETCTL_JOIN, &k, sizeof(k)) == -EINVAL && !memcmp(k.passphrase, zero, sizeof(k.passphrase)));
    k.ssid_len = 33;
    strcpy(k.passphrase, "surfsim-wpa2-test-passphrase");
    CHECK(sys_netctl(NETCTL_JOIN, &k, sizeof(k)) == -EINVAL && !memcmp(k.passphrase, zero, sizeof(k.passphrase)));
    wdev.up = false;
    k.ssid_len = 10;
    strcpy(k.passphrase, "surfsim-wpa2-test-passphrase");
    CHECK(sys_netctl(NETCTL_JOIN, &k, sizeof(k)) == -ENODEV && !memcmp(k.passphrase, zero, sizeof(k.passphrase)));
    wdev.up = true;
    have_wifi = false;
    CHECK(join("SurfSimWPA", "surfsim-wpa2-test-passphrase", 0) == -ENODEV);
    have_wifi = true;
    fail_in = 1;                                                    /* no memory for the scan list */
    strcpy(k.passphrase, "surfsim-wpa2-test-passphrase");
    CHECK(sys_netctl(NETCTL_JOIN, &k, sizeof(k)) == -ENOMEM && !memcmp(k.passphrase, zero, sizeof(k.passphrase)));
    fail_in = 0;
    bad_lo = (const char *)&k + 50;                                 /* not the program's to write: nothing read */
    bad_hi = bad_lo + 1;
    strcpy(k.passphrase, "surfsim-wpa2-test-passphrase");
    CHECK(sys_netctl(NETCTL_JOIN, &k, sizeof(k)) == -EFAULT && k.passphrase[0] == 's');
    bad_lo = bad_hi = NULL;
    CHECK(threads_made == 0 && sta.joins == 0);
    /* busy: the PBKDF2 is not even made, and the password goes all the same */
    CHECK(sys_netctl(NETCTL_SCAN, NULL, 0) > 0);
    CHECK(sys_netctl(NETCTL_JOIN, &k, sizeof(k)) == -EBUSY && !memcmp(k.passphrase, zero, sizeof(k.passphrase)));
    run_thread();
    finish();
}

static void join_hidden_and_open(void) {
    struct netctl_job j;
    start();
    CHECK(join("Unheard", "surfsim-wpa2-test-passphrase", NETCTL_JOIN_SAVE) > 0);   /* not heard: looked for first */
    CHECK(req.unheard && !req.hidden && req.has_pmk && job_now().stage == NETCTL_STAGE_SCANNING && sta.scans == 0);
    run_thread();                                                   /* still not heard: hidden, WPA2 */
    CHECK(sta.scans == 1 && sta.join_stage == NETCTL_STAGE_JOINING);
    CHECK(sta.join_had_pmk && saved_net.hidden && saved_net.security == NETCFG_SEC_WPA2);
    CHECK(join("Unheard2", "", NETCTL_JOIN_SAVE) > 0);              /* not heard, no password: hidden, open */
    CHECK(req.unheard && !req.has_pmk);
    dhcp_calls = 0;
    sta.scan_r = -ETIMEDOUT;                                        /* a scan cut short: what it heard is in the list */
    run_thread();
    CHECK(sta.scans == 2 && !sta.join_had_pmk && saved_net.hidden && saved_net.security == NETCFG_SEC_OPEN &&
          !saved_net.has_psk);
    sta.scan_r = 0;
    j = job_now();
    CHECK(j.result == 0 && (j.flags & NETCTL_DONE_SAVED) && (j.flags & NETCTL_DONE_ADDRESS));
    CHECK(dhcp_calls == 0);                                         /* wlan0 kept its address: no DHCP again */
    sta.state = WLAN_STA_IDLE;
    CHECK(join("SurfSim", "", 0) > 0);                              /* open, heard: no scan first */
    CHECK(!req.unheard && !req.hidden && !req.has_pmk && job_now().stage == NETCTL_STAGE_JOINING);
    run_thread();
    CHECK(!sta.join_had_pmk && sta.security == WLAN_SEC_OPEN && sta.scans == 2);
    finish();
}

/* a name not heard in the last minute (the station's list forgets): the job's scan hears it, and the join is decided
   on what it heard, as wifi join decides after its scan; its refusals in words */
static void join_looked_for(void) {
    struct netctl_job j;
    static const u8 zero[32];
    start();
    add_bss("Later", WLAN_SEC_WPA2_PSK, -50, 6, false);            /* heard by the job's scan, not before */
    sta.heard = sta.list[--sta.nlist];
    sta.heard_by_scan = true;
    CHECK(join("Later", "surfsim-wpa2-test-passphrase", NETCTL_JOIN_SAVE) > 0 && req.unheard);
    run_thread();
    j = job_now();
    CHECK(j.result == 0 && (j.flags & NETCTL_DONE_SAVED) && sta.scans == 1 && sta.joins == 1);
    CHECK(sta.join_had_pmk && saved_net.security == NETCFG_SEC_WPA2 && !saved_net.hidden);    /* not hidden: heard */
    CHECK(sta.join_stage == NETCTL_STAGE_JOINING);
    sta.state = WLAN_STA_IDLE;
    add_bss("LaterOpen", WLAN_SEC_OPEN, -50, 6, false);            /* open: the password was a mistake */
    sta.heard = sta.list[--sta.nlist];
    sta.heard_by_scan = true;
    CHECK(join("LaterOpen", "surfsim-wpa2-test-passphrase", 0) > 0);
    run_thread();
    j = job_now();
    CHECK(j.result == -EINVAL && sta.joins == 1 && !memcmp(req.pmk, zero, 32) && !req.has_pmk);
    CHECK(!strcmp(j.why, "it is an open network, which takes no password: the password was not used"));
    add_bss("LaterWPA", WLAN_SEC_WPA2_WPA3, -50, 6, false);        /* WPA2, asked for with no password */
    sta.heard = sta.list[--sta.nlist];
    sta.heard_by_scan = true;
    CHECK(join("LaterWPA", "", 0) > 0);
    run_thread();
    j = job_now();
    CHECK(j.result == -EACCES && sta.joins == 1 && !strcmp(j.why, "it is a WPA2 network: join it with its password"));
    add_bss("LaterWEP", WLAN_SEC_WEP, -50, 6, false);
    sta.heard = sta.list[--sta.nlist];
    sta.heard_by_scan = true;
    CHECK(join("LaterWEP", "", 0) > 0);
    run_thread();
    j = job_now();
    CHECK(j.result == -ENOSYS && sta.joins == 1 && !strncmp(j.why, "it uses WEP, which SurfOS does not support", 42));
    add_bss("LaterSAE", WLAN_SEC_WPA3_SAE, -50, 6, false);
    sta.heard = sta.list[--sta.nlist];
    sta.heard_by_scan = true;
    CHECK(join("LaterSAE", "surfsim-wpa2-test-passphrase", 0) > 0);
    run_thread();
    j = job_now();
    CHECK(j.result == -ENOSYS && !strcmp(j.why, "it is WPA3-Personal only (SAE): not supported yet") &&
          !memcmp(req.pmk, zero, 32));
    sta.scan_r = -EBUSY;                                            /* the scan refused (a wifi command's): no join */
    CHECK(join("Elsewhere", "", 0) > 0);
    run_thread();
    j = job_now();
    CHECK(j.result == -EBUSY && sta.joins == 1 && !j.why[0]);
    CHECK(strstr(klog, "join \"LaterOpen\": ") && !strstr(klog, "surfsim-wpa2"));
    sta.scan_r = 0;
    finish();
}

static void join_failures(void) {
    struct netctl_job j;
    static const u8 zero[32];
    start();
    sta.join_r = -EACCES;                                           /* the 4-way handshake failed: the password */
    CHECK(join("SurfSimWPA", "not-the-surfsim-passphrase", NETCTL_JOIN_SAVE) > 0);
    run_thread();
    j = job_now();
    CHECK(j.result == -EACCES && !j.flags && save_calls == 0 && dhcp_calls == 0 && !memcmp(req.pmk, zero, 32));
    CHECK(strstr(klog, "join \"SurfSimWPA\": permission denied\n") && !strstr(klog, "not-the-surfsim"));
    sta.join_r = 0;                                                 /* a wifi leave since the request: called off */
    CHECK(join("SurfSimWPA", "surfsim-wpa2-test-passphrase", 0) > 0);
    sta.stops++;
    run_thread();
    j = job_now();
    CHECK(j.result == -ECANCELED && sta.state != WLAN_STA_UP);
    sta.other_join = true;                                          /* another join came meanwhile: no save, no DHCP */
    CHECK(join("SurfSimWPA", "surfsim-wpa2-test-passphrase", NETCTL_JOIN_SAVE) > 0);
    run_thread();
    j = job_now();
    CHECK(j.result == 0 && j.flags == NETCTL_DONE_NOT_SAVED && save_calls == 0 && dhcp_calls == 0);
    CHECK(!strcmp(j.why, "wlan0 was asked to join another network meanwhile"));
    sta.other_join = false;
    save_r = -EROFS;                                                /* the stick may not be written: joined all the same */
    CHECK(join("SurfSimWPA", "surfsim-wpa2-test-passphrase", NETCTL_JOIN_SAVE) > 0);
    run_thread();
    j = job_now();
    CHECK(j.result == 0 && j.flags == (NETCTL_DONE_NOT_SAVED | NETCTL_DONE_ADDRESS));
    CHECK(!strcmp(j.why, "the stick may not be written") && strstr(klog, "done, an address, not saved\n"));
    sta.no_link = true;                                             /* the link went before an address came */
    wnd.ip = 0;
    save_r = 0;
    dhcp_calls = 0;
    CHECK(join("SurfSim", "", 0) > 0);
    run_thread();
    j = job_now();
    CHECK(j.result == 0 && !(j.flags & NETCTL_DONE_ADDRESS) && dhcp_calls == 0);
    finish();
}

static void card(void) {
    struct netctl_job j;
    start();
    CHECK(request(NETCTL_CARD, 0, NULL, NULL, NULL) > 0);
    j = job_now();
    CHECK(j.stage == NETCTL_STAGE_STOPPING);
    run_thread();
    CHECK(sta.downs == 1 && !wdev.up && job_now().result == 0);
    CHECK(sys_netctl(NETCTL_SCAN, NULL, 0) == -ENODEV);
    /* on, the setting wifi on but nothing saved: the card alone */
    CHECK(request(NETCTL_CARD, 1, NULL, NULL, NULL) > 0 && job_now().stage == NETCTL_STAGE_STARTING);
    run_thread();
    CHECK(sta.ups == 1 && wdev.up && connect_calls == 0 && job_now().result == 0 && !job_now().flags);
    /* saved networks: what wifi connect does */
    nnets = 1;
    memcpy(nets[0].ssid, "SurfSim", 7);
    nets[0].ssid_len = 7;
    wdev.up = false;
    CHECK(request(NETCTL_CARD, 1, NULL, NULL, NULL) > 0);
    run_thread();
    j = job_now();
    CHECK(connect_calls == 1 && j.result == 0 && j.flags == (NETCTL_DONE_JOINED | NETCTL_DONE_ADDRESS));
    /* joined already: nothing more */
    CHECK(request(NETCTL_CARD, 1, NULL, NULL, NULL) > 0);
    run_thread();
    CHECK(connect_calls == 1 && sta.ups == 2);                     /* the card was on: not started again */
    /* none in range */
    sta.state = WLAN_STA_IDLE;
    connect_r = -ENOENT;
    CHECK(request(NETCTL_CARD, 1, NULL, NULL, NULL) > 0);
    run_thread();
    j = job_now();
    CHECK(connect_calls == 2 && j.result == 0 && j.flags == NETCTL_DONE_NONE_NEAR);
    CHECK(strstr(klog, "card on: done, no saved network in range\n") != NULL);
    /* the setting wifi off, or settings=off: the card alone */
    netcfg_set(&cfg, NETCFG_WIFI, "off", j.why, sizeof(j.why));
    CHECK(request(NETCTL_CARD, 1, NULL, NULL, NULL) > 0);
    run_thread();
    CHECK(connect_calls == 2);
    netcfg_set(&cfg, NETCFG_WIFI, "on", j.why, sizeof(j.why));
    settings_off = true;
    CHECK(request(NETCTL_CARD, 1, NULL, NULL, NULL) > 0);
    run_thread();
    CHECK(connect_calls == 2);
    /* the card does not start */
    settings_off = false;
    wdev.up = false;
    sta.up_r = -EIO;
    CHECK(request(NETCTL_CARD, 1, NULL, NULL, NULL) > 0);
    run_thread();
    CHECK(job_now().result == -EIO && connect_calls == 2);
    fail_in = 1;                                                    /* no memory to count the saved networks: the card */
    sta.up_r = 0;
    CHECK(request(NETCTL_CARD, 1, NULL, NULL, NULL) > 0);
    run_thread();
    CHECK(job_now().result == 0 && connect_calls == 2);
    finish();
}

static void leave_save_forget(void) {
    struct netctl_job j;
    static const u8 zero[32];
    start();
    CHECK(sys_netctl(NETCTL_SAVE, NULL, 0) > 0);                   /* not joined */
    run_thread();
    j = job_now();
    CHECK(j.result == -ENOTCONN && !strcmp(j.why, "wlan0 is not joined to a network") && save_calls == 0);
    sta.state = WLAN_STA_UP;                                        /* joined by a wifi join in a Terminal */
    memcpy(sta.ssid, "SurfSimWPA", 10);
    sta.ssid_len = 10;
    sta.security = WLAN_SEC_WPA2_PSK;
    sta.has_key = true;
    memset(sta.key, 0xab, 32);
    CHECK(sys_netctl(NETCTL_SAVE, NULL, 0) > 0);
    run_thread();
    CHECK(job_now().result == 0 && save_calls == 1 && saved_net.has_psk && saved_net.psk[0] == 0xab);
    sta.has_key = false;                                            /* WPA2 without a key kept: say so */
    CHECK(sys_netctl(NETCTL_SAVE, NULL, 0) > 0);
    run_thread();
    j = job_now();
    CHECK(j.result == -EACCES && save_calls == 1 && strstr(j.why, "keeps no key"));
    sta.security = WLAN_SEC_OPEN;
    CHECK(sys_netctl(NETCTL_SAVE, NULL, 0) > 0);
    run_thread();
    CHECK(job_now().result == 0 && saved_net.security == NETCFG_SEC_OPEN && !saved_net.has_psk);
    CHECK(!memcmp(saved_net.psk, zero, 32));
    CHECK(sys_netctl(NETCTL_LEAVE, NULL, 0) > 0 && job_now().stage == NETCTL_STAGE_LEAVING);
    run_thread();
    CHECK(job_now().result == 0 && sta.leaves == 1 && sta.idle == WLAN_IDLE_LEFT);
    /* forget: no device needed (the stick's list) */
    have_wifi = false;
    CHECK(request(NETCTL_FORGET, 0, "", NULL, NULL) == -EINVAL);
    CHECK(request(NETCTL_FORGET, 0, "SurfSimWPA", NULL, NULL) > 0);
    j = job_now();
    CHECK(j.stage == NETCTL_STAGE_WRITING && j.ssid_len == 10);
    run_thread();
    CHECK(job_now().result == 0 && forget_calls == 1 && forget_len == 10 && !memcmp(forget_ssid, "SurfSimWPA", 10));
    forget_r = -ENOENT;
    CHECK(request(NETCTL_FORGET, 0, "Nothing", NULL, NULL) > 0);
    run_thread();
    CHECK(job_now().result == -ENOENT && strstr(klog, "forget \"Nothing\": not found\n"));
    {
        struct netctl_req r;
        memset(&r, 0, sizeof(r));
        r.ssid_len = 33;
        CHECK(sys_netctl(NETCTL_FORGET, &r, sizeof(r)) == -EINVAL);
    }
    finish();
}

/* a step of the desktop's theme or pointer speed, in force at once, nothing written, no job */
static void apply(void) {
    struct netctl_req r;
    int before;
    start();
    apply_calls = 0;
    before = allocs;
    CHECK(request(NETCTL_APPLY, 0, NULL, "desktop.theme", "dusk") == 0 && cfg.theme == NETCFG_THEME_DUSK);
    CHECK(request(NETCTL_APPLY, 0, NULL, "Pointer.Speed", "fast") == 0 && cfg.pointer == 3);
    CHECK(apply_calls == 2 && threads_made == 0 && set_calls == 0 && allocs == before);   /* no job, no write */
    CHECK(request(NETCTL_APPLY, 0, NULL, "desktop.theme", "plaid") == -EINVAL && cfg.theme == NETCFG_THEME_DUSK);
    CHECK(request(NETCTL_APPLY, 0, NULL, "wifi", "off") == -EINVAL && cfg.wifi);  /* only the desktop's two keys */
    CHECK(request(NETCTL_APPLY, 0, NULL, "hostname", "laptop") == -EINVAL && request(NETCTL_APPLY, 0, NULL, "", "x") ==
          -EINVAL);
    CHECK(apply_calls == 3);                            /* the other keys never reach the network manager */
    settings_off = true;                                /* settings=off: nothing in force */
    CHECK(request(NETCTL_APPLY, 0, NULL, "desktop.theme", "ocean") == 1 && cfg.theme == NETCFG_THEME_DUSK);
    settings_off = false;
    memset(&r, 'x', sizeof(r));                         /* no NUL in the key */
    CHECK(sys_netctl(NETCTL_APPLY, &r, sizeof(r)) == -EINVAL);
    CHECK(sys_netctl(NETCTL_APPLY, &r, sizeof(r) - 1) == -EINVAL && sys_netctl(NETCTL_APPLY, NULL, sizeof(r)) == -EFAULT);
    CHECK(sys_netctl(NETCTL_SCAN, NULL, 0) > 0);        /* a job running: a step is no job, it is never busy */
    CHECK(request(NETCTL_APPLY, 0, NULL, "desktop.theme", "ocean") == 0 && cfg.theme == NETCFG_THEME_OCEAN);
    run_thread();
    finish();
}

static void set(void) {
    struct netctl_req r;
    struct netctl_job j;
    start();
    CHECK(request(NETCTL_SET, 0, NULL, "nosuchkey", "on") == -EINVAL);
    CHECK(request(NETCTL_SET, 0, NULL, "wifi", "maybe") == -EINVAL);          /* checked as the file would */
    CHECK(request(NETCTL_SET, 0, NULL, "wifi", "") == -EINVAL);
    CHECK(request(NETCTL_SET, 0, NULL, "hostname", "-bad") == -EINVAL);
    CHECK(request(NETCTL_SET, 0, NULL, "telnetd.port", "70000") == -EINVAL);
    memset(&r, 'x', sizeof(r));                                     /* no NUL in the key, or in the value */
    CHECK(sys_netctl(NETCTL_SET, &r, sizeof(r)) == -EINVAL);
    strcpy(r.key, "wifi");
    CHECK(sys_netctl(NETCTL_SET, &r, sizeof(r)) == -EINVAL);
    CHECK(set_calls == 0 && threads_made == 0);
    CHECK(request(NETCTL_SET, 0, NULL, "Hostname", "laptop") > 0); /* names as the command takes them */
    run_thread();
    j = job_now();
    CHECK(j.result == 0 && !j.flags && set_calls == 1 && set_k == NETCFG_HOSTNAME && !strcmp(set_value, "laptop"));
    CHECK(strstr(klog, "set hostname = laptop: done\n") != NULL && !strcmp(cfg.hostname, "laptop"));
    set_r = 1;                                                      /* in force, not on the stick */
    CHECK(request(NETCTL_SET, 0, NULL, "ethernet", "dhcp") > 0);
    run_thread();
    j = job_now();
    CHECK(j.result == 0 && j.flags == NETCTL_DONE_NOT_SAVED && !strcmp(j.why, "the stick could not be written"));
    set_r = -1;
    CHECK(request(NETCTL_SET, 0, NULL, "history", "on") > 0);
    run_thread();
    j = job_now();
    CHECK(j.result == -EINVAL && !strcmp(j.why, "refused by the network manager"));
    finish();
}

/* memory the program may read but not write: the reads and a join (whose password the call wipes) refused before
   anything is read or written; a request the call only reads taken */
static void read_only_memory(void) {
    static const int reads[6] = { NETCTL_WIFI, NETCTL_NETWORKS, NETCTL_SAVED, NETCTL_SETTINGS, NETCTL_IFACES, NETCTL_JOB };
    static const unsigned sizes[6] = { sizeof(struct netctl_wifi), sizeof(struct netctl_bss_list),
                                       sizeof(struct netctl_saved), sizeof(struct netctl_settings),
                                       sizeof(struct netctl_ifaces), sizeof(struct netctl_job) };
    static char big[4096];
    struct netctl_join *k = (struct netctl_join *)big;
    struct netctl_req *r = (struct netctl_req *)big;
    int i;
    start();
    ro_lo = big;
    ro_hi = big + sizeof(big);
    for(i = 0; i < 6; i++) {
        memset(big, 0x77, sizeof(big));
        CHECK(sys_netctl(reads[i], big, sizes[i]) == -EFAULT && (u8)big[0] == 0x77);
    }
    memset(big, 0, sizeof(big));
    memcpy(k->ssid, "SurfSimWPA", 10);
    k->ssid_len = 10;
    strcpy(k->passphrase, "surfsim-wpa2-test-passphrase");
    CHECK(sys_netctl(NETCTL_JOIN, k, sizeof(*k)) == -EFAULT && !strcmp(k->passphrase, "surfsim-wpa2-test-passphrase"));
    CHECK(threads_made == 0);
    memset(big, 0, sizeof(big));
    strcpy(r->key, "desktop.theme");
    strcpy(r->value, "dusk");
    CHECK(sys_netctl(NETCTL_APPLY, r, sizeof(*r)) == 0 && cfg.theme == NETCFG_THEME_DUSK);
    strcpy(r->key, "history");
    strcpy(r->value, "on");
    CHECK(sys_netctl(NETCTL_SET, r, sizeof(*r)) > 0);
    run_thread();
    CHECK(job_now().result == 0);
    ro_lo = ro_hi = NULL;
    finish();
}

/* 11/05/26 (the review, M2): an open network while a server without a password runs is refused unless the person was
   told (NETCTL_JOIN_EXPOSED): at the call, and by the job just before it joins (a server that started while it looked) */
static void join_exposed(void) {
    struct netctl_job j;
    start();
    telnet_exposed = true;
    CHECK(join("SurfSim", "", 0) == -EPERM && threads_made == 0 && sta.joins == 0);
    CHECK(join("Unheard", "", NETCTL_JOIN_SAVE) == -EPERM);        /* not heard, no password: open, refused too */
    CHECK(join("SurfSim", "", NETCTL_JOIN_EXPOSED | 4) == -EINVAL); /* flags that are none */
    CHECK(join("SurfSimWPA", "surfsim-wpa2-test-passphrase", 0) > 0);    /* WPA2: no matter */
    run_thread();
    j = job_now();
    CHECK(j.result == 0 && !(j.flags & NETCTL_DONE_EXPOSED) && sta.joins == 1);
    sta.state = WLAN_STA_IDLE;
    CHECK(join("SurfSim", "", NETCTL_JOIN_EXPOSED) > 0 && req.exposed_ok);   /* the person was told */
    run_thread();
    j = job_now();
    CHECK(j.result == 0 && (j.flags & NETCTL_DONE_EXPOSED) && sta.joins == 2 && !sta.join_had_pmk);
    CHECK(strstr(klog, ", open while a server without a password runs\n") != NULL);
    telnet_exposed = false;
    sta.state = WLAN_STA_IDLE;
    add_bss("LaterOpen", WLAN_SEC_OPEN, -50, 6, false);            /* heard by the job's scan only */
    sta.heard = sta.list[--sta.nlist];
    sta.heard_by_scan = true;
    CHECK(join("LaterOpen", "", 0) > 0 && req.unheard);
    ftp_exposed = true;                                             /* ftpd started while the job looked */
    run_thread();
    j = job_now();
    CHECK(j.result == -EPERM && (j.flags & NETCTL_DONE_EXPOSED) && sta.joins == 2 && sta.scans == 1);
    CHECK(!strcmp(j.why, "a server without a password runs (telnetd or ftpd), and anyone on an open network reaches it: "
                         "not joined without the person's word"));
    CHECK(strstr(klog, "join \"LaterOpen\": ") != NULL);
    CHECK(join("LaterOpen", "", NETCTL_JOIN_EXPOSED) > 0);         /* told: heard now, joined */
    run_thread();
    j = job_now();
    CHECK(j.result == 0 && (j.flags & NETCTL_DONE_EXPOSED) && sta.joins == 3);
    ftp_exposed = false;
    finish();
}

/* the n bytes s in the stack below the caller's frame, where the call it just made had its frames: what a missing wipe
   leaves there. Not inlined, so that its own frame lies over theirs; it only reads */
static __attribute__((noinline)) bool stack_holds(const void *s, size_t n) {
    volatile u8 probe[16384];
    const volatile u8 *p = probe;
    size_t i, j;
    __asm__ volatile("" : "+r"(p));                     /* where p points, forgotten: the bytes read as they are */
    for(i = 0; i + n <= sizeof(probe); i++) {
        for(j = 0; j < n && p[i + j] == ((const u8 *)s)[j]; j++) ;
        if(j == n) return true;
    }
    return false;
}

/* 11/05/26 (the review): the call's own copies on the kernel stack (start_join()'s k and r) are wiped: the password when
   the call refuses before the PMK is made (a job runs), the PMK once the job's record has it */
static void join_stack_wiped(void) {
    static const char pass[] = "stack-probe-passphrase-7Qx";      /* made up for this test */
    u8 pmk[32];
    int r1, r2;
    bool pass1, pass2, pmk2;
    start();
    CHECK(wpa_passphrase_to_pmk(pass, (const u8 *)"Probe", 5, pmk) == 0);
    add_bss("Probe", WLAN_SEC_WPA2_PSK, -50, 6, false);
    CHECK(sys_netctl(NETCTL_SCAN, NULL, 0) > 0);
    r1 = join("Probe", pass, 0);                                    /* busy: refused before the PBKDF2 */
    pass1 = stack_holds(pass, sizeof(pass) - 1);
    CHECK(r1 == -EBUSY && !pass1);
    run_thread();
    r2 = join("Probe", pass, 0);                                    /* started: the PMK made, handed to the record */
    pmk2 = stack_holds(pmk, sizeof(pmk));
    pass2 = stack_holds(pass, sizeof(pass) - 1);
    CHECK(r2 > 0 && !pmk2 && !pass2 && !memcmp(req.pmk, pmk, 32));
    run_thread();
    crypto_wipe(pmk, sizeof(pmk));
    finish();
}

int main(void) {
    sizes_and_pointers();
    reads();
    reads_without_memory();
    jobs_one_at_a_time();
    join_wpa2();
    join_refusals();
    join_hidden_and_open();
    join_looked_for();
    join_failures();
    card();
    leave_save_forget();
    apply();
    set();
    read_only_memory();
    join_exposed();
    join_stack_wiped();
    printf("test_netctl: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
