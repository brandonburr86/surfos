/*
SurfOS user program: wkit
--------------------
File: demo.c    Date: 11/05/26
--------------------
The window of the toolkit's demonstration (demo.h): four tabs over a status bar.

    Controls  a name and a password (fields), Wi-Fi (a check box), Join at startup (a switch),
              the speed (a radio group), and Greet, Ask, Rename... and Copy, which open a message,
              a question and a line to type over the window, and copy the name to the desktop's
              clipboard
    Lists     networks in a list with columns, what is selected, Add and Remove, and a scroll bar
              on its own
    Drawing   an area the program draws: 64 x 64 cells with scroll bars, a cursor cell moved by the
              arrows (the area has the keys: its cursor in coral), Space or a click marking a cell;
              only the cells that change are painted again
    More      notes (a text of several lines), the seconds of the minute (a progress bar), a volume
              (a slider), colours and tools (palettes)

The status bar says what the program last heard, and at its right the seconds since it started (a
timer). Its menus, in the desktop's bar while the window has the focus (and by their shortcuts in any
case): File with Open (Ctrl+O) and Save as (Ctrl+S), which ask the desktop's choosers (or, on a desktop
without them, a line to type the path in), and Close (Ctrl+W); View with the pages (F5 to F8) and
Wi-Fi, checked as the check box is, and Speed, a submenu; About the toolkit in the desktop's Help. Esc,
Ctrl+W or the close button ask before the window closes.
11/05/26 (stage 3, on the desktop's own calls): a key no widget took is said with its modifiers ("Key
0x78 mods 2": Alt+X), as is a new theme; Copy pastes the clipboard back at once, which the kernel
gives as the window's own copy; Ctrl+Shift+O keeps the program busy for three seconds, without
reading its events, before it asks for Open: events that fill the window's queue meanwhile make the
desktop refuse the request for now (EAGAIN), and the toolkit asks again once it has read them. The
chooser's answer then says how many events were lost in the spell (WIN_INFO): any at all means the
queue was full when Open was asked for.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <surf.h>
#include "demo.h"

static const char *const networks[] = {           /* more than the list shows: it scrolls */
    "SurfSim\t-40 dB\topen", "SurfSimWPA\t-48 dB\tWPA2", "Harbour\t-55 dB\tWPA2", "Lighthouse\t-61 dB\tWPA2",
    "Dunes\t-67 dB\topen", "Tidepool\t-70 dB\tWPA2", "Breakwater\t-72 dB\tWPA2", "Kelp\t-75 dB\topen",
    "Reef\t-78 dB\tWPA2", "Estuary\t-81 dB\tWPA2", "Seawall\t-84 dB\topen", "Jetty\t-88 dB\tWPA2",
    "Lagoon\t-89 dB\tWPA2", "Sandbar\t-90 dB\topen", "Pier\t-90 dB\tWPA2", "Shoal\t-91 dB\tWPA2",
    "Cove\t-92 dB\topen", "Inlet\t-92 dB\tWPA2", "Bayside\t-93 dB\tWPA2", "Driftwood\t-94 dB\topen",
    "Saltmarsh\t-95 dB\tWPA2", "Headland\t-96 dB\tWPA2", "Sounding\t-97 dB\topen", "Wavecrest\t-98 dB\tWPA2",
};
#define NETWORKS (int)(sizeof(networks) / sizeof(networks[0]))
static const char *const speeds[] = { "Slow", "Normal", "Fast" };
static const unsigned colors[] = { GW_INK, GW_TEAL, GW_FOAM, GW_CORAL, GW_SAND, GW_SKY, GW_PAPER, GW_MUTED };
static const char *const tools[] = { "Pencil", "Eraser" };

