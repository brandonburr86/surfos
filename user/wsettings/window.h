/*
SurfOS user program: wsettings
--------------------
File: window.h  Date: 11/05/26 (the desktop applications, track settings)
--------------------
Settings' window (window.c) on the toolkit (gk.h): its widgets and what each event does to the
model (settings.c), apart from the loop (main.c), so that tests/host/test_wsettings_win.c drives
it on the fake desktop (tests/host/gk_fake.c) with a simulated kernel, as wkit's demo.c is driven.
*/
#ifndef _WSETTINGS_WINDOW_H
#define _WSETTINGS_WINDOW_H

#include <gk.h>
#include "settings.h"

#define WSW_W           600             /* the window's content, as it opens */
#define WSW_H           400
#define WSW_MIN_W       560
#define WSW_MIN_H       360
#define WSW_TICK_MS     250             /* the model's tick: the job looked at; the kernel read every WS_READ_MS */
#define WSW_ABOUT_ROWS  6

/* the ids of what the window hears from */
enum {
    ID_TABS = 1,
    ID_CARD, ID_LEAVE, ID_SAVE, ID_SCAN, ID_NETS, ID_JOIN, ID_OTHER, ID_SAVED, ID_FORGET, ID_STARTUP,     /* Wi-Fi */
    ID_ETHERNET, ID_IFACES, ID_HOST, ID_HOST_SET,                                                     /* Network */
    ID_THEME, ID_POINTER,                                                                             /* Desktop */
    ID_TELNETD, ID_HISTORY,                                                                           /* System */
    ID_ASK_NAME, ID_ASK_PASS, ID_ASK_SAVE, ID_ASK_YES, ID_ASK_NO,                                     /* the panel */
    ID_TEXT = 50,                                                       /* ID_TEXT + T_*: the texts drawn here */
    TIMER_TICK = 100,
    MENU_CLOSE = 200, MENU_PAGE, MENU_SCAN = MENU_PAGE + WS_PAGES, MENU_OTHER, MENU_LEAVE, MENU_ABOUT,
};

/* the texts the window wraps and draws itself, in areas */
enum { T_STATION, T_NETNOTE, T_ASK, T_TELNETD, T_HISTORY, T_WHERE, T_DESK, T_MSG, T_TEXTS };

struct wsw_text {
    struct gk_widget *w;
    struct gk *ui;
    char text[WS_STATUS + 200];
    int lines;                          /* at most: the last one cut with the desktop's dot */
    int band;                           /* drawn as a status bar: the band colour and a line above */
    int muted;
};

struct wswin {
    struct gk *ui;
    struct wsettings m;
    void (*about)(struct ws_about *ab, int all);   /* the About page's figures: main.c's sys.c, or a test's */
    struct gk_widget *main, *panel, *tabs;
    struct gk_widget *card, *leave, *save, *scan, *nets, *join, *other, *saved, *forget, *startup;
    struct gk_widget *ethernet, *ifaces, *host, *host_set;
    struct gk_widget *theme_row, *theme, *pointer_row, *pointer;
    struct gk_widget *telnetd, *history;
    struct gk_widget *about_value[WSW_ABOUT_ROWS];
    struct gk_widget *ask_title, *ask_name_row, *ask_name, *ask_pass_row, *ask_pass, *ask_save, *ask_yes, *ask_no;
    struct wsw_text text[T_TEXTS];
    char themes[WS_CHOICES][NETCTL_VALUE_MAX], speeds[WS_CHOICES][NETCTL_VALUE_MAX];
    int nthemes, nspeeds;
    char host_shown[NETCTL_VALUE_MAX];  /* the host name last put in the field: it follows the setting while it is so */
    int asked;                          /* the question the panel shows (WS_ASK_*), WS_ASK_NONE */
    unsigned about_at;                  /* when the About page's figures were last read */
    char title[64];
};

int wsw_open(struct wswin *s, ws_netctl_fn netctl, void *ctx, void (*about)(struct ws_about *ab, int all));
int wsw_event(struct wswin *s, const struct gk_event *ev);     /* 0 once the window is to close */
void wsw_close(struct wswin *s);

#endif
