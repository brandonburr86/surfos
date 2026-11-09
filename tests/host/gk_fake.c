/*
SurfOS host tests: a fake desktop for programs' windows
--------------------
File: gk_fake.c Date: 11/05/26
--------------------
gk_fake.h's desktop: the kernel's window calls (include/surfos/win.h) as a program sees them, for
libgui and the toolkit built for the host (the desktop applications, track kit). One window at a
time; its descriptor is the read end of a pipe the test writes the desktop's events into, so that
libgui's read() and poll() are the host's own. Presents are copied into the desktop's picture of the
window, checked as the kernel checks them (within the window and within a row of the stride), so a
test sees what the desktop would show, and how much was presented. Track desk's calls (11/05/26:
WIN_MENU, WIN_PASTE, WIN_DIALOG, WIN_TAKE, WIN_THEME) are checked as kernel/win.c checks them (as desk's
1447a4c2 has them: its menu_cells(), reads_as(), desk_title(), desk_help_item() and menu_check() are copied
here, reading labels with the console's own UTF-8 decoder, driver/cp437.c's utf8_feed(), so that a
label the toolkit makes is held to the kernel's rule and not to the toolkit's idea of it) and then
done as the desktop will do them; gkf_lacks() makes them say ENOSYS, as until desk's stage 2, or
EINVAL, as a kernel before them. As desk's implementation (2c5afd30) and its fix round have it: WIN_DIALOG
says EAGAIN while WIN_EVENTS events wait unread in the window's pipe (FIONREAD) and EBUSY while an
answered path waits to be taken; WIN_PASTE gives the window its own WIN_CLIP until the desktop's
clipboard is newer (gkf_set_paste()); an item of the program's Help that reads as one of the desktop's
own Help items is refused. msleep() moves the clock on, nothing waits.
*/

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <surf.h>
#include "gk_fake.h"
#include "../../user/libsurf/guikit.h"

static struct {
    int rfd, wfd, open, gone;
    int w, h, min_w, min_h, max_w, max_h;
    unsigned flags, background;
    char title[WIN_TITLE_MAX];
    unsigned *screen;
    int desk_w, desk_h;
    int focused, refuse;
    unsigned now, buttons, mods;
    int presents, since, bx0, by0, bx1, by1;
    int nlog, nlast;                    /* the presents since the last look, and those of the last look */
    int log[64][4], last[64][4];
    char *clip, *paste;
    int clips, alerts;
    int clip_new;                       /* the window's WIN_CLIP, which the desktop has not made its clipboard yet */
    unsigned lacks;                     /* ops (1 << op) answered lack_err */
    int lack_err;
    struct win_menu_item menu[WIN_MENU_ENTRIES];
    int nmenu, menus_sent;
    struct win_dialog dialog;
    int dialog_waiting, have_path;
    int input;                          /* a key, a press or a menu choice sent since the last chooser asked for */
    unsigned sent_buttons;              /* the buttons of the pointer event sent last */
    char path[WIN_PATH_MAX];
    int themed;
    struct win_theme theme;
} F = { -1, -1 };

/* the ocean's colours by role, as kernel/win.c gives them */
static const unsigned ocean[WIN_COLORS] = {
    0x245A67, 0xF4F1E6, 0x153D49, 0x6C888C, 0x197D83, 0xF4F1E6, 0xE7E2D2, 0xDEE3D8,
    0xC9CFC4, 0xBFE3DD, 0xFFFDF5, 0x153D49, 0x102E3B, 0xB8E9D9, 0xEE906D, 0xA3B4B3
};

static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static void drop(void) {
    if(F.wfd >= 0) close(F.wfd);
    F.wfd = -1;
    free(F.screen);
    F.screen = NULL;
    F.open = 0;
}

void gkf_reset(int max_w, int max_h) {
    drop();
    free(F.clip);
    free(F.paste);
    memset(&F, 0, sizeof(F));
    F.rfd = F.wfd = -1;
    F.desk_w = max_w > 0 ? max_w : 958;
    F.desk_h = max_h > 0 ? max_h : 457;
    F.now = 1000;
    F.theme.theme = WIN_THEME_OCEAN;
    F.theme.count = WIN_COLORS;
    memcpy(F.theme.colors, ocean, sizeof(ocean));
}

