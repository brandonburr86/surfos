/*
SurfOS user program: wsettings
--------------------
File: settings.h  Date: 11/05/26 (the desktop applications, track settings)
--------------------
Settings' model (settings.c): its five pages, what each shows and what each of its controls asks of
the kernel, through SYS_netctl (include/surfos/netctl.h) handed in as a function, so that
tests/host/test_wsettings.c runs it on the host against a simulated kernel. No drawing and no system
call here: main.c draws the pages with the toolkit and hands in the time, the About page's figures
and the kernel's call.
*/
#ifndef _WSETTINGS_SETTINGS_H
#define _WSETTINGS_SETTINGS_H

#include <surfos/netctl.h>

#define WS_WIFI         0               /* the pages */
#define WS_NETWORK      1
#define WS_DESKTOP      2
#define WS_SYSTEM       3
#define WS_ABOUT        4
#define WS_PAGES        5

#define WS_STATUS       400             /* what the window says last: a sentence or two, wrapped by the window */
#define WS_NAME         (4 * NETCTL_SSID_MAX + 1)   /* an SSID as text: every byte \xHH at worst */
#define WS_LINE         200
#define WS_READ_MS      2000            /* the kernel's answers are read again so often */
#define WS_CHOICES      8
#define WS_STEPS        2               /* settings stepped through and not yet written: the theme, the pointer speed */
#define WS_WRITE_AFTER_MS 1500          /* a stepped setting is written once its steps have stopped for so long */

/* the kernel's call: SYS_netctl, or the tests' simulation. Its answer: >= 0, or -errno */
typedef int (*ws_netctl_fn)(void *ctx, int op, void *arg, unsigned size);

/* a row of the networks in range: the access points of one name together */
struct ws_net {
    unsigned char ssid[NETCTL_SSID_MAX];
    int ssid_len;
    int security;                       /* what a join uses: WPA2 if any of them has it, else open if one is, else the
                                           strongest one's (the kernel decides so too) */
    int rssi;                           /* the strongest one's, dBm */
    int band;                           /* the strongest one's: 0 2.4 GHz, 1 5 GHz */
    int count;                          /* access points */
    int saved;                          /* a saved network has that name */
    int joined;                         /* wlan0 is joined to it */
};

/* what the About page shows, read by main.c (sysinfo(), /initrd/etc/build-id, CPUID) */
struct ws_about {
    char build[96];                     /* the build id's line, "" for none */
    char cpu[64];                       /* the processor's name, "" when not known */
    unsigned mem_kb, free_kb, programs_kb, heap_kb;
    unsigned uptime_s;
    int have_mem;
};

/* a question in the window (an in-window box) and what it is about */
#define WS_ASK_NONE     0
#define WS_ASK_PASSWORD 1               /* a WPA2 network's password (a field that never shows its text), Save it */
#define WS_ASK_OTHER    2               /* a network by its name, its password ("" for an open one), Save it */
#define WS_ASK_OPEN     3               /* an open network while a server without a password runs: join anyway? */
#define WS_ASK_FORGET   4               /* forget a saved network? */
#define WS_ASK_TELNETD  5               /* telnetd at startup on? the shell's warning first */
#define WS_ASK_CLOSE    6               /* the host name typed and not set: close all the same? */

struct ws_ask {
    int kind;
    unsigned char ssid[NETCTL_SSID_MAX];    /* the network it is about */
    int ssid_len;
    int save;                           /* the box's Save it, as it starts */
    char title[48];
    char text[480];
    char yes[24];                       /* its buttons' words */
    char no[24];
};

struct wsettings {
    ws_netctl_fn netctl;
    void *ctx;
    int page;
    unsigned now;                       /* milliseconds since boot, from main.c before each call */
    /* the kernel's answers: have_* when the last read of each worked */
    struct netctl_wifi wifi;
    struct netctl_bss_list bss;
    struct netctl_saved saved;
    struct netctl_settings settings;
    struct netctl_ifaces ifaces;
    int have_wifi, have_bss, have_saved, have_settings, have_ifaces;
    int no_call;                        /* the kernel has no SYS_netctl: an older SurfOS */
    int read_once;
    unsigned read_at;
    /* the networks in range, a row per name, strongest first; the access points that carry no name */
    struct ws_net nets[NETCTL_BSS_MAX];
    int nnets, nameless;
    /* the lists' selections, by name, so that they stay on a network when the list moves */
    unsigned char net_sel[NETCTL_SSID_MAX], saved_sel[NETCTL_SSID_MAX];
    int net_sel_len, saved_sel_len;     /* -1: none */
    /* the job this program started (job 0: none runs) */
    int job, job_op, job_stage, job_quiet;
    unsigned char job_ssid[NETCTL_SSID_MAX];
    int job_ssid_len;
    char job_key[NETCTL_KEY_MAX], job_value[NETCTL_VALUE_MAX];
    int scanned;                        /* a scan was asked for (1), one is to be by itself (0, -1) */
    struct {                            /* settings in force by their last step (NETCTL_APPLY), to be written (key "": none) */
        char key[NETCTL_KEY_MAX], value[NETCTL_VALUE_MAX];
        unsigned at;                    /* the last step's time */
    } step[WS_STEPS];
    int closing;                        /* closing waits for a job to end, to write the last steps */
    int refused;                        /* the kernel's last refusal of a job's start (-errno), 0 */
    struct ws_ask ask;
    char status[WS_STATUS];
    int said_scan;                      /* status is a scan's count of networks, which a minute unheard makes old */
    struct ws_about about;
};

