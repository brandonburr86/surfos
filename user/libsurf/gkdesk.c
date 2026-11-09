/*
SurfOS window toolkit: the desktop's menus, choosers, clipboard and theme
--------------------
File: gkdesk.c  Date: 11/05/26
--------------------
The toolkit's side of track desk's calls (surfos/win.h as the review of its stage 1 amended it;
gui.h's gw_menu() and the others; the desktop applications,
docs/superpowers/plans/2026-11-05-desktop-apps.md).

Menus: a program builds them once (gk_menu(), gk_menu_item(), gk_submenu()), and the toolkit sends
them whole (WIN_MENU) at the next flush whenever anything in them changed: each menu's title, its
items, and an item that opens a submenu followed by the submenu's items, as WIN_MENU_CHILD entries.
A menu titled Help goes into the desktop's Help (WIN_MENU_HELP). Labels go as the desktop draws them:
a byte that is no whole UTF-8 character, or a control character, is sent as '?', since the kernel
refuses a menu with one. Their shortcuts are the toolkit's: the desktop only shows a shortcut's text,
so a key the window gets that is an enabled item's shortcut becomes GK_EV_MENU, as a click on the item
in the bar does. A desktop that does not show a program's menus (a kernel before them says EINVAL,
one that has not got them yet ENOSYS) still has every command by its keys; gk_menus_shown() says
which.

Choosers: WIN_THEME, read as the window opens, says whether the kernel has these calls at all
(surfos/win.h: it is their feature test). With them, EINVAL from WIN_DIALOG is a request that is none
and comes back to the program, ENOSYS a desktop without choosers yet; without them, or with ENOSYS, a
line to type the path in over the window stands in (gkbox.c), answered as GK_EV_CHOSEN too. A chooser
is asked only while the program answers a person's own key, press or choice (gkcore.c's user_act), as
the kernel wants a key, a press or a menu choice since the window's last request. A chosen path is
taken (WIN_TAKE) as its answer comes. EAGAIN (the window's events fill its queue, and the answer
would have no place) is no answer: the request is kept and asked again once gk_next() has read the
events (gk_choose_again()), and if the desktop refuses it then (the focus gone meanwhile), it is
answered as cancelled, so the program hears of every request it made.

The clipboard is read whole when it is longer than the caller's buffer (WIN_PASTE says how long it
is), and what is not wanted is wiped. The theme is read at the window's opening and again at each
WIN_EV_THEME, the colours by their roles, those the desktop fills (count).
*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gkpriv.h"

/* s into an array of size bytes, its NUL within it, cut at a character's start */
static void put_text(char *to, size_t size, const char *s) {
    size_t n = strlen(s ? s : "");
    if(n >= size) {
        n = size - 1;
        while(n && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    }
    memcpy(to, s ? s : "", n);
    to[n] = 0;
}

/* a label as the desktop draws it (surfos/win.h): whole UTF-8 characters by the console's rules, none of them a
   control character (C0, DEL or C1): any other byte becomes '?'. Then cut to size at a character's start */
static void put_label(char *to, size_t size, const char *s) {
    char clean[256];
    size_t k = 0;
    int len;
    unsigned char c;
    while(s && *s && k < sizeof(clean) - 5) {
        c = (unsigned char)*s;
        gw_char(s, (int)strnlen(s, 4), &len);
        if(c < 0x20 || c == 0x7F || (c >= 0x80 && len == 1) || (c == 0xC2 && len == 2 && (unsigned char)s[1] < 0xA0)) {
            clean[k++] = '?';                           /* no character the desktop would draw as one, or a control */
            s += len;
            continue;
        }
        memcpy(clean + k, s, (size_t)len);
        k += (size_t)len;
        s += len;
    }
    clean[k] = 0;
    put_text(to, size, clean);
}

/**** menus ****/

/* a shortcut's text, as the desktop's own menus write theirs: "Ctrl+Shift+Z", "F5", "Del" */
static void shortcut_text(unsigned sc, char *out, size_t n) {
    static const char *const names[] = { "Up", "Down", "Left", "Right", "Home", "End", "PgUp", "PgDn", "Del", "Ins" };
    static const char *const ctrl_keys[] = { "Tab", "Enter", "Esc", "Bksp" };   /* WIN_KEY_TAB .. WIN_KEY_BACKSPACE */
    unsigned key = sc & 0xFFFF, mods = sc >> 16;
    char name[8], text[32];
    if(!sc) {
        out[0] = 0;
        return;
    }
    if(key >= (unsigned)WIN_KEY_F(1) && key <= (unsigned)WIN_KEY_F(12)) snprintf(name, sizeof(name), "F%u", key - WIN_KEY_F(0));
    else if(key >= WIN_KEY_UP && key <= WIN_KEY_INSERT) snprintf(name, sizeof(name), "%s", names[key - WIN_KEY_UP]);
    else if(key >= WIN_KEY_TAB && key <= WIN_KEY_BACKSPACE) snprintf(name, sizeof(name), "%s", ctrl_keys[key - WIN_KEY_TAB]);
    else if(key == 13) snprintf(name, sizeof(name), "Enter");
    else if(key == 9) snprintf(name, sizeof(name), "Tab");
    else if(key == 27) snprintf(name, sizeof(name), "Esc");
    else if(key == 127) snprintf(name, sizeof(name), "Bksp");
    else if(key == ' ') snprintf(name, sizeof(name), "Space");
    else if(key > ' ' && key < 127) snprintf(name, sizeof(name), "%c", key >= 'a' && key <= 'z' ? key - 'a' + 'A' : key);
    else snprintf(name, sizeof(name), "?");
    snprintf(text, sizeof(text), "%s%s%s%s", (mods & WIN_MOD_CTRL) ? "Ctrl+" : "", (mods & WIN_MOD_ALT) ? "Alt+" : "",
             (mods & WIN_MOD_SHIFT) ? "Shift+" : "", name);
    put_text(out, n, text);                             /* cut to the menu's array: "Ctrl+Alt+Shift+" is too long */
}

static struct gk_menus *menus_of(struct gk *ui) {
    if(!ui->menus) {
        if(!(ui->menus = calloc(1, sizeof(*ui->menus)))) ui->failed = 1;
        else ui->menus->help = -1;
    }
    return ui->menus;
}

static int count_of(const struct gk_menus *m, int menu, int *last) {
    int i, n = 0;
    *last = -1;
    for(i = 0; i < m->n; i++) {
        if(m->e[i].menu != menu || (m->e[i].flags & WIN_MENU_TITLE)) continue;
        n++;
        *last = i;
    }
    return n;
}

/* the cells of a label as the desktop draws it (each whole character one) */
static int cells_of(const char *s) {
    int n = 0;
    for(; *s; s++) if(((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

/* a label's characters as the desktop draws them (a label as put_label() leaves it: one cell each), their codes in
   cell[]; how many */
static int cells(const char *s, unsigned char *cell) {
    int n = 0, len;
    while(*s && n < WIN_MENU_LABEL) {
        cell[n++] = (unsigned char)gw_char(s, (int)strnlen(s, 4), &len);
        s += len;
    }
    return n;
}

/* whether n cells, without the blanks around them (a space, a no-break space), read as word in any case */
static int reads_as(const unsigned char *cell, int n, const char *word) {
    int a = 0, k;
    while(a < n && (cell[a] == ' ' || cell[a] == 0xFF)) a++;
    while(n > a && (cell[n - 1] == ' ' || cell[n - 1] == 0xFF)) n--;
    for(k = 0; a + k < n && word[k] && (cell[a + k] | 0x20) == word[k]; k++) ;
    return a + k == n && !word[k];
}

/* a title that would pass for one of the desktop's own (surfos/win.h), by the characters it draws: ≋ in it (code page
   437's 0xF7, which U+224B and U+2248 both draw), or SurfOS, Apps, Window or Help (*help: the program's Help, merged
   into the desktop's) */
static int desk_title(const char *s, int *help) {
    unsigned char cell[WIN_MENU_LABEL];
    int n = cells(s, cell), i;
    *help = 0;
    for(i = 0; i < n; i++) if(cell[i] == 0xF7) return 1;
    if(reads_as(cell, n, "surfos") || reads_as(cell, n, "apps") || reads_as(cell, n, "window")) return 1;
    *help = reads_as(cell, n, "help");
    return 0;
}

/* an item of the program's Help that would read as one of the desktop's own Help items, which follow it in the same
   menu (the desktop refuses one, as desk's fix round has it) */
static int desk_help_item(const char *s) {
    unsigned char cell[WIN_MENU_LABEL];
    int n = cells(s, cell);
    return reads_as(cell, n, "keyboard and mouse") || reads_as(cell, n, "about surfos");
}

/* the bar's cells so far and with a title more (the Help menu is not in the bar) */
static int bar_cells(const struct gk_menus *m, const char *more) {
    int i, cells = 0;
    for(i = 0; i < m->n; i++)
        if((m->e[i].flags & WIN_MENU_TITLE) && !(m->e[i].flags & WIN_MENU_HELP)) cells += cells_of(m->e[i].label) + 3;
    return cells + (more ? cells_of(more) + 3 : 0);
}

/* a key as a shortcut is kept and matched (surfos/win.h's keys): Ctrl and a letter as the letter in capitals with
   Ctrl (its control byte with WIN_MOD_CTRL, or alone from a desktop before the modifiers, but Backspace's 8, Tab, LF
   and Enter, which were those keys), a letter in capitals, a character that Shift makes ('+', '!') without Shift,
   and the modifiers but the others (a repeat) */
static unsigned shortcut_of(unsigned key, unsigned mods) {
    mods &= WIN_MOD_SHIFT | WIN_MOD_ALT | WIN_MOD_CTRL;
    if(key >= 1 && key <= 26 && ((mods & WIN_MOD_CTRL) || (key != 8 && key != 9 && key != 10 && key != 13))) {
        key = 'A' + key - 1;
        mods |= WIN_MOD_CTRL;
    } else if(key >= 'a' && key <= 'z') key = key - 'a' + 'A';
    else if(key > ' ' && key < 127 && !(key >= 'A' && key <= 'Z')) mods &= ~(unsigned)WIN_MOD_SHIFT;
    return mods << 16 | key;
}

static int add(struct gk_menus *m, int menu, unsigned id, unsigned flags, const char *label, unsigned shortcut,
               int radio, int sub) {
    struct gk_menu_entry *e;
    if(m->n == WIN_MENU_ENTRIES) return -1;
    if(shortcut) shortcut = shortcut_of(shortcut & 0xFFFF, shortcut >> 16);
    e = &m->e[m->n++];
    memset(e, 0, sizeof(*e));
    e->menu = menu;
    e->id = id;
    e->flags = flags;
    e->sub = sub;
    e->shortcut = shortcut;
    e->radio = (unsigned char)radio;
    put_label(e->label, sizeof(e->label), label);
    shortcut_text(shortcut, e->keys, sizeof(e->keys));
    m->sent = 0;
    return 0;
}

int gk_menu(struct gk *ui, const char *title) {
    struct gk_menus *m = menus_of(ui);
    char cut[WIN_MENU_LABEL];
    int help, number, tops = 0, i;
    if(!m || !title || !*title || m->numbers == GK_MENU_NUMBERS) return -1;
    put_label(cut, sizeof(cut), title);
    if(desk_title(cut, &help) || (help && m->help >= 0)) return -1;    /* the desktop's own, or a second Help */
    for(i = 0; i < m->numbers; i++) tops += m->parent[i] < 0;
    if(tops == WIN_MENUS || (!help && bar_cells(m, cut) > WIN_MENU_BAR)) return -1;
    number = m->numbers;
    if(add(m, number, (unsigned)number, WIN_MENU_TITLE | (help ? WIN_MENU_HELP : 0), cut, 0, 0, -1) < 0) return -1;
    m->parent[number] = -1;
    if(help) m->help = number;
    return m->numbers++;
}

int gk_submenu(struct gk *ui, int menu, int id, const char *label, unsigned flags) {
    struct gk_menus *m = ui->menus;
    int last, number;
    if(!m || !label || !*label || menu < 0 || menu >= m->numbers || m->parent[menu] >= 0 || menu == m->help ||
       m->numbers == GK_MENU_NUMBERS || count_of(m, menu, &last) == WIN_MENU_ITEMS) return -1;
    number = m->numbers;
    if(add(m, menu, (unsigned)id, WIN_MENU_SUBMENU | ((flags & GK_ITEM_OFF) ? WIN_MENU_DISABLED : 0), label, 0, 0,
           number) < 0)
        return -1;
    m->parent[number] = menu;
    return m->numbers++;
}

static int item(struct gk *ui, int menu, unsigned id, unsigned flags, const char *label, unsigned shortcut, int radio) {
    struct gk_menus *m = ui->menus;
    char cut[WIN_MENU_LABEL];
    int last, most;
    if(!m || menu < 0 || menu >= m->numbers) return -1;
    most = menu == m->help ? WIN_MENU_HELP_ITEMS : WIN_MENU_ITEMS;
    if(count_of(m, menu, &last) >= most) return -1;
    if(menu == m->help && !(flags & WIN_MENU_SEPARATOR)) {
        put_label(cut, sizeof(cut), label);
        if(desk_help_item(cut)) return -1;              /* About SurfOS is the desktop's */
    }
    return add(m, menu, id, flags, label, shortcut, radio, -1);
}

int gk_menu_item(struct gk *ui, int menu, int id, const char *label, unsigned shortcut, unsigned flags) {
    unsigned f = 0;
    if(!label || !*label) return -1;
    if(flags & GK_ITEM_OFF) f |= WIN_MENU_DISABLED;
    if((flags & GK_ITEM_ON) && (flags & GK_ITEM_RADIO)) f |= WIN_MENU_RADIO;
    else if((flags & GK_ITEM_ON) && (flags & GK_ITEM_CHECK)) f |= WIN_MENU_CHECKED;
    return item(ui, menu, (unsigned)id, f, label, shortcut, (flags & GK_ITEM_RADIO) != 0);
}

int gk_menu_line(struct gk *ui, int menu) {
    return item(ui, menu, 0, WIN_MENU_SEPARATOR, "", 0, 0);
}

static void set_flag(struct gk *ui, int id, unsigned flag, int on) {
    struct gk_menus *m = ui->menus;
    unsigned want, was;
    int i;
    if(!m) return;
    for(i = 0; i < m->n; i++) {
        struct gk_menu_entry *e = &m->e[i];
        if((e->flags & (WIN_MENU_TITLE | WIN_MENU_SEPARATOR)) || (int)e->id != id) continue;   /* a submenu's item too */
        want = flag == WIN_MENU_CHECKED && e->radio ? WIN_MENU_RADIO : flag;
        was = e->flags;
        if(on) e->flags |= want;
        else e->flags &= ~want;
        if(e->flags != was) m->sent = 0;
    }
}

void gk_menu_enable(struct gk *ui, int id, int on) {
    set_flag(ui, id, WIN_MENU_DISABLED, !on);
}

void gk_menu_check(struct gk *ui, int id, int on) {
    set_flag(ui, id, WIN_MENU_CHECKED, on);
}

int gk_menus_shown(const struct gk *ui) {
    return ui->menus && ui->menus->state == GK_MENUS_SHOWN;
}

/* the entries of menu (a submenu's as WIN_MENU_CHILD), each item that opens a submenu followed by its own; a submenu
   with no items is left out with its item, as the kernel refuses one. How many entries went into out */
static int emit(const struct gk_menus *m, int menu, int child, struct win_menu_item *out, int n) {
    int i, last, k;
    for(i = 0; i < m->n && n < WIN_MENU_ENTRIES; i++) {
        const struct gk_menu_entry *e = &m->e[i];
        if(e->menu != menu || (e->flags & WIN_MENU_TITLE)) continue;
        if((e->flags & WIN_MENU_SUBMENU) && (child || !count_of(m, e->sub, &last))) continue;
        out[n].id = e->id;
        out[n].flags = e->flags | (child ? WIN_MENU_CHILD : 0);
        memcpy(out[n].label, e->label, sizeof(out[n].label));
        memcpy(out[n].keys, e->keys, sizeof(out[n].keys));
        k = ++n;
        if(e->flags & WIN_MENU_SUBMENU) n = emit(m, e->sub, 1, out, k);
    }
    return n;
}

/* the menus to the desktop, whole, once they changed: those with items (the kernel refuses a menu with none) */
void gk_menus_send(struct gk *ui) {
    struct gk_menus *m = ui->menus;
    static struct win_menu_item out[WIN_MENU_ENTRIES];  /* copied by the kernel during the call */
    int i, n = 0, k, last;
    if(!m || m->sent) return;
    m->sent = 1;
    for(i = 0; i < m->n && n < WIN_MENU_ENTRIES; i++) {
        const struct gk_menu_entry *e = &m->e[i];
        if(!(e->flags & WIN_MENU_TITLE) || !count_of(m, e->menu, &last)) continue;
        out[n].id = e->id;
        out[n].flags = e->flags;
        memcpy(out[n].label, e->label, sizeof(out[n].label));
        memset(out[n].keys, 0, sizeof(out[n].keys));
        k = n + 1;
        n = emit(m, e->menu, 0, out, k);
        if(n == k) n--;                                 /* nothing in it went (an empty submenu alone): none */
    }
    if(gw_menu(ui->win, out, (unsigned)n) == 0) m->state = GK_MENUS_SHOWN;
    else if(errno == ENOSYS || errno == EINVAL) m->state = GK_MENUS_KEYS;
}

/* the menu an entry is reached through, and whether it can be: its menu's title not greyed, and for a submenu's item
   its item not greyed either */
static int reachable(const struct gk_menus *m, const struct gk_menu_entry *e, int *top) {
    int i, menu = e->menu;
    if(e->flags & (WIN_MENU_SEPARATOR | WIN_MENU_DISABLED | WIN_MENU_TITLE | WIN_MENU_SUBMENU)) return 0;
    if(m->parent[menu] >= 0) {                          /* a submenu's: its item, then its menu */
        for(i = 0; i < m->n; i++)
            if((m->e[i].flags & WIN_MENU_SUBMENU) && m->e[i].sub == menu && (m->e[i].flags & WIN_MENU_DISABLED)) return 0;
        menu = m->parent[menu];
    }
    for(i = 0; i < m->n; i++)
        if((m->e[i].flags & WIN_MENU_TITLE) && m->e[i].menu == menu && (m->e[i].flags & WIN_MENU_DISABLED)) return 0;
    *top = menu;
    return 1;
}

/* a key that is an enabled item's shortcut: GK_EV_MENU, as the desktop's bar would send it */
int gk_menu_key(struct gk *ui, unsigned key, unsigned mods) {
    struct gk_menus *m = ui->menus;
    struct gk_event *ev;
    int i, top;
    unsigned sc = shortcut_of(key, mods);
    if(!m) return 0;
    for(i = 0; i < m->n; i++) {
        const struct gk_menu_entry *e = &m->e[i];
        if(!e->shortcut || e->shortcut != sc || !reachable(m, e, &top)) continue;
        if((ev = gk_post_ev(ui, GK_EV_MENU, NULL, top))) ev->id = (int)e->id;
        return 1;
    }
    return 0;
}

/**** choosers ****/

int gk_choose(struct gk *ui, int id, int kind, const char *dir, const char *name, const char *types) {
    static const char *const titles[] = { "", "Open a file", "Save as", "Choose a folder" };
    struct win_dialog d;
    char path[WIN_PATH_MAX];
    if(!dir) dir = "";
    if(!name) name = "";
    if(!types) types = "";
    if(kind < WIN_DLG_OPEN || kind > WIN_DLG_FOLDER || strlen(dir) >= sizeof(d.dir) || strlen(name) >= sizeof(d.name) ||
       strlen(types) >= sizeof(d.types)) {
        errno = EINVAL;
        return -1;
    }
    if(!ui->user_act) {                                 /* only in answer to a person's own key, press or choice */
        errno = EACCES;
        return -1;
    }
    if(ui->choosing) {                                  /* one request at a time, a line typed instead too */
        errno = EBUSY;
        return -1;
    }
    if(ui->desk_calls) {                                /* a kernel with the calls: its own checks and answer */
        memset(&d, 0, sizeof(d));
        d.kind = (unsigned)kind;
        d.id = (unsigned)id;
        put_text(d.dir, sizeof(d.dir), dir);
        put_text(d.name, sizeof(d.name), name);
        put_text(d.types, sizeof(d.types), types);
        if(gw_dialog(ui->win, &d) == 0) {
            ui->choosing = 1;
            return 0;
        }
        if(errno == EAGAIN) {                           /* the events first: asked again once they are read */
            if(!(ui->later = malloc(sizeof(*ui->later)))) {
                ui->failed = 1;
                errno = ENOMEM;
                return -1;
            }
            *ui->later = d;
            ui->choosing = 1;
            return 0;
        }
        if(errno != ENOSYS) return -1;                  /* EINVAL, EBUSY, EACCES: the program hears why */
    }
    /* no choosers here: the path typed, in a line over the window, which starts in dir with name offered */
    if(dir[0] != '/') dir = "/";
    snprintf(path, sizeof(path), "%s%s%s", dir, dir[strlen(dir) - 1] != '/' ? "/" : "",
             kind == WIN_DLG_FOLDER ? "" : name);
    gk_box_chooser(ui, id, titles[kind], dir, path);
    return 0;
}

/* a request the desktop had no room for, asked again now that the window's events were read: taken, left waiting
   while the events still fill the queue, or answered as cancelled when the desktop refuses it now */
void gk_choose_again(struct gk *ui) {
    struct win_dialog *d = ui->later;
    int r;
    if(!d) return;
    r = gw_dialog(ui->win, d);
    if(r < 0 && errno == EAGAIN) return;
    ui->later = NULL;
    if(r < 0) {
        ui->choosing = 0;
        gk_post_text(ui, GK_EV_CHOSEN, (int)d->id, 0, NULL, 0);
    }
    free(d);
}

/* WIN_EV_DIALOG: the path taken now (WIN_TAKE, which works after a hang-up too), and the program told */
void gk_dialog_answer(struct gk *ui, const struct win_event *e) {
    char path[WIN_PATH_MAX];
    int n = -1;
    ui->choosing = 0;
    if(e->value == 1) n = gw_take(ui->win, path, sizeof(path));
    if(n > 0 && n < (int)sizeof(path)) gk_post_text(ui, GK_EV_CHOSEN, e->x, 1, path, n);
    else gk_post_text(ui, GK_EV_CHOSEN, e->x, 0, NULL, 0);
}

/**** the clipboard ****/

char *gk_paste_all(struct gk *ui, int *len) {
    char *buf;
    int n, r, tries;
    for(tries = 0; tries < 3; tries++) {
        if((n = gw_paste(ui->win, NULL, 0)) < 0) return NULL;
        if(!(buf = malloc((size_t)n + 1))) {
            ui->failed = 1;
            errno = ENOMEM;
            return NULL;
        }
        r = gw_paste(ui->win, buf, (unsigned)n + 1);
        if(r >= 0 && r <= n) {
            buf[r] = 0;
            if(len) *len = r;
            return buf;
        }
        gk_wipe(buf, (size_t)n + 1);
        free(buf);
        if(r >= 0 || errno != ENOSPC) return NULL;      /* (it grew meanwhile: again) */
    }
    errno = ENOSPC;
    return NULL;
}

int gk_paste(struct gk *ui, char *buf, unsigned size) {
    char *all;
    int n, len;
    if(!size) return 0;
    n = gw_paste(ui->win, buf, size);
    if(n >= 0 && (unsigned)n < size) return n;
    if(n >= 0 || errno == ENOSPC) {                     /* longer than buf: all of it, and as much as fits */
        if(!(all = gk_paste_all(ui, &len))) return -1;
        n = len < (int)size - 1 ? len : (int)size - 1;
        while(n && n < len && ((unsigned char)all[n] & 0xC0) == 0x80) n--;
        memcpy(buf, all, (size_t)n);
        buf[n] = 0;
        gk_wipe(all, (size_t)len);
        free(all);
        return n;
    }
    return -1;
}

/**** the theme ****/

/* the theme's colours by role, those the desktop fills (count). 1 when the kernel has the calls of 11/05/26 (it
   answered, or knows the op but not yet what it does: ENOSYS), 0 for one before them (EINVAL) */
int gk_theme_read(struct gk *ui) {
    struct win_theme w;
    struct gk_theme *t = &ui->theme;
    unsigned *slot[WIN_COLORS], i;
    if(gw_theme(ui->win, &w) < 0) return errno == ENOSYS;   /* the ocean's, as they are */
    slot[WIN_COLOR_DESKTOP] = &t->desktop;
    slot[WIN_COLOR_WINDOW] = &t->paper;
    slot[WIN_COLOR_TEXT] = &t->ink;
    slot[WIN_COLOR_MUTED] = &t->muted;
    slot[WIN_COLOR_ACCENT] = &t->teal;
    slot[WIN_COLOR_ON_ACCENT] = &t->on_teal;
    slot[WIN_COLOR_BAND] = &t->band;
    slot[WIN_COLOR_BUTTON] = &t->shell;
    slot[WIN_COLOR_LINE] = &t->line;
    slot[WIN_COLOR_SELECTION] = &t->pick;
    slot[WIN_COLOR_FIELD] = &t->sheet;
    slot[WIN_COLOR_FRAME] = &t->frame;
    slot[WIN_COLOR_DARK] = &t->shadow;
    slot[WIN_COLOR_LIGHT] = &t->foam;
    slot[WIN_COLOR_WARN] = &t->coral;
    slot[WIN_COLOR_DISABLED] = &t->grey;
    t->which = (int)w.theme;
    for(i = 0; i < WIN_COLORS && i < w.count && i < WIN_THEME_SLOTS; i++) *slot[i] = w.colors[i] & 0xFFFFFF;
    return 1;
}