void gkf_lacks(unsigned ops, int err) {
    F.lacks = ops;
    F.lack_err = err;
}

void gkf_refuse(int err) {
    F.refuse = err;
}

unsigned int uptime(void) {
    return F.now;
}

/* 11/05/26: a program that waits (wkit's busy spell): the clock moves on at once */
int msleep(unsigned int ms) {
    F.now += ms;
    return 0;
}

/* the events written into the window's pipe that it has not read (FIONREAD), as the kernel counts its queue */
static int unread(void) {
    int n = 0;
    if(F.rfd < 0 || ioctl(F.rfd, FIONREAD, &n) < 0) return 0;
    return n / (int)sizeof(struct win_event);
}

void gkf_advance(unsigned ms) {
    F.now += ms;
}

unsigned gkf_clock(void) {
    return F.now;
}

static void fill(unsigned *p, int n, unsigned c) {
    int i;
    for(i = 0; i < n; i++) p[i] = c;
}

int winopen(const struct win_open *o) {
    int fds[2], max_w, max_h, w, h;
    if(F.refuse) {
        errno = F.refuse;
        F.refuse = 0;
        return -1;
    }
    if(!o || o->version != WIN_VERSION || (o->flags & ~WIN_RESIZABLE) || o->background > 0xFFFFFF || !o->w || !o->h) {
        errno = EINVAL;
        return -1;
    }
    if(gkf_open()) {                                    /* one window at a time here */
        errno = EAGAIN;
        return -1;
    }
    drop();
    max_w = o->max_w && o->max_w < F.desk_w ? o->max_w : F.desk_w;
    max_h = o->max_h && o->max_h < F.desk_h ? o->max_h : F.desk_h;
    w = clamp(o->w, o->min_w, max_w);
    h = clamp(o->h, o->min_h, max_h);
    if(w < 1 || h < 1 || pipe(fds) < 0) {
        errno = w < 1 || h < 1 ? EINVAL : EMFILE;
        return -1;
    }
    if(!(F.screen = malloc((size_t)w * (size_t)h * 4))) {
        close(fds[0]);
        close(fds[1]);
        errno = ENOMEM;
        return -1;
    }
    F.rfd = fds[0];
    F.wfd = fds[1];
    F.open = 1;
    F.gone = 0;
    F.w = w;
    F.h = h;
    F.min_w = o->min_w;
    F.min_h = o->min_h;
    F.max_w = max_w;
    F.max_h = max_h;
    F.flags = o->flags;
    F.background = o->background;
    F.focused = 0;
    F.buttons = 0;
    memcpy(F.title, o->title, sizeof(F.title));
    F.title[sizeof(F.title) - 1] = 0;
    fill(F.screen, w * h, o->background);
    return fds[0];
}

static void presented(int x, int y, int w, int h) {
    F.presents++;
    if(F.nlog < 64) {
        F.log[F.nlog][0] = x;
        F.log[F.nlog][1] = y;
        F.log[F.nlog][2] = w;
        F.log[F.nlog++][3] = h;
    }
    if(!F.since++) {
        F.bx0 = x;
        F.by0 = y;
        F.bx1 = x + w;
        F.by1 = y + h;
        return;
    }
    if(x < F.bx0) F.bx0 = x;
    if(y < F.by0) F.by0 = y;
    if(x + w > F.bx1) F.bx1 = x + w;
    if(y + h > F.by1) F.by1 = y + h;
}

/**** track desk's calls, checked as kernel/win.c checks them ****/

/* driver/cp437.c's decoder (include/sys/cp437.h; its struct in plain types, as user/include/surfos/cp437.h declares
   the rest) */
struct utf8_dec { unsigned char have, need, b[4]; };
int utf8_feed(struct utf8_dec *d, unsigned char c, unsigned char *out);

/* kernel/win.c's menu_cells(): a label as the desktop draws it, its cells (their codes in cell[]), or -1 */
static int menu_cells(const char *s, unsigned char *cell) {
    struct utf8_dec d;
    unsigned char out[4], had, c;
    int cells = 0, k;
    memset(&d, 0, sizeof(d));
    for(; *s; s++) {
        c = (unsigned char)*s;
        if(c < 0x20 || c == 0x7F || (d.have == 1 && d.b[0] == 0xC2 && c < 0xA0)) return -1;
        had = d.have;
        k = utf8_feed(&d, c, out);
        if(c < 0x80) {
            if(had) return -1;
        } else if(!(!had && d.have == 1 && !k) && !(had && d.have == had + 1 && !k) && !(had && !d.have && k == 1))
            return -1;
        if(k && cell) cell[cells] = out[0];
        cells += k;
    }
    return d.have ? -1 : cells;
}

