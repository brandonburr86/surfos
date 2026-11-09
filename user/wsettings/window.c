/*
SurfOS user program: wsettings
--------------------
File: window.c  Date: 11/05/26 (the desktop applications, track settings)
--------------------
Settings' window on the toolkit (gk.h): five tabs over a band that says what the window last did,
and a question panel that takes the tabs' place while the model asks something.

    Wi-Fi     the card (a switch), the network joined with its address, Disconnect and Save; the
              networks in range (a list: name, signal in bars and dBm, security, joined or saved),
              Scan, Join and By name; why the network chosen cannot be joined; the saved networks
              (a list: name, security, state) and Forget; joining at startup (a switch)
    Network   Ethernet's DHCP (a switch); every interface's addresses and lease (a list of lines);
              the host name (a field, and Set)
    Desktop   the theme and the pointer's speed (radio groups), once the kernel's table has them
    System    telnetd at startup and the shell's history on the stick (switches), each with what it
              means; where the settings are kept
    About     the version, the build, the memory, the programs', the time since the start, and the
              processor

The question panel asks for a WPA2 network's password, in a field that shows a dot for each
character, copies nothing and is wiped the moment its text has been handed on (to the model, which
hands it to the kernel and wipes its copies) or the question goes; for a network by its name; and
whether to go on: joining an open network while a server without a password runs, forgetting a
saved network, telnetd at startup (the settings command's warning in full), closing with a host
name typed and not set. Its words are wrapped to the window, so that no warning is ever cut, which
the toolkit's own boxes (two lines) would do.

Each event goes to the model (settings.c), and then the widgets are made to say what the model
says (show_model()): a widget is touched only when what it shows changed, so the toolkit presents only
that. A timer ticks the model every WSW_TICK_MS (the job looked at; the kernel read every
WS_READ_MS). While a job of this program's runs, what would start another is greyed, and a switch
shows the value asked for. The menus: File > Close (Ctrl+W); View, the pages (F5 to F9); Wi-Fi,
Scan (Ctrl+R), Join a network by its name (Ctrl+J), Disconnect; Help > About Settings. Esc answers
No to a question, and otherwise closes the window as its close button does.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <surf.h>
#include "window.h"

#define LABEL_W 120                     /* the labels before a field or a group: "Pointer speed:" */

static void wipe(void *p, unsigned n) {
    volatile unsigned char *q = (volatile unsigned char *)p;
    while(n--) *q++ = 0;
}

/**** widgets changed only when what they show changes ****/

static void set_text(struct gk_widget *w, const char *t) {
    if(w && strcmp(gk_text(w), t)) gk_set_text(w, t);
}

static void set_value(struct gk_widget *w, int v) {
    if(w && gk_value(w) != v) gk_set_value(w, v);
}

static void enable(struct gk_widget *w, int on) {
    if(w && !gk_enabled(w) != !on) gk_enable(w, on);
}

static void show(struct gk_widget *w, int on) {
    if(w && !gk_shown(w) != !on) gk_show(w, on);
}

/**** the texts drawn here: wrapped to the area's width, the last line cut with the desktop's dot ****/

static void draw_text(struct gk_widget *area, struct gw *win, const struct gk_view *v, void *data) {
    const struct wsw_text *t = (const struct wsw_text *)data;
    const struct gk_theme *th = gk_theme(t->ui);
    char line[sizeof(t->text) + 4];
    const char *p;
    int x = t->band ? 10 : 0, y = t->band ? 4 : 2, cells = (v->w - 2 * x) / 8, i, len, more;
    (void)area;
    gw_rect(win, v->cx, v->cy, v->cw, v->ch, t->band ? th->band : th->paper);
    if(t->band) gw_rect(win, 0, 0, v->w, 1, th->line);
    for(i = 0; i < t->lines && (p = ws_wrap(t->text, cells, i, &len)) != NULL; i++) {
        memcpy(line, p, (size_t)len);
        line[len] = 0;
        more = i == t->lines - 1 && ws_wrap(t->text, cells, i + 1, &len) != NULL;
        if(more) strcat(line, "\xe2\x88\x99");          /* more than fits: the dot, as the desktop cuts names */
        gw_text_cut(win, x, y + i * 16, line, cells, t->muted ? th->muted : th->ink);
    }
}