/* what a call asks main.c to do besides drawing */
#define WS_DRAW         1               /* draw again */
#define WS_CLOSE        2               /* the window closes */

void ws_init(struct wsettings *m, ws_netctl_fn netctl, void *ctx);
int ws_refresh(struct wsettings *m);    /* everything read again now: WS_DRAW when what the page shows changed */
int ws_tick(struct wsettings *m);       /* every pass: the job looked at, the answers read every WS_READ_MS */
int ws_busy(const struct wsettings *m); /* a job of this program's runs */
int ws_page(struct wsettings *m, int page);
const char *ws_page_name(int page);
void ws_title(const struct wsettings *m, char *out, int n);
int ws_close(struct wsettings *m, const char *typed_host);  /* the close button: WS_CLOSE, or WS_DRAW (asks) */

/**** Wi-Fi ****/
int ws_card(struct wsettings *m, int on);
int ws_scan(struct wsettings *m);
int ws_select_net(struct wsettings *m, int i);
int ws_net_selected(const struct wsettings *m);         /* its row, -1 */
int ws_select_saved(struct wsettings *m, int i);
int ws_saved_selected(const struct wsettings *m);       /* its row, -1 */
int ws_can_join(const struct wsettings *m, int i);      /* the card on, a security SurfOS joins, not joined to it now */
int ws_join(struct wsettings *m, int i);                /* the row's network: joined at once, or asked about */
int ws_join_other(struct wsettings *m);                 /* a network by its name: asked for */
/* the answer to WS_ASK_PASSWORD or WS_ASK_OTHER: name (OTHER's, name_len bytes of UTF-8), password (NUL-terminated,
   wiped here whatever happens), save. The box stays when the answer is no answer (WS_DRAW and its text says why) */
int ws_join_answer(struct wsettings *m, const char *name, int name_len, char *password, int save);
int ws_leave(struct wsettings *m);
int ws_save_joined(struct wsettings *m);
int ws_joined_saved(const struct wsettings *m);         /* the network joined now is a saved one */
int ws_forget(struct wsettings *m, int i);              /* the saved row's network: asked about first */
int ws_answer(struct wsettings *m, int yes);            /* the other boxes' Yes or No */
void ws_wifi_line(const struct wsettings *m, char *out, int n);    /* the card and the network joined */
/* the lists' rows, their columns apart at tabs: a network in range (its name, the signal in four bars and in dBm, the
   security, joined, saved or not supported), a saved network (its name, its security, its state as wifi saved says it) */
void ws_net_row(const struct wsettings *m, int i, char *out, int n);
void ws_saved_row(const struct wsettings *m, int i, char *out, int n);
void ws_net_note(const struct wsettings *m, int i, char *out, int n);    /* why a row cannot be joined, "" */
int ws_bars(int rssi);                                  /* the signal in four bars: 0 to 4 */

/**** the settings ****/
const struct netctl_setting *ws_setting(const struct wsettings *m, const char *name);    /* NULL: not in the table */
int ws_is_on(const struct wsettings *m, const char *name);
int ws_choices(const struct netctl_setting *s, char out[][NETCTL_VALUE_MAX], int max);  /* "a | b | c" taken apart */
int ws_set(struct wsettings *m, const char *name, const char *value);  /* telnetd on asks first */
/* a step of a setting a person steps through (the desktop's theme, its pointer speed: radio groups whose arrows repeat):
   in force at once, nothing written; written once its steps stop for WS_WRITE_AFTER_MS, and before the window closes */
int ws_choose(struct wsettings *m, const char *name, const char *value);
int ws_toggle(struct wsettings *m, const char *name);   /* an on | off setting the other way */
int ws_host_ok(const char *name, char *why, int n);     /* a host name the table takes (as net/netcfg.c's rule) */
int ws_set_host(struct wsettings *m, const char *name);
void ws_iface_lines(const struct wsettings *m, int i, char *a, int an, char *b, int bn, char *c, int cn);
void ws_about_line(const struct wsettings *m, int k, char *label, int ln, char *value, int vn);   /* k 0-5 */

/**** words ****/
void ws_ssid_text(const unsigned char *ssid, int len, char *out, int n);
const char *ws_security_name(int security);             /* the wifi command's names */
const char *ws_security_short(int security);            /* a list's: open, WPA2, ... */
void ws_ip(unsigned ip, char *out, int n);              /* network byte order, as the kernel keeps it */
void ws_duration(unsigned s, char *out, int n);         /* 2 h 05 min, 3 min 20 s, 1 day 4 h */
void ws_size_kb(unsigned kb, char *out, int n);         /* 640 KB, 31.5 MB, 2.0 GB */
/* where text's line-th line starts wrapped at spaces to cells columns (a UTF-8 character a cell; a longer word cut, a
   newline a line's end), its bytes in *len; NULL past the last; and how many lines there are */
const char *ws_wrap(const char *text, int cells, int line, int *len);
int ws_wrap_lines(const char *text, int cells);

#endif