/* kernel/win.c's reads_as(): the cells drawn read as one of names (lower case), in any case, with blank cells around
   them (0xFF: the no-break space) */
static int reads_as(const unsigned char *cell, unsigned n, const char *const *names, unsigned count) {
    unsigned i, k;
    while(n && (cell[0] == ' ' || cell[0] == 0xFF)) cell++, n--;
    while(n && (cell[n - 1] == ' ' || cell[n - 1] == 0xFF)) n--;
    for(i = 0; i < count; i++) {
        for(k = 0; k < n && names[i][k] && (cell[k] | 0x20) == names[i][k]; k++) ;
        if(k == n && !names[i][k]) return 1;
    }
    return 0;
}

/* kernel/win.c's desk_title() and desk_help_item() (desk's review of its stage 2) */
static int desk_title(const unsigned char *cell, unsigned n, int help) {
    static const char *const own[] = { "surfos", "apps", "window", "help" };
    unsigned i;
    for(i = 0; i < n; i++) if(cell[i] == 0xF7) return 1;
    return reads_as(cell, n, own, help ? 3 : 4);
}

static int desk_help_item(const unsigned char *cell, unsigned n) {
    static const char *const own[] = { "keyboard and mouse", "about surfos" };
    return reads_as(cell, n, own, 2);
}

/* kernel/win.c's menu_check() (desk 1447a4c2), but that an array without its NUL is refused here (the kernel cuts it:
   the toolkit must never send one) */
static int menu_check(struct win_menu_item *it, unsigned n) {
    const unsigned known = WIN_MENU_TITLE | WIN_MENU_SEPARATOR | WIN_MENU_DISABLED | WIN_MENU_CHECKED | WIN_MENU_RADIO |
                           WIN_MENU_SUBMENU | WIN_MENU_CHILD | WIN_MENU_HELP;
    unsigned i, menus = 0, items = 0, children = 0, cells = 0, helps = 0, most = WIN_MENU_ITEMS;
    int sub = 0, help = 0, c;
    unsigned char cell[WIN_MENU_LABEL];
    for(i = 0; i < n; i++) {
        struct win_menu_item *e = &it[i];
        unsigned f = e->flags;
        int title = (f & WIN_MENU_TITLE) != 0, child = (f & WIN_MENU_CHILD) != 0, sep = (f & WIN_MENU_SEPARATOR) != 0;
        if((f & ~known) || (title && (f & (WIN_MENU_SEPARATOR | WIN_MENU_SUBMENU | WIN_MENU_CHILD))) ||
           ((f & WIN_MENU_HELP) && !title) || (child && (f & WIN_MENU_SUBMENU)) || (sep && (f & WIN_MENU_SUBMENU)))
            return -1;
        if(!memchr(e->label, 0, sizeof(e->label)) || !memchr(e->keys, 0, sizeof(e->keys))) return -1;
        if((c = menu_cells(e->label, cell)) < 0 || menu_cells(e->keys, NULL) < 0) return -1;
        if(title) {
            if((menus && !items) || (sub && !children) || ++menus > WIN_MENUS || !c) return -1;
            if(desk_title(cell, (unsigned)c, (f & WIN_MENU_HELP) != 0)) return -1;
            if((f & WIN_MENU_HELP) && ++helps > 1) return -1;
            if(!(f & WIN_MENU_HELP)) cells += (unsigned)c + 3;
            most = f & WIN_MENU_HELP ? WIN_MENU_HELP_ITEMS : WIN_MENU_ITEMS;
            help = (f & WIN_MENU_HELP) != 0;
            items = 0;
            sub = 0;
        } else if(help && !sep && desk_help_item(cell, (unsigned)c)) {
            return -1;
        } else if(child) {
            if(!sub || ++children > WIN_MENU_ITEMS || (!sep && !c)) return -1;
        } else {
            if(!menus || (sub && !children) || ++items > most || (!sep && !c)) return -1;
            sub = (f & WIN_MENU_SUBMENU) != 0;
            children = 0;
        }
    }
    if(n && (!items || (sub && !children))) return -1;
    return cells > WIN_MENU_BAR ? -1 : 0;
}