static struct gk_widget *text_area(struct wswin *s, int k, struct gk_widget *parent, int lines, int band, int muted) {
    struct wsw_text *t = &s->text[k];
    t->ui = s->ui;
    t->lines = lines;
    t->band = band;
    t->muted = muted;
    t->w = gk_area(parent, ID_TEXT + k, 0, draw_text, t);
    gk_size(t->w, 0, lines * 16 + (band ? 8 : 4));
    gk_grow(t->w, 1, 0);
    return t->w;
}

static void text_set(struct wswin *s, int k, const char *text) {
    struct wsw_text *t = &s->text[k];
    if(!strcmp(t->text, text)) return;
    snprintf(t->text, sizeof(t->text), "%s", text);
    gk_redraw(t->w);
}

/**** what the window shows, from the model ****/

/* a setting's value as the window shows it: the newest step, in force and waiting to be written (the review's L3);
   else the one asked for while its change runs */
static const char *shown(const struct wsettings *m, const char *key) {
    const struct netctl_setting *e = ws_setting(m, key);
    int i;
    for(i = 0; i < WS_STEPS; i++) if(m->step[i].key[0] && !strcmp(m->step[i].key, key)) return m->step[i].value;
    if(m->job && m->job_op == NETCTL_SET && !strcmp(m->job_key, key)) return m->job_value;
    return e ? e->value : "";
}

static int card_shown(const struct wsettings *m) {
    if(m->job && m->job_op == NETCTL_CARD) return m->job_value[0] == '1';
    return m->have_wifi && m->wifi.present && m->wifi.up;
}

/* a list's rows made what row() says, row by row, only those that changed */
static void list_sync(struct gk_widget *list, int count, void (*row)(const struct wsettings *, int, char *, int),
                      const struct wsettings *m) {
    char b[WS_STATUS];
    const char *t;
    int i, have = gk_list_count(list);
    for(i = 0; i < count; i++) {
        row(m, i, b, sizeof(b));
        if(i >= have) gk_list_add(list, b);
        else if(!(t = gk_list_text(list, i)) || strcmp(t, b)) gk_list_set(list, i, b);
    }
    for(i = have - 1; i >= count; i--) gk_list_remove(list, i);
}

/* the interfaces' lines: three each (ws_iface_lines()), the second and third indented, a blank line between them */
static void iface_row(const struct wsettings *m, int i, char *out, int n) {
    char a[WS_LINE], b[WS_LINE], c[WS_LINE];
    out[0] = 0;
    ws_iface_lines(m, i / 4, a, sizeof(a), b, sizeof(b), c, sizeof(c));
    switch(i % 4) {
    case 0: snprintf(out, (size_t)n, "%s", a); break;
    case 1: snprintf(out, (size_t)n, "    %s", b); break;
    case 2: if(c[0]) snprintf(out, (size_t)n, "    %s", c); break;
    }
}

static void about_row(struct wswin *s) {
    char label[24], value[WS_LINE];
    int k;
    for(k = 0; k < WSW_ABOUT_ROWS; k++) {
        ws_about_line(&s->m, k, label, sizeof(label), value, sizeof(value));
        set_text(s->about_value[k], value);
    }
}

/* the choices of a setting as the radio group shows them: its words with a capital */
static void radio_fill(struct gk_widget *radio, char choices[][NETCTL_VALUE_MAX], int n) {
    char b[NETCTL_VALUE_MAX];
    int i;
    for(i = 0; i < n; i++) {
        snprintf(b, sizeof(b), "%s", choices[i]);
        if(b[0] >= 'a' && b[0] <= 'z') b[0] = (char)(b[0] - 32);
        gk_radio_add(radio, b);
    }
}

static int choice_of(char choices[][NETCTL_VALUE_MAX], int n, const char *value) {
    int i;
    for(i = 0; i < n; i++) if(!strcmp(choices[i], value)) return i;
    return -1;
}