/* the tools' pictures, 16 x 16, drawn here: a pencil and an eraser; '.' is see-through */
static const char *const pictures[2][16] = {
    { "............CC..", "...........CCCC.", "..........KSSCCK", ".........KSSSSK.", "........KSSSSK..",
      ".......KSSSSK...", "......KSSSSK....", ".....KSSSSK.....", "....KSSSSK......", "...KSSSSK.......",
      "..KPPSSK........", "..KPPPK.........", ".KKPPK..........", ".KKKK...........", ".KK.............",
      "................" },
    { "................", "................", "........KKKKK...", ".......KCCCCCK..", "......KCCCCCCK..",
      ".....KCCCCCCK...", "....KPKCCCCK....", "...KPPPKCCK.....", "..KPPPPPKK......", ".KPPPPPPK.......",
      ".KPPPPPK........", "..KPPPK.........", "...KKK..........", "................", "................",
      "................" },
};

static void picture(int i, unsigned *px) {
    int k;
    for(k = 0; k < 256; k++) {
        char c = pictures[i][k / 16][k % 16];
        px[k] = c == 'K' ? GW_INK : c == 'C' ? GW_CORAL : c == 'S' ? GW_SAND : c == 'P' ? 0xFFFDF5 : 0xFF000000u;
    }
}

static void say(struct demo *d, const char *s) {
    gk_set_text(d->status, s);
}

/* the cells that meet the part to be painted, each a pixel short of the next */
static void draw(struct gk_widget *area, struct gw *win, const struct gk_view *v, void *data) {
    const struct demo *d = data;
    const struct gk_theme *t = gk_theme(d->ui);
    int c0 = v->sx + v->cx / DEMO_CELL, r0 = v->sy + v->cy / DEMO_CELL;
    int c1 = v->sx + (v->cx + v->cw - 1) / DEMO_CELL, r1 = v->sy + (v->cy + v->ch - 1) / DEMO_CELL, r, c, x, y;
    (void)area;
    gw_rect(win, v->cx, v->cy, v->cw, v->ch, t->line);
    for(r = r0; r <= r1 && r < DEMO_CELLS; r++)
        for(c = c0; c <= c1 && c < DEMO_CELLS; c++) {
            x = (c - v->sx) * DEMO_CELL;
            y = (r - v->sy) * DEMO_CELL;
            gw_rect(win, x, y, DEMO_CELL - 1, DEMO_CELL - 1, d->mark[r][c] ? t->teal : (r + c) % 2 ? t->band : t->sheet);
            if(c == d->cx && r == d->cy) {
                gw_frame(win, x, y, DEMO_CELL - 1, DEMO_CELL - 1, d->area_keys ? t->coral : t->muted);
                gw_frame(win, x + 1, y + 1, DEMO_CELL - 3, DEMO_CELL - 3, d->area_keys ? t->coral : t->muted);
            }
        }
}

static void cell_damage(struct demo *d, int c, int r) {
    struct gk_view v;
    gk_area_view(d->area, &v);
    gk_damage(d->area, (c - v.sx) * DEMO_CELL, (r - v.sy) * DEMO_CELL, DEMO_CELL, DEMO_CELL);
}

static void cell_say(struct demo *d) {
    char b[64];
    snprintf(b, sizeof(b), "Cell %d, %d%s", d->cx, d->cy, d->mark[d->cy][d->cx] ? ", marked" : "");
    say(d, b);
}

/* the cursor to a cell, kept in view: the cells it leaves and reaches painted again */
static void cursor_to(struct demo *d, int c, int r) {
    c = c < 0 ? 0 : c >= DEMO_CELLS ? DEMO_CELLS - 1 : c;
    r = r < 0 ? 0 : r >= DEMO_CELLS ? DEMO_CELLS - 1 : r;
    cell_damage(d, d->cx, d->cy);
    d->cx = c;
    d->cy = r;
    gk_area_reveal(d->area, c, r, 1, 1);
    cell_damage(d, c, r);
    cell_say(d);
}

static void mark(struct demo *d) {
    d->mark[d->cy][d->cx] = !d->mark[d->cy][d->cx];
    cell_damage(d, d->cx, d->cy);
    cell_say(d);
}