static int plain_text(const char *s, size_t size) {
    size_t i;
    for(i = 0; i < size && s[i]; i++) if((unsigned char)s[i] < 0x20 || s[i] == 0x7F) return 0;
    return i < size;
}

/* surfos/win.h's win_types() (seen only with the kernel's types): extensions of letters and digits, or whole names
   after '=' of letters, digits, '.', '-' and '_', 1 to WIN_TYPE_LEN each, WIN_TYPES at most, the NUL within size */
static int types_ok(const char *s, size_t size) {
    size_t i = 0, len;
    int n = 0;
    while(i < size && s[i]) {
        int whole = 0;
        if(s[i] == ' ') {
            i++;
            continue;
        }
        if(s[i] == '=') {
            whole = 1;
            i++;
        }
        for(len = 0; i + len < size && s[i + len] && s[i + len] != ' '; len++) {
            char c = s[i + len];
            if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 (whole && (c == '.' || c == '-' || c == '_')))) return 0;
        }
        if(!len || len > WIN_TYPE_LEN || n == WIN_TYPES || i + len >= size) return 0;
        n++;
        i += len;
    }
    return i < size;
}

static int desk_call(int op, void *arg) {
    const struct win_menu *m;
    const struct win_dialog *d;
    struct win_buffer *b;
    size_t n;
    switch(op) {
    case WIN_THEME:
        if(!arg) break;
        memcpy(arg, &F.theme, sizeof(F.theme));
        F.themed = 1;
        return 0;
    case WIN_MENU:
        m = arg;
        if(!m || (m->count && !m->items)) break;
        if(m->count > WIN_MENU_ENTRIES) goto invalid;
        {
            struct win_menu_item copy[WIN_MENU_ENTRIES];
            memcpy(copy, m->items, m->count * sizeof(copy[0]));
            if(menu_check(copy, m->count)) goto invalid;
            if(F.lacks & (1u << op)) goto lacking;
            memcpy(F.menu, copy, m->count * sizeof(copy[0]));
        }
        F.nmenu = (int)m->count;
        F.menus_sent++;
        return 0;
    case WIN_PASTE:
        b = arg;
        if(!b || (b->size && !b->buf)) break;
        if(F.lacks & (1u << op)) goto lacking;
        if(!F.focused) {
            errno = EACCES;
            return -1;
        }
        {                                               /* the window's own copy not yet taken is the newer */
            const char *text = F.clip_new && F.clip ? F.clip : F.paste ? F.paste : "";
            n = strlen(text);
            if(!b->size) return (int)n;
            if(b->size < n) {
                errno = ENOSPC;
                return -1;
            }
            memcpy(b->buf, text, n);
            if(b->size > n) b->buf[n] = 0;
            return (int)n;
        }
    case WIN_DIALOG:
        d = arg;
        if(!d) break;
        if(d->kind < WIN_DLG_OPEN || d->kind > WIN_DLG_FOLDER || d->flags || !plain_text(d->dir, sizeof(d->dir)) ||
           (d->dir[0] && d->dir[0] != '/') || !plain_text(d->name, sizeof(d->name)) || strchr(d->name, '/') ||
           !types_ok(d->types, sizeof(d->types))) goto invalid;
        if(F.lacks & (1u << op)) goto lacking;
        if(!F.focused) {
            errno = EACCES;
            return -1;
        }
        if(F.dialog_waiting || F.have_path) {           /* (desk's fix round: an answer's path not taken yet too) */
            errno = EBUSY;
            return -1;
        }
        if(!F.input) {                                  /* nothing the user did since the last request */
            errno = EACCES;
            return -1;
        }
        if(unread() >= WIN_EVENTS) {                    /* no place for the answer: the events first */
            errno = EAGAIN;
            return -1;
        }
        F.input = 0;
        F.dialog = *d;
        F.dialog_waiting = 1;
        return 0;
    case WIN_TAKE:
        b = arg;
        if(!b || (b->size && !b->buf)) break;
        if(F.lacks & (1u << op)) goto lacking;
        if(!F.have_path) {
            errno = ENOENT;
            return -1;
        }
        n = strlen(F.path);
        if(b->size < n + 1) {
            errno = ENOSPC;
            return -1;
        }
        memcpy(b->buf, F.path, n + 1);
        F.have_path = 0;
        return (int)n;
    }
    errno = EFAULT;
    return -1;
invalid:
    errno = EINVAL;
    return -1;
lacking:
    errno = F.lack_err;
    return -1;
}