static void desktop_sync(struct wswin *s) {
    const struct wsettings *m = &s->m;
    const struct netctl_setting *t = ws_setting(m, "desktop.theme"), *p = ws_setting(m, "pointer.speed");
    if(t && !s->nthemes && (s->nthemes = ws_choices(t, s->themes, WS_CHOICES)) > 0) radio_fill(s->theme, s->themes,
                                                                                               s->nthemes);
    if(p && !s->nspeeds && (s->nspeeds = ws_choices(p, s->speeds, WS_CHOICES)) > 0) radio_fill(s->pointer, s->speeds,
                                                                                               s->nspeeds);
    show(s->theme_row, t && s->nthemes);
    show(s->pointer_row, p && s->nspeeds);
    if(t) set_value(s->theme, choice_of(s->themes, s->nthemes, shown(m, "desktop.theme")));
    if(p) set_value(s->pointer, choice_of(s->speeds, s->nspeeds, shown(m, "pointer.speed")));
    enable(s->theme, t != NULL);                        /* never greyed by a job: a step needs none (ws_choose()) */
    enable(s->pointer, p != NULL);
    text_set(s, T_DESK, t || p ? "They act at once, and are kept on the stick SurfOS was started from, as every setting is "
             "(System says where)." : "This SurfOS keeps no desktop settings: its network manager's table has no "
             "desktop.theme or pointer.speed.");
}

static void system_sync(struct wswin *s) {
    const struct wsettings *m = &s->m;
    const struct netctl_setting *port = ws_setting(m, "telnetd.port");
    char b[WS_STATUS + 160];
    int e = m->have_settings ? m->settings.exposed : 0;
    set_value(s->telnetd, !strcmp(shown(m, "telnetd"), "on"));
    set_value(s->history, !strcmp(shown(m, "history"), "on"));
    enable(s->telnetd, ws_setting(m, "telnetd") && !m->job);
    enable(s->history, ws_setting(m, "history") && !m->job);
    snprintf(b, sizeof(b), "It has no password and no encryption, and starts only for an address on Ethernet or a WPA2 "
             "network; it listens on port %s (settings telnetd.port in a Terminal changes it).%s", port ? port->value :
             "23", e & NETCTL_EXPOSED_TELNET ? " A telnet server runs now, or is to start." : "");
    text_set(s, T_TELNETD, b);
    text_set(s, T_HISTORY, "surfsh keeps the lines typed at its prompts in surfos-config/history, as plain text (a line "
             "that starts with a space is never kept), and recalls them at its next start.");
    if(!m->have_settings) snprintf(b, sizeof(b), "%s", m->no_call ? "This SurfOS has no network settings for programs "
                                   "(SYS_netctl)." : "The settings could not be read.");
    else snprintf(b, sizeof(b), "The settings: %s.%s", m->settings.where, m->settings.off ? " This start does not use "
                  "them (settings=off): a change is for the next." : !m->settings.can_save ? " This stick is only read: "
                  "a change lasts until SurfOS restarts." : "");
    text_set(s, T_WHERE, b);
    show(s->save_again, ws_unsaved(m) > 0);             /* in force here, not on the stick: written again on a click */
    enable(s->save_again, !m->job);
}