static void area_key(struct demo *d, unsigned key) {
    struct gk_view v;
    gk_area_view(d->area, &v);
    switch(key) {
    case WIN_KEY_LEFT: cursor_to(d, d->cx - 1, d->cy); break;
    case WIN_KEY_RIGHT: cursor_to(d, d->cx + 1, d->cy); break;
    case WIN_KEY_UP: cursor_to(d, d->cx, d->cy - 1); break;
    case WIN_KEY_DOWN: cursor_to(d, d->cx, d->cy + 1); break;
    case WIN_KEY_PGUP: cursor_to(d, d->cx, d->cy - (v.h / DEMO_CELL > 1 ? v.h / DEMO_CELL - 1 : 1)); break;
    case WIN_KEY_PGDN: cursor_to(d, d->cx, d->cy + (v.h / DEMO_CELL > 1 ? v.h / DEMO_CELL - 1 : 1)); break;
    case WIN_KEY_HOME: cursor_to(d, 0, 0); break;
    case WIN_KEY_END: cursor_to(d, DEMO_CELLS - 1, DEMO_CELLS - 1); break;
    case ' ': mark(d); break;
    case 27:                                            /* Esc closes the window here too */
        gk_confirm(d->ui, BOX_CLOSE, "Close the demonstration?", "Its window closes.", "", "Close", NULL, "Cancel");
        break;
    }
}

static void picked(struct demo *d) {
    char b[80], *tab;
    const char *s = gk_text(d->list);
    snprintf(b, sizeof(b), "%s", *s ? s : "nothing");
    if((tab = strchr(b, '\t'))) *tab = 0;
    gk_set_text(d->picked, b);
}

static void clicked(struct demo *d, int id) {
    char b[160];
    int n;
    switch(id) {
    case ID_GREET:
        snprintf(b, sizeof(b), "Hello, %s.", *gk_text(d->name) ? gk_text(d->name) : "whoever you are");
        gk_message(d->ui, BOX_GREET, "Greet", b, "A message over the window: Enter or OK closes it.");
        break;
    case ID_ASK:
        gk_confirm(d->ui, BOX_ASK, "Ask", "Is the sea calm today?", "Left and Right choose, Enter answers.",
                   "Yes", "No", "Cancel");
        break;
    case ID_RENAME:
        gk_ask(d->ui, BOX_RENAME, "Rename", "A new name:", gk_text(d->name), 0);
        break;
    case ID_COPY:
        n = (int)strlen(gk_text(d->name));
        if(!n) say(d, "Nothing to copy: type a name first");
        else if(gk_copy(d->ui, gk_text(d->name), (unsigned)n) == 0) {
            char back[24];                          /* pasted back at once: the window's own copy, newer than the
                                                       desktop's clipboard until the desktop takes it */
            if(gk_paste(d->ui, back, sizeof(back)) < 0) snprintf(back, sizeof(back), "%s", strerror(errno));
            snprintf(b, sizeof(b), "Copied the name, pasted back: %s", back);
            say(d, b);
        } else say(d, errno == EACCES ? "Not copied: the window must have the focus" : "Not copied");
        break;
    case ID_ADD:
        snprintf(b, sizeof(b), "New network %d\t-90 dB\topen", ++d->added);
        n = gk_list_add(d->list, b);
        gk_set_value(d->list, n);
        gk_list_reveal(d->list, n);
        picked(d);
        say(d, "Added a line");
        break;
    case ID_REMOVE:
        if(gk_value(d->list) < 0) {
            say(d, "Nothing selected to remove");
            break;
        }
        gk_list_remove(d->list, gk_value(d->list));
        picked(d);
        say(d, "Removed a line");
        break;
    case ID_CLEAR:
        memset(d->mark, 0, sizeof(d->mark));
        gk_redraw(d->area);
        say(d, "Cleared the drawing");
        break;
    }
}

/* the View menu's dot on the page shown, and in its Speed submenu on the speed chosen */
static void page_shown(struct demo *d) {
    int i;
    for(i = 0; i < d->pages; i++) gk_menu_check(d->ui, MENU_PAGE + i, i == gk_value(d->tabs));
}