int winctl(int fd, int op, void *arg) {
    const struct win_present *p;
    const struct win_clip *c;
    struct win_info *info;
    unsigned long v;
    int row, w, h;
    size_t n;
    if(!F.open || fd != F.rfd) {
        errno = EBADF;
        return -1;
    }
    if(op >= WIN_MENU && op <= WIN_THEME && (F.lacks & (1u << op)) && F.lack_err == EINVAL) {
        errno = EINVAL;                                 /* a kernel before them knows none of them */
        return -1;
    }
    if(op == WIN_THEME) return desk_call(op, arg);       /* (once the window is let go: the last theme it had) */
    if(F.gone && op != WIN_INFO && op != WIN_TAKE) {    /* (a path chosen just before is not lost) */
        errno = EPIPE;
        return -1;
    }
    if(op >= WIN_MENU && op <= WIN_TAKE) return desk_call(op, arg);
    switch(op) {
    case WIN_PRESENT:
        p = arg;
        if(!p) {
            errno = EFAULT;
            return -1;
        }
        if(p->stride > WIN_STRIDE_MAX || (unsigned)p->x + p->w > p->stride || p->x + p->w > F.w || p->y + p->h > F.h) {
            errno = EINVAL;
            return -1;
        }
        for(row = 0; row < p->h; row++)
            memcpy(F.screen + (size_t)(p->y + row) * F.w + p->x, p->pixels + (size_t)(p->y + row) * p->stride + p->x,
                   (size_t)p->w * 4);
        presented(p->x, p->y, p->w, p->h);
        return 0;
    case WIN_SETSIZE:
        v = (unsigned long)arg;
        w = (int)(v >> 16 & 0xFFFF);
        h = (int)(v & 0xFFFF);
        if(w < 1 || h < 1 || w < F.min_w || h < F.min_h || w > F.max_w || h > F.max_h) {
            errno = EINVAL;
            return -1;
        }
        free(F.screen);
        if(!(F.screen = malloc((size_t)w * (size_t)h * 4))) {
            errno = ENOMEM;
            F.w = F.h = 0;
            return -1;
        }
        F.w = w;
        F.h = h;
        fill(F.screen, w * h, F.background);
        return 0;
    case WIN_TITLE:
        if(!arg) {
            errno = EFAULT;
            return -1;
        }
        n = strlen(arg);
        if(n > sizeof(F.title) - 1) n = sizeof(F.title) - 1;
        while(n && (((const unsigned char *)arg)[n] & 0xC0) == 0x80) n--;   /* cut between characters */
        memcpy(F.title, arg, n);
        F.title[n] = 0;
        return 0;
    case WIN_INFO:
        info = arg;
        memset(info, 0, sizeof(*info));
        info->version = WIN_VERSION;
        info->w = (unsigned short)F.w;
        info->h = (unsigned short)F.h;
        info->screen_w = F.gone ? 0 : 960;
        info->screen_h = F.gone ? 0 : 540;
        info->scale = F.gone ? 0 : 2;
        info->focused = (unsigned short)F.focused;
        return 0;
    case WIN_CLIP:
        c = arg;
        if(!c || !c->len || c->len > WIN_CLIP_MAX || !c->text[0]) {
            errno = EINVAL;
            return -1;
        }
        if(!F.focused) {
            errno = EACCES;
            return -1;
        }
        for(n = 0; n < c->len && c->text[n]; n++) ;
        free(F.clip);
        if(!(F.clip = malloc(n + 1))) {
            errno = ENOMEM;
            return -1;
        }
        memcpy(F.clip, c->text, n);
        F.clip[n] = 0;
        F.clips++;
        F.clip_new = 1;
        return 0;
    case WIN_ALERT:
        F.alerts++;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

int gkf_open(void) {
    return F.open && fcntl(F.rfd, F_GETFD) != -1;
}

int gkf_w(void) { return F.w; }
int gkf_h(void) { return F.h; }
const char *gkf_title(void) { return F.title; }
int gkf_resizable(void) { return (F.flags & WIN_RESIZABLE) != 0; }
int gkf_presents(void) { return F.presents; }
const char *gkf_clip(void) { return F.clip ? F.clip : ""; }
int gkf_clips(void) { return F.clips; }
int gkf_alerts(void) { return F.alerts; }

void gkf_set_paste(const char *text) {
    free(F.paste);
    F.paste = text ? strdup(text) : NULL;
    F.clip_new = 0;                                     /* the desktop's clipboard is newer than the window's copy */
}

unsigned gkf_pixel(int x, int y) {
    if(!F.screen || x < 0 || y < 0 || x >= F.w || y >= F.h) return 0xFF000000u;   /* no colour of a window's */
    return F.screen[(size_t)y * F.w + x];
}

int gkf_text(int x, int y, const char *utf8, unsigned fg) {
    int cells = guikit_cells(utf8), i, j, ok = 1, lit;
    unsigned *glyphs;
    void *kit;
    if(!cells) return 1;
    glyphs = calloc((size_t)cells * 8 * 16, 4);
    kit = malloc(guikit_size());
    if(!glyphs || !kit) {
        free(glyphs);
        free(kit);
        return 0;
    }
    guikit_setup(kit, glyphs, cells * 8, 16);
    guikit_text(kit, 0, 0, utf8, 0xFFFFFF);
    for(j = 0; ok && j < 16; j++)
        for(i = 0; ok && i < cells * 8; i++) {
            lit = glyphs[j * cells * 8 + i] == 0xFFFFFF;
            if(lit ? gkf_pixel(x + i, y + j) != fg : gkf_pixel(x + i, y + j) == fg) ok = 0;
        }
    free(glyphs);
    free(kit);
    return ok;
}

int gkf_find(const char *utf8, unsigned fg, int *x, int *y) {
    int i, j;
    for(j = 0; j < F.h; j++)
        for(i = 0; i < F.w; i++)
            if(gkf_text(i, j, utf8, fg)) {
                if(x) *x = i;
                if(y) *y = j;
                return 1;
            }
    return 0;
}

int gkf_presented(int *x, int *y, int *w, int *h) {
    int n = F.since;
    *x = n ? F.bx0 : 0;
    *y = n ? F.by0 : 0;
    *w = n ? F.bx1 - F.bx0 : 0;
    *h = n ? F.by1 - F.by0 : 0;
    F.since = 0;
    memcpy(F.last, F.log, sizeof(F.last));
    F.nlast = F.nlog;
    F.nlog = 0;
    return n;
}

int gkf_presented_rect(int i, int *x, int *y, int *w, int *h) {
    if(i < 0 || i >= F.nlast) return 0;
    *x = F.last[i][0];
    *y = F.last[i][1];
    *w = F.last[i][2];
    *h = F.last[i][3];
    return 1;
}

/**** events ****/

static void put_event(unsigned short type, unsigned short mods, int x, int y, unsigned value) {
    struct win_event e;
    if(F.wfd < 0) return;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.mods = mods;
    e.x = x;
    e.y = y;
    e.value = value;
    if(write(F.wfd, &e, sizeof(e)) != (ssize_t)sizeof(e)) abort();   /* a test that fills a pipe is wrong */
    if(type == WIN_EV_KEY || type == WIN_EV_MENU || (type == WIN_EV_POINTER && x >= 0 && y >= 0 && (value & ~F.sent_buttons)))
        F.input = 1;                                    /* a key, a choice, a press over the content (1447a4c2) */
    if(type == WIN_EV_POINTER) F.sent_buttons = value;
}

void gkf_key(unsigned key, unsigned mods) {
    put_event(WIN_EV_KEY, (unsigned short)mods, 0, 0, key);
}

void gkf_type(const char *text) {
    for(; *text; text++) gkf_key(*text == '\n' ? '\r' : (unsigned char)*text, 0);
}

void gkf_pointer(int x, int y, unsigned buttons) {
    F.buttons = buttons;
    put_event(WIN_EV_POINTER, (unsigned short)F.mods, x, y, buttons);
}

void gkf_mods(unsigned mods) {
    F.mods = mods;
}

void gkf_click(int x, int y) {
    gkf_pointer(x, y, F.buttons | WIN_BUTTON_LEFT);
    gkf_pointer(x, y, F.buttons & ~WIN_BUTTON_LEFT);
}

void gkf_drag(int x0, int y0, int x1, int y1) {
    gkf_pointer(x0, y0, F.buttons | WIN_BUTTON_LEFT);
    gkf_pointer((x0 + x1) / 2, (y0 + y1) / 2, F.buttons);
    gkf_pointer(x1, y1, F.buttons);
    gkf_pointer(x1, y1, F.buttons & ~WIN_BUTTON_LEFT);
}

void gkf_wheel(int x, int y, int rows) {
    put_event(WIN_EV_WHEEL, (unsigned short)F.mods, x, y, (unsigned)rows);
}

void gkf_focus(int on) {
    F.focused = on != 0;
    put_event(WIN_EV_FOCUS, 0, 0, 0, (unsigned)F.focused);
}

void gkf_resize(int w, int h) {
    put_event(WIN_EV_RESIZE, 0, w, h, 0);
}

void gkf_close(void) {
    put_event(WIN_EV_CLOSE, 0, 0, 0, 0);
}

void gkf_gone(void) {
    if(F.dialog_waiting) {                              /* the desktop answers a request that waits: cancelled */
        F.dialog_waiting = 0;
        put_event(WIN_EV_DIALOG, 0, (int)F.dialog.id, 0, 0);
    }
    if(F.wfd >= 0) close(F.wfd);
    F.wfd = -1;
    F.gone = 1;
}

/**** track desk's calls, from the desktop's side ****/

int gkf_menu_count(void) {
    return F.nmenu;
}

int gkf_menus_sent(void) {
    return F.menus_sent;
}

const struct win_menu_item *gkf_menu_entry(int i) {
    return i >= 0 && i < F.nmenu ? &F.menu[i] : NULL;
}

int gkf_menu_find(const char *label) {
    int i;
    for(i = 0; i < F.nmenu; i++) if(!strcmp(F.menu[i].label, label)) return i;
    return -1;
}

/* the item of that id chosen in the bar, or in a submenu: WIN_EV_MENU, x its menu's id (a submenu's item too), as the
   desktop does it: never a disabled one, one in a disabled menu or submenu, or an item that opens a submenu */
int gkf_menu_choose(int id) {
    int i, title = -1, sub = -1;
    for(i = 0; i < F.nmenu; i++) {
        unsigned f = F.menu[i].flags;
        if(f & WIN_MENU_TITLE) {
            title = i;
            continue;
        }
        if(!(f & WIN_MENU_CHILD)) sub = f & WIN_MENU_SUBMENU ? i : -1;
        if((f & (WIN_MENU_SEPARATOR | WIN_MENU_SUBMENU)) || (int)F.menu[i].id != id) continue;
        if((f & WIN_MENU_DISABLED) || (title >= 0 && (F.menu[title].flags & WIN_MENU_DISABLED)) ||
           ((f & WIN_MENU_CHILD) && sub >= 0 && (F.menu[sub].flags & WIN_MENU_DISABLED))) return 0;
        put_event(WIN_EV_MENU, 0, title >= 0 ? (int)F.menu[title].id : 0, 0, (unsigned)id);
        return 1;
    }
    return 0;
}

const struct win_dialog *gkf_dialog(void) {
    return F.dialog_waiting ? &F.dialog : NULL;
}

void gkf_dialog_answer(const char *path) {
    if(!F.dialog_waiting) return;
    F.dialog_waiting = 0;
    F.have_path = path != NULL;
    if(path) snprintf(F.path, sizeof(F.path), "%s", path);
    put_event(WIN_EV_DIALOG, 0, (int)F.dialog.id, 0, path ? 1 : 0);
}

void gkf_set_theme(unsigned which, const unsigned *colors) {
    F.theme.theme = which;
    if(colors) memcpy(F.theme.colors, colors, WIN_COLORS * sizeof(F.theme.colors[0]));
    if(F.themed) put_event(WIN_EV_THEME, 0, 0, 0, which);
}