static void wifi_sync(struct wswin *s) {
    const struct wsettings *m = &s->m;
    char b[WS_STATUS];
    int sel, up = m->have_wifi && m->wifi.present && m->wifi.up, joined = up && m->wifi.state == NETCTL_STA_UP;
    int linked = up && (m->wifi.state == NETCTL_STA_UP || m->wifi.state == NETCTL_STA_JOINING ||
                        m->wifi.state == NETCTL_STA_RECONNECTING);
    set_value(s->card, card_shown(m));
    enable(s->card, m->have_wifi && m->wifi.present && !m->job);
    ws_wifi_line(m, b, sizeof(b));
    text_set(s, T_STATION, b);
    show(s->leave, linked);
    enable(s->leave, linked && !m->job);
    show(s->save, joined && !ws_joined_saved(m));
    enable(s->save, joined && !m->job && m->have_settings && m->settings.can_save);
    enable(s->scan, up && !m->job);
    enable(s->other, up && !m->job);
    list_sync(s->nets, m->nnets, ws_net_row, m);
    sel = ws_net_selected(m);
    set_value(s->nets, sel);
    enable(s->join, ws_can_join(m, sel));
    ws_net_note(m, sel, b, sizeof(b));
    if(!b[0] && m->nameless == 1) snprintf(b, sizeof(b), "%s", "A network nearby hides its name: By name joins it.");
    else if(!b[0] && m->nameless)
        snprintf(b, sizeof(b), "%d networks nearby hide their names: By name joins one.", m->nameless);
    if(!b[0] && up && !m->nnets && !m->job)
        snprintf(b, sizeof(b), "%s", m->scanned > 0 ? "Nothing heard in the last minute: Scan looks again." :
                 "No networks heard yet: Scan looks for them.");
    text_set(s, T_NETNOTE, b);
    list_sync(s->saved, m->have_saved ? (int)m->saved.count : 0, ws_saved_row, m);
    set_value(s->saved, ws_saved_selected(m));
    enable(s->forget, ws_saved_selected(m) >= 0 && !m->job);
    set_value(s->startup, !strcmp(shown(m, "wifi"), "on"));
    enable(s->startup, ws_setting(m, "wifi") && !m->job);
}

static void network_sync(struct wswin *s) {
    const struct wsettings *m = &s->m;
    const struct netctl_setting *h = ws_setting(m, "hostname");
    set_value(s->ethernet, !strcmp(shown(m, "ethernet"), "dhcp"));
    enable(s->ethernet, ws_setting(m, "ethernet") && !m->job);
    list_sync(s->ifaces, m->have_ifaces ? 4 * (int)m->ifaces.count : 0, iface_row, m);
    if(h && strcmp(h->value, s->host_shown) && !strcmp(gk_text(s->host), s->host_shown)) {
        set_text(s->host, h->value);                    /* the setting moved, and nobody typed over the field */
        snprintf(s->host_shown, sizeof(s->host_shown), "%s", h->value);
    }
    enable(s->host, h != NULL);
    enable(s->host_set, h && !m->job && strcmp(gk_text(s->host), h->value));
}

/* the question panel: shown while the model asks, filled as it asks; the password field wiped when it goes */
static void panel_sync(struct wswin *s) {
    const struct ws_ask *a = &s->m.ask;
    int join = a->kind == WS_ASK_PASSWORD || a->kind == WS_ASK_OTHER;
    if(a->kind == s->asked) {
        if(a->kind) text_set(s, T_ASK, a->text);         /* the model's words may change: a password too short */
        return;
    }
    gk_field_wipe(s->ask_pass);
    set_text(s->ask_name, "");
    s->asked = a->kind;
    if(!a->kind) {
        set_value(s->main, 0);
        return;
    }
    set_text(s->ask_title, a->title);
    text_set(s, T_ASK, a->text);
    show(s->ask_name_row, a->kind == WS_ASK_OTHER);
    show(s->ask_pass_row, join);
    show(s->ask_save, join);
    set_value(s->ask_save, a->save);
    enable(s->ask_save, s->m.have_settings && s->m.settings.can_save);
    set_text(s->ask_yes, a->yes);
    set_text(s->ask_no, a->no);
    set_value(s->main, 1);
    gk_focus(a->kind == WS_ASK_OTHER ? s->ask_name : join ? s->ask_pass : s->ask_no);  /* a question's safe answer first */
}