static void speed_shown(struct demo *d) {
    int i;
    for(i = 0; i < 3; i++) gk_menu_check(d->ui, MENU_SPEED + i, i == gk_value(d->speed));
}

static void menu(struct demo *d, int id) {
    char b[64];
    switch(id) {
    case MENU_OPEN:
        if(gk_choose(d->ui, CHOOSE_OPEN, WIN_DLG_OPEN, "", "", "") < 0) {
            snprintf(b, sizeof(b), "No chooser: %s", errno == EACCES ? "not now (focus, or a key first)" : strerror(errno));
            say(d, b);
        }
        break;
    case MENU_SAVE:
        if(gk_choose(d->ui, CHOOSE_SAVE, WIN_DLG_SAVE, "", "notes.txt", "txt") < 0) {
            snprintf(b, sizeof(b), "No chooser: %s", errno == EACCES ? "not now (focus, or a key first)" : strerror(errno));
            say(d, b);
        }
        break;
    case MENU_CLOSE:
        gk_confirm(d->ui, BOX_CLOSE, "Close the demonstration?", "Its window closes.", "", "Close", NULL, "Cancel");
        break;
    case MENU_WIFI:
        gk_set_value(d->wifi, !gk_value(d->wifi));
        gk_menu_check(d->ui, MENU_WIFI, gk_value(d->wifi));
        say(d, gk_value(d->wifi) ? "Wi-Fi on" : "Wi-Fi off");
        break;
    case MENU_ABOUT:
        gk_message(d->ui, BOX_ABOUT, "About the toolkit", "wkit shows the widgets of gk.h,",
                   "the toolkit of SurfOS's desktop applications.");
        break;
    default:
        if(id >= MENU_SPEED && id < MENU_SPEED + 3) {
            gk_set_value(d->speed, id - MENU_SPEED);
            speed_shown(d);
            snprintf(b, sizeof(b), "Speed: %s", speeds[id - MENU_SPEED]);
            say(d, b);
        } else if(id >= MENU_PAGE && id < MENU_PAGE + d->pages) {
            gk_set_value(d->tabs, id - MENU_PAGE);
            page_shown(d);
            snprintf(b, sizeof(b), "Page: %s", gk_text(d->tabs));
            say(d, b);
        }
    }
}

static void changed(struct demo *d, const struct gk_event *ev) {
    char b[160];
    switch(ev->id) {
    case ID_NAME: snprintf(b, sizeof(b), "Name: %s", gk_text(d->name)); break;
    case ID_SECRET: snprintf(b, sizeof(b), "Password: %d bytes", ev->value); break;
    case ID_WIFI:
        snprintf(b, sizeof(b), "Wi-Fi %s", ev->value ? "on" : "off");
        gk_menu_check(d->ui, MENU_WIFI, ev->value);
        break;
    case ID_JOIN: snprintf(b, sizeof(b), "Join at startup %s", ev->value ? "on" : "off"); break;
    case ID_SPEED:
        snprintf(b, sizeof(b), "Speed: %s", speeds[ev->value % 3]);
        speed_shown(d);
        break;
    case ID_TABS:
        snprintf(b, sizeof(b), "Page: %s", gk_text(d->tabs));
        page_shown(d);
        break;
    case ID_LIST:
        picked(d);
        snprintf(b, sizeof(b), "Selected: %s", gk_text(d->picked));
        break;
    case ID_BAR:
        snprintf(b, sizeof(b), "at %d", ev->value);
        gk_set_text(d->barval, b);
        snprintf(b, sizeof(b), "Scroll bar at %d", ev->value);
        break;
    case ID_NOTES: snprintf(b, sizeof(b), "Notes: %d bytes", ev->value); break;
    case ID_VOLUME: snprintf(b, sizeof(b), "Volume: %d", ev->value); break;
    case ID_COLORS: snprintf(b, sizeof(b), "Colour: #%06X", gk_palette_get_color(d->colors, ev->value)); break;
    case ID_TOOLS: snprintf(b, sizeof(b), "Tool: %s", tools[ev->value % 2]); break;
    default: return;
    }
    say(d, b);
}