static void show_model(struct wswin *s) {
    const struct wsettings *m = &s->m;
    char b[64];
    int k;
    ws_title(m, b, sizeof(b));
    if(strcmp(b, s->title)) {
        gw_title(gk_gw(s->ui), b);
        snprintf(s->title, sizeof(s->title), "%s", b);
    }
    set_value(s->tabs, m->page);
    for(k = 0; k < WS_PAGES; k++) gk_menu_check(s->ui, MENU_PAGE + k, k == m->page);
    panel_sync(s);
    if(s->asked == WS_ASK_PASSWORD || s->asked == WS_ASK_OTHER)    /* by name: WPA2 once a password is typed */
        set_text(s->ask_save, ws_save_label(m, s->asked == WS_ASK_PASSWORD || gk_text(s->ask_pass)[0]));
    enable(s->ask_yes, !(m->job && ws_answer_starts(m)));  /* a job runs: the answer waits, the box stays (L2) */
    wifi_sync(s);
    network_sync(s);
    desktop_sync(s);
    system_sync(s);
    about_row(s);
    gk_menu_enable(s->ui, MENU_SCAN, gk_enabled(s->scan));
    gk_menu_enable(s->ui, MENU_OTHER, gk_enabled(s->other));
    gk_menu_enable(s->ui, MENU_LEAVE, gk_shown(s->leave) && gk_enabled(s->leave));
    /* the band: the model's last words, or what a job it started by itself does */
    if(!m->status[0] && m->job && m->job_op == NETCTL_SCAN) text_set(s, T_MSG, "Looking for networks...");
    else text_set(s, T_MSG, m->status[0] ? m->status : "Ready");
}

/**** events ****/

/* the panel's first answer: a join's, with the password taken out of its field at once; or Yes */
static int panel_yes(struct wswin *s) {
    char pass[NETCTL_PASS_MAX + 4], name[NETCTL_SSID_MAX + 4];
    int r, kind = s->m.ask.kind;
    if(kind != WS_ASK_PASSWORD && kind != WS_ASK_OTHER) return ws_answer(&s->m, 1);
    snprintf(name, sizeof(name), "%s", gk_text(s->ask_name));
    snprintf(pass, sizeof(pass), "%s", gk_text(s->ask_pass));
    gk_field_wipe(s->ask_pass);                         /* the field's copy goes before anything else happens */
    r = ws_join_answer(&s->m, name, (int)strlen(name), pass, gk_value(s->ask_save));   /* it wipes pass too */
    wipe(pass, sizeof(pass));
    if(s->m.ask.kind == kind) gk_focus(kind == WS_ASK_OTHER && !name[0] ? s->ask_name : s->ask_pass);    /* asked again */
    return r;
}

static int clicked(struct wswin *s, int id) {
    struct wsettings *m = &s->m;
    switch(id) {
    case ID_LEAVE:    return ws_leave(m);
    case ID_SAVE:     return ws_save_joined(m);
    case ID_SCAN:     return ws_scan(m);
    case ID_JOIN:     return ws_join(m, ws_net_selected(m));
    case ID_OTHER:    return ws_join_other(m);
    case ID_FORGET:   return ws_forget(m, ws_saved_selected(m));
    case ID_HOST_SET: return ws_set_host(m, gk_text(s->host));
    case ID_SAVE_AGAIN: return ws_save_again(m);
    case ID_ASK_YES:  return panel_yes(s);
    case ID_ASK_NO:   return ws_answer(m, 0);
    }
    return 0;
}

static int changed(struct wswin *s, const struct gk_event *ev) {
    struct wsettings *m = &s->m;
    switch(ev->id) {
    case ID_TABS:     return ws_page(m, ev->value);
    case ID_CARD:     return ws_card(m, ev->value);
    case ID_NETS:     return ws_select_net(m, ev->value);
    case ID_SAVED:    return ws_select_saved(m, ev->value);
    case ID_STARTUP:  return ws_set(m, "wifi", ev->value ? "on" : "off");
    case ID_ETHERNET: return ws_set(m, "ethernet", ev->value ? "dhcp" : "off");
    case ID_TELNETD:  return ws_set(m, "telnetd", ev->value ? "on" : "off");
    case ID_HISTORY:  return ws_set(m, "history", ev->value ? "on" : "off");
    case ID_THEME:                                      /* steps: in force at once, written once they stop */
        if(ev->value >= 0 && ev->value < s->nthemes) return ws_choose(m, "desktop.theme", s->themes[ev->value]);
        return 0;
    case ID_POINTER:
        if(ev->value >= 0 && ev->value < s->nspeeds) return ws_choose(m, "pointer.speed", s->speeds[ev->value]);
        return 0;
    }
    return 0;                                           /* the fields' text: read when it is used */
}

static int activated(struct wswin *s, const struct gk_event *ev) {
    switch(ev->id) {
    case ID_NETS:     return ws_join(&s->m, ev->value);
    case ID_HOST:     return ws_set_host(&s->m, gk_text(s->host));
    case ID_ASK_NAME:
        gk_focus(s->ask_pass);                          /* Enter in the name goes on to the password */
        return 0;
    case ID_ASK_PASS: return gk_enabled(s->ask_yes) ? panel_yes(s) : 0;    /* greyed: the password stays typed */
    }
    return 0;
}

static int menu(struct wswin *s, int id) {
    if(id >= MENU_PAGE && id < MENU_PAGE + WS_PAGES) return ws_page(&s->m, id - MENU_PAGE);
    switch(id) {
    case MENU_CLOSE: return ws_close(&s->m, gk_text(s->host));
    case MENU_SCAN:  return ws_scan(&s->m);
    case MENU_OTHER: return ws_join_other(&s->m);
    case MENU_LEAVE: return ws_leave(&s->m);
    case MENU_ABOUT: return ws_page(&s->m, WS_ABOUT);
    }
    return 0;
}

int wsw_event(struct wswin *s, const struct gk_event *ev) {
    struct wsettings *m = &s->m;
    int r = 0;
    m->now = uptime();
    switch(ev->type) {
    case GK_EV_CLOSE:
        r = ws_close(m, gk_text(s->host));
        break;
    case GK_EV_KEY:                                     /* a key no widget took: Esc */
        if(!ev->w && ev->value == 27) r = m->ask.kind ? ws_answer(m, 0) : ws_close(m, gk_text(s->host));
        break;
    case GK_EV_TIMER:
        if(ev->id != TIMER_TICK) break;
        r = ws_tick(m);                                 /* (WS_CLOSE once a close that waited may end) */
        if(m->page == WS_ABOUT && s->about && m->now - s->about_at >= 1000) {
            s->about(&m->about, 0);                     /* the memory and the time since the start, as they go */
            s->about_at = m->now;
        }
        break;
    case GK_EV_CLICK:
        r = clicked(s, ev->id);
        break;
    case GK_EV_CHANGE:
        r = changed(s, ev);
        break;
    case GK_EV_ACTIVATE:
        r = activated(s, ev);
        break;
    case GK_EV_MENU:
        r = menu(s, ev->id);
        break;
    }
    if(r == WS_CLOSE) return 0;
    show_model(s);
    return 1;
}

/**** the window ****/

static struct gk_widget *labelled(struct gk_widget *col, const char *text) {
    struct gk_widget *row = gk_hbox(col, 8);
    gk_size(gk_label(row, text), LABEL_W, 0);
    return row;
}

static void build_wifi(struct wswin *s, struct gk_widget *page) {
    static const int net_cols[3] = { 200, 320, 410 }, saved_cols[2] = { 200, 260 };
    struct gk_widget *col = gk_vbox(page, 4), *row;
    gk_pad(col, 8);
    gk_grow(col, 1, 1);
    row = gk_hbox(col, 8);
    s->card = gk_switch(row, "Wi-Fi card", ID_CARD, 0);
    text_area(s, T_STATION, row, 2, 0, 0);
    s->leave = gk_button(row, "Disconnect", ID_LEAVE);
    s->save = gk_button(row, "Save", ID_SAVE);
    row = gk_hbox(col, 8);
    gk_label(row, "Networks in range");
    gk_spacer(row);
    s->scan = gk_button(row, "Scan", ID_SCAN);
    s->other = gk_button(row, "By name\xe2\x88\x99", ID_OTHER);
    s->join = gk_button(row, "Join\xe2\x88\x99", ID_JOIN);
    s->nets = gk_list(col, ID_NETS);
    gk_list_columns(s->nets, 3, net_cols);
    gk_grow(s->nets, 1, 1);
    text_area(s, T_NETNOTE, col, 2, 0, 1);
    row = gk_hbox(col, 8);
    gk_label(row, "Saved networks, tried in this order");
    gk_spacer(row);
    s->forget = gk_button(row, "Forget\xe2\x88\x99", ID_FORGET);
    s->saved = gk_list(col, ID_SAVED);
    gk_list_columns(s->saved, 2, saved_cols);
    gk_size(s->saved, 0, 3 * GK_ROW_H + 4);
    gk_grow(s->saved, 1, 0);
    s->startup = gk_switch(col, "Join a saved network by itself at startup", ID_STARTUP, 0);
}