/* Ctrl+Shift+O: three seconds busy, the window's events left unread, then Open; the events lost meanwhile (WIN_INFO's
   count, before and after) are said with the chooser's answer */
static void busy(struct demo *d) {
    struct win_info before, after;
    memset(&before, 0, sizeof(before));
    memset(&after, 0, sizeof(after));
    winctl(gk_gw(d->ui)->fd, WIN_INFO, &before);
    msleep(3000);
    winctl(gk_gw(d->ui)->fd, WIN_INFO, &after);
    if(gk_choose(d->ui, CHOOSE_OPEN, WIN_DLG_OPEN, "", "", "") < 0) {
        say(d, "No chooser after the busy spell");
        return;
    }
    d->busy = 1;
    d->busy_lost = (int)(after.lost - before.lost);
}

int demo_event(struct demo *d, const struct gk_event *ev) {
    char b[160];
    int n;
    switch(ev->type) {
    case GK_EV_CLOSE:
        gk_confirm(d->ui, BOX_CLOSE, "Close the demonstration?", "Its window closes.", "", "Close", NULL, "Cancel");
        break;
    case GK_EV_ANSWER:
        if(ev->id == BOX_CLOSE) {
            if(ev->value == 0) return 0;
            say(d, "Not closed");
        } else if(ev->id == BOX_RENAME && ev->value == 0) {
            gk_set_text(d->name, ev->text);
            snprintf(b, sizeof(b), "Renamed: %s", ev->text);
            say(d, b);
        } else {
            snprintf(b, sizeof(b), "Answered %d", ev->value);
            say(d, b);
        }
        break;
    case GK_EV_CLICK:
        clicked(d, ev->id);
        break;
    case GK_EV_MENU:
        menu(d, ev->id);
        break;
    case GK_EV_CHOSEN:
        n = ev->value ? snprintf(b, sizeof(b), "Chose %s", ev->text) : snprintf(b, sizeof(b), "Not chosen");
        if(d->busy && n > 0 && n < (int)sizeof(b)) snprintf(b + n, sizeof(b) - (size_t)n, "; lost while busy: %d", d->busy_lost);
        d->busy = 0;
        say(d, b);
        break;
    case GK_EV_CHANGE:
        changed(d, ev);
        break;
    case GK_EV_ACTIVATE:
        if(ev->id == ID_NAME) snprintf(b, sizeof(b), "Enter: %s", gk_text(d->name));
        else if(ev->id == ID_LIST) snprintf(b, sizeof(b), "Opened: %s", gk_text(d->picked));
        else break;
        say(d, b);
        break;
    case GK_EV_KEY:
        if(ev->w == d->area) area_key(d, (unsigned)ev->value);
        else if(ev->value == 27 && !(ev->mods & WIN_MOD_CTRL))     /* Esc (not Ctrl+[) */
            gk_confirm(d->ui, BOX_CLOSE, "Close the demonstration?", "Its window closes.", "", "Close", NULL, "Cancel");
        else if(ev->value == 15 && (ev->mods & WIN_MOD_CTRL) && (ev->mods & WIN_MOD_SHIFT))
            busy(d);                                    /* Ctrl+Shift+O */
        else {
            snprintf(b, sizeof(b), "Key 0x%x mods %u", (unsigned)ev->value,
                     ev->mods & (WIN_MOD_SHIFT | WIN_MOD_ALT | WIN_MOD_CTRL | WIN_MOD_REPEAT));
            say(d, b);
        }
        break;
    case GK_EV_THEME:
        say(d, ev->value == WIN_THEME_DUSK ? "Theme: dusk" : "Theme: ocean");
        break;
    case GK_EV_POINTER:                                 /* the drawing: a press marks the cell under it */
        if(ev->w == d->area && (ev->value & WIN_BUTTON_LEFT) && ev->x >= 0 && ev->y >= 0) {
            struct gk_view v;
            gk_area_view(d->area, &v);
            if(ev->x < v.w && ev->y < v.h) {
                cursor_to(d, v.sx + ev->x / DEMO_CELL, v.sy + ev->y / DEMO_CELL);
                mark(d);
            }
        }
        break;
    case GK_EV_FOCUS:
        if(ev->w == d->area) {
            d->area_keys = ev->value;
            cell_damage(d, d->cx, d->cy);
        }
        break;
    case GK_EV_SCROLL:
        snprintf(b, sizeof(b), "Scrolled to %d, %d", ev->x, ev->y);
        say(d, b);
        break;
    case GK_EV_TIMER:
        snprintf(b, sizeof(b), "%u s", ++d->seconds);
        gk_status_right(d->status, b);
        gk_set_value(d->minute, (int)(d->seconds % 60));
        snprintf(b, sizeof(b), "%u of 60", d->seconds % 60);
        gk_set_text(d->minute, b);
        break;
    case GK_EV_RESIZE:
        snprintf(b, sizeof(b), "Size %dx%d", ev->x, ev->y);
        say(d, b);
        break;
    }
    return 1;
}

static struct gk_widget *labelled(struct gk_widget *col, const char *text) {
    struct gk_widget *row = gk_hbox(col, 8), *label = gk_label(row, text);
    gk_size(label, 88, 0);
    return row;
}

int demo_open(struct demo *d) {
    static const int cols[2] = { 112, 184 };
    struct gk_widget *root, *page, *col, *row;
    unsigned px[256];
    int i, k;
    memset(d, 0, sizeof(*d));
    d->ui = gk_open_limits("Toolkit", DEMO_W, DEMO_H, 400, 240, 0, 0, WIN_RESIZABLE);
    if(!d->ui) return -1;
    root = gk_root(d->ui);
    d->tabs = gk_tabs(root, ID_TABS, 0);
    /* Controls */
    page = gk_tab(d->tabs, "Controls");
    col = gk_vbox(page, 6);
    gk_pad(col, 10);
    d->name = gk_field(labelled(col, "Name:"), ID_NAME, 64, 0);
    gk_field_hint(d->name, "Type a name");
    d->secret = gk_field(labelled(col, "Password:"), ID_SECRET, 63, GK_PASSWORD);
    row = gk_hbox(col, 24);
    d->wifi = gk_check(row, "Wi-Fi", ID_WIFI, 1);
    d->join = gk_switch(row, "Join at startup", ID_JOIN, 0);
    d->speed = gk_radio(labelled(col, "Speed:"), ID_SPEED, 1);
    for(i = 0; i < 3; i++) gk_radio_add(d->speed, speeds[i]);
    gk_set_value(d->speed, 1);
    gk_separator(col);
    row = gk_hbox(col, 8);
    gk_button(row, "Greet", ID_GREET);
    gk_button(row, "Ask", ID_ASK);
    gk_button(row, "Rename\xe2\x88\x99", ID_RENAME);
    gk_button(row, "Copy", ID_COPY);
    /* Lists */
    page = gk_tab(d->tabs, "Lists");
    row = gk_hbox(page, 10);
    gk_pad(row, 10);
    gk_grow(row, 1, 1);
    d->list = gk_list(row, ID_LIST);
    gk_list_columns(d->list, 2, cols);
    for(i = 0; i < NETWORKS; i++) gk_list_add(d->list, networks[i]);
    col = gk_vbox(row, 6);
    gk_size(col, 150, 0);
    gk_label(col, "Selected:");
    d->picked = gk_label(col, "nothing");
    gk_button(col, "Add", ID_ADD);
    gk_button(col, "Remove", ID_REMOVE);
    gk_label(col, "A scroll bar:");
    d->bar = gk_scrollbar(col, ID_BAR, 1);
    gk_scroll_set(d->bar, 100, 10, 0);
    d->barval = gk_label(col, "at 0");
    /* Drawing */
    page = gk_tab(d->tabs, "Drawing");
    d->area = gk_area(page, ID_AREA, GK_FOCUSABLE, draw, d);
    gk_area_extent(d->area, DEMO_CELLS, DEMO_CELL, DEMO_CELLS, DEMO_CELL);
    row = gk_hbox(page, 8);
    gk_pad(row, 4);
    gk_label(row, "Arrows move, Space or a click marks");
    gk_spacer(row);
    gk_button(row, "Clear", ID_CLEAR);
    /* More */
    page = gk_tab(d->tabs, "More");
    row = gk_hbox(page, 10);
    gk_pad(row, 10);
    gk_grow(row, 1, 1);
    col = gk_vbox(row, 6);
    gk_grow(col, 1, 1);
    gk_label(col, "Notes:");
    d->notes = gk_text_area(col, ID_NOTES, 4096, 0);
    col = gk_vbox(row, 6);
    gk_size(col, 200, 0);
    gk_label(col, "This minute:");
    d->minute = gk_progress(col, 60);
    gk_set_text(d->minute, "0 of 60");
    gk_label(col, "Volume:");
    d->volume = gk_slider(col, ID_VOLUME, 0, 100, 5);
    gk_set_value(d->volume, 50);
    gk_label(col, "Colour:");
    d->colors = gk_palette(col, ID_COLORS, 4, 18, 18);
    for(i = 0; i < (int)(sizeof(colors) / sizeof(colors[0])); i++) gk_palette_color(d->colors, colors[i]);
    gk_set_value(d->colors, 0);
    gk_label(col, "Tool:");
    d->tools = gk_palette(col, ID_TOOLS, 2, 24, 24);
    for(i = 0; i < 2; i++) {
        picture(i, px);
        gk_palette_icon(d->tools, px, 16, 16);
    }
    gk_set_value(d->tools, 0);
    /* the menus: File, View with its Speed submenu, and the program's part of the desktop's Help; their items'
       shortcuts working whether or not the desktop shows them */
    i = gk_menu(d->ui, "File");
    gk_menu_item(d->ui, i, MENU_OPEN, "Open\xe2\x88\x99", GK_CTRL | 'O', 0);
    gk_menu_item(d->ui, i, MENU_SAVE, "Save as\xe2\x88\x99", GK_CTRL | 'S', 0);
    gk_menu_line(d->ui, i);
    gk_menu_item(d->ui, i, MENU_CLOSE, "Close", GK_CTRL | 'W', 0);
    i = gk_menu(d->ui, "View");
    d->pages = gk_tab_count(d->tabs);
    for(k = 0; k < d->pages; k++) {                     /* each page by the tab's label (gk_text(): the one shown) */
        gk_set_value(d->tabs, k);
        gk_menu_item(d->ui, i, MENU_PAGE + k, gk_text(d->tabs), WIN_KEY_F(5 + k), GK_ITEM_RADIO | (k ? 0 : GK_ITEM_ON));
    }
    gk_set_value(d->tabs, 0);
    gk_menu_line(d->ui, i);
    gk_menu_item(d->ui, i, MENU_WIFI, "Wi-Fi", 0, GK_ITEM_CHECK | GK_ITEM_ON);
    k = gk_submenu(d->ui, i, MENU_SPEEDS, "Speed", 0);
    for(i = 0; i < 3; i++) gk_menu_item(d->ui, k, MENU_SPEED + i, speeds[i], 0, GK_ITEM_RADIO | (i ? 0 : GK_ITEM_ON));
    i = gk_menu(d->ui, "Help");
    gk_menu_item(d->ui, i, MENU_ABOUT, "About the toolkit", 0, 0);
    /* the status bar, and the seconds */
    d->status = gk_status(root);
    say(d, "Ready");
    gk_status_right(d->status, "0 s");
    gk_timer(d->ui, TIMER_SECONDS, 1000, 1);
    gk_focus(d->name);
    if(!gk_ok(d->ui)) {
        gk_close(d->ui);
        d->ui = NULL;
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

void demo_close(struct demo *d) {
    gk_close(d->ui);
    d->ui = NULL;
}