static void build_network(struct wswin *s, struct gk_widget *page) {
    struct gk_widget *col = gk_vbox(page, 6), *row;
    gk_pad(col, 10);
    gk_grow(col, 1, 1);
    s->ethernet = gk_switch(col, "Ethernet: ask for an address whenever a cable is connected (DHCP)", ID_ETHERNET, 0);
    gk_separator(col);
    gk_label(col, "Interfaces");
    s->ifaces = gk_list(col, ID_IFACES);
    gk_grow(s->ifaces, 1, 1);
    row = labelled(col, "Host name:");
    s->host = gk_field(row, ID_HOST, 32, 0);         /* net/netcfg.c's NETCFG_HOSTNAME_MAX */
    gk_grow(s->host, 1, 0);
    s->host_set = gk_button(row, "Set", ID_HOST_SET);
    gk_label_color(gk_label(col, "Letters, digits and -: the name SurfOS gives in its DHCP requests, which the router "
                                  "lists."), gk_theme(s->ui)->muted);
}

static void build_desktop(struct wswin *s, struct gk_widget *page) {
    struct gk_widget *col = gk_vbox(page, 10);
    gk_pad(col, 10);
    gk_grow(col, 1, 1);
    s->theme_row = labelled(col, "Theme:");
    s->theme = gk_radio(s->theme_row, ID_THEME, 1);
    s->pointer_row = labelled(col, "Pointer speed:");
    s->pointer = gk_radio(s->pointer_row, ID_POINTER, 1);
    text_area(s, T_DESK, col, 3, 0, 1);
}

static void build_system(struct wswin *s, struct gk_widget *page) {
    struct gk_widget *col = gk_vbox(page, 4);
    gk_pad(col, 10);
    gk_grow(col, 1, 1);
    s->telnetd = gk_switch(col, "Start the telnet server at startup (telnetd)", ID_TELNETD, 0);
    text_area(s, T_TELNETD, col, 3, 0, 1);
    gk_separator(col);
    s->history = gk_switch(col, "Keep the shell's history on the stick (history)", ID_HISTORY, 0);
    text_area(s, T_HISTORY, col, 2, 0, 1);
    gk_separator(col);
    text_area(s, T_WHERE, col, 3, 0, 0);
    s->save_again = gk_button(col, "Save again on the stick", ID_SAVE_AGAIN);
    show(s->save_again, 0);
}

static void build_about(struct wswin *s, struct gk_widget *page) {
    static const char *const labels[WSW_ABOUT_ROWS] = { "Version", "Build", "Memory", "Programs", "Up for",
                                                        "Processor" };
    struct gk_widget *col = gk_vbox(page, 4), *row, *label;
    int k;
    gk_pad(col, 12);
    gk_grow(col, 1, 1);
    for(k = 0; k < WSW_ABOUT_ROWS; k++) {
        row = gk_hbox(col, 8);
        label = gk_label(row, labels[k]);
        gk_size(label, LABEL_W, 0);
        gk_label_color(label, gk_theme(s->ui)->muted);
        s->about_value[k] = gk_label(row, "");
    }
}

static void build_panel(struct wswin *s, struct gk_widget *panel) {
    struct gk_widget *row;
    gk_pad(panel, 16);
    s->ask_title = gk_label(panel, "");
    text_area(s, T_ASK, panel, 8, 0, 0);
    s->ask_name_row = labelled(panel, "Name:");
    s->ask_name = gk_field(s->ask_name_row, ID_ASK_NAME, NETCTL_SSID_MAX, 0);
    gk_grow(s->ask_name, 1, 0);
    s->ask_pass_row = labelled(panel, "Password:");
    s->ask_pass = gk_field(s->ask_pass_row, ID_ASK_PASS, NETCTL_PASS_MAX - 1, GK_PASSWORD);
    gk_grow(s->ask_pass, 1, 0);
    s->ask_save = gk_check(panel, "Save it: SurfOS joins it by itself at every start", ID_ASK_SAVE, 0);
    gk_spacer(panel);
    row = gk_hbox(panel, 8);
    gk_spacer(row);
    s->ask_no = gk_button(row, "Cancel", ID_ASK_NO);
    s->ask_yes = gk_button(row, "Join", ID_ASK_YES);
}

static void menus(struct wswin *s) {
    int i, k;
    i = gk_menu(s->ui, "File");
    gk_menu_item(s->ui, i, MENU_CLOSE, "Close", GK_CTRL | 'W', 0);
    i = gk_menu(s->ui, "View");
    for(k = 0; k < WS_PAGES; k++)
        gk_menu_item(s->ui, i, MENU_PAGE + k, ws_page_name(k), WIN_KEY_F(5 + k), GK_ITEM_RADIO | (k ? 0 : GK_ITEM_ON));
    i = gk_menu(s->ui, "Wi-Fi");
    gk_menu_item(s->ui, i, MENU_SCAN, "Scan for networks", GK_CTRL | 'R', 0);
    gk_menu_item(s->ui, i, MENU_OTHER, "Join a network by its name\xe2\x88\x99", GK_CTRL | 'J', 0);
    gk_menu_item(s->ui, i, MENU_LEAVE, "Disconnect", 0, 0);
    i = gk_menu(s->ui, "Help");
    gk_menu_item(s->ui, i, MENU_ABOUT, "About Settings", 0, 0);
}

int wsw_open(struct wswin *s, ws_netctl_fn netctl, void *ctx, void (*about)(struct ws_about *ab, int all)) {
    struct gk_widget *root;
    memset(s, 0, sizeof(*s));
    ws_init(&s->m, netctl, ctx);
    s->about = about;
    s->m.now = uptime();
    s->ui = gk_open_limits("Settings", WSW_W, WSW_H, WSW_MIN_W, WSW_MIN_H, 0, 0, WIN_RESIZABLE);
    if(!s->ui) return -1;
    root = gk_root(s->ui);
    s->main = gk_pages(root);                           /* the tabs, or the question panel in their place */
    s->tabs = gk_tabs(s->main, ID_TABS, 0);
    build_wifi(s, gk_tab(s->tabs, ws_page_name(WS_WIFI)));
    build_network(s, gk_tab(s->tabs, ws_page_name(WS_NETWORK)));
    build_desktop(s, gk_tab(s->tabs, ws_page_name(WS_DESKTOP)));
    build_system(s, gk_tab(s->tabs, ws_page_name(WS_SYSTEM)));
    build_about(s, gk_tab(s->tabs, ws_page_name(WS_ABOUT)));
    s->panel = gk_vbox(s->main, 8);
    build_panel(s, s->panel);
    text_area(s, T_MSG, root, 2, 1, 0);
    menus(s);
    gk_timer(s->ui, TIMER_TICK, WSW_TICK_MS, 1);
    if(!gk_ok(s->ui)) {
        gk_close(s->ui);
        s->ui = NULL;
        errno = ENOMEM;
        return -1;
    }
    if(s->about) s->about(&s->m.about, 1);
    s->about_at = s->m.now;
    ws_tick(&s->m);                                     /* the kernel read, the scan the Wi-Fi page asks for */
    show_model(s);
    gk_focus(s->card);
    return 0;
}

void wsw_close(struct wswin *s) {
    if(s->ui) {
        gk_field_wipe(s->ask_pass);
        gk_close(s->ui);
    }
    s->ui = NULL;
}
