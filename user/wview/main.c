/*
SurfOS user program: wview
--------------------
File: main.c    Date: 11/05/26
--------------------
The Image Viewer as a program with a window of its own (libgui, gui.h; the model is view.c and
folder.c): BMP, PNG, GIF and JPEG through libsurf's decoders (image.h), fitted, zoomed and moved
about as the desktop's Image Viewer showed BMP pictures, the folder's next and previous picture,
the file's details and Copy path. "wview FILE" shows FILE; the desktop starts it so from Files and
from its Image Viewer entry (shell/guiview.c), and Open asks the desktop's chooser (WIN_DIALOG, its
answer taken with WIN_TAKE).

A file is read whole, at most FILE_MAX bytes, then decoded, and its bytes go again: what is kept is
the picture as decoded, at most the decoders' 16 megapixels (64 MB). The picture shown stays until
another is good, so a file that is no picture, or too big for the memory there is, costs only a
line in the status bar. Esc, Ctrl+W or the close button end it; there is nothing to save. The
window takes the focus only as the desktop gives it (the launcher's rule). Status 0, or 2 when it
could not start (no window: not run on the desktop).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <surf.h>
#include <gui.h>
#include <image.h>
#include "view.h"

#define FILE_MAX (32u << 20)                    /* a file read whole: a BMP of 11 megapixels in 24 bits */

static struct view v;
static struct wimg pic;                         /* the picture shown, as decoded */
static struct folder folder;                    /* the pictures of the folder of the file opened last */
static char names[FOLDER_BYTES];
static char folder_dir[VIEW_PATH];              /* that folder */
static int cursor = -1;                         /* the picture N and P step from: the last one tried */
static unsigned shown;                          /* pictures shown so far: the room is drawn again for a new one */

/**** files ****/

/* the file's bytes, whole, in a malloc'ed buffer: 0, or -1 with why */
static int read_whole(const char *path, unsigned char **out, size_t *len, struct stat *st, char *why, size_t n) {
    unsigned char *buf;
    size_t got = 0;
    int fd = open(path, O_RDONLY), e;
    *out = NULL;
    *len = 0;
    if(fd < 0) {
        snprintf(why, n, "%s", strerror(errno));
        return -1;
    }
    if(fstat(fd, st) < 0 || !S_ISREG(st->st_mode)) {
        snprintf(why, n, "not a file");
        close(fd);
        return -1;
    }
    if((unsigned long long)st->st_size > FILE_MAX) {
        snprintf(why, n, "a file of %u MB: over the limit of %u MB", (unsigned)(st->st_size >> 20), FILE_MAX >> 20);
        close(fd);
        return -1;
    }
    buf = malloc((size_t)st->st_size + 1);      /* one byte more: a file that grew meanwhile is seen */
    if(!buf) {
        snprintf(why, n, "out of memory for a file of %u bytes", (unsigned)st->st_size);
        close(fd);
        return -1;
    }
    for(;;) {
        int r = (int)read(fd, buf + got, (size_t)st->st_size + 1 - got);
        if(r < 0 && errno == EINTR) continue;
        if(r < 0) {
            e = errno;
            snprintf(why, n, "%s", strerror(e));
            free(buf);
            close(fd);
            return -1;
        }
        if(r == 0 || (got += (size_t)r) > (size_t)st->st_size) break;
    }
    close(fd);
    if(got != (size_t)st->st_size) {
        snprintf(why, n, "it changed while it was read");
        free(buf);
        return -1;
    }
    *out = buf;
    *len = got;
    return 0;
}

/* what kind of picture the bytes are, by their first (image.c's signatures), into out; a BMP's bits a pixel too, as
   the desktop's Image Viewer said them ("24-bit"), from its header of 12 bytes or of 40 and more */
static void kind_of(const unsigned char *s, size_t n, char *out, size_t size) {
    if(n >= 8 && !memcmp(s, "\x89PNG\r\n\x1a\n", 8)) snprintf(out, size, "PNG");
    else if(n >= 3 && s[0] == 0xff && s[1] == 0xd8 && s[2] == 0xff) snprintf(out, size, "JPEG");
    else if(n >= 6 && (!memcmp(s, "GIF87a", 6) || !memcmp(s, "GIF89a", 6))) snprintf(out, size, "GIF");
    else if(n >= 30 && s[0] == 'B' && s[1] == 'M') {
        unsigned header = s[14] | (unsigned)s[15] << 8, bits = header == 12 ? (unsigned)(s[24] | s[25] << 8) : (unsigned)(s[28] | s[29] << 8);
        snprintf(out, size, "%u-bit BMP", bits);
    } else snprintf(out, size, "BMP");
}

/* path made whole: from the current folder when it is not from the root. 0, or -1 when it would be too long */
static int whole_path(const char *path, char *out, size_t n) {
    char here[VIEW_PATH];
    if(path[0] == '/') return snprintf(out, n, "%s", path) < (int)n ? 0 : -1;
    if(!getcwd(here, sizeof(here))) return -1;
    return snprintf(out, n, "%s%s%s", here, here[strlen(here) - 1] == '/' ? "" : "/", path) < (int)n ? 0 : -1;
}

/* the folder of path, a whole path (its pictures, in order), and path's place among them */
static void list_folder(const char *path) {
    const char *name = view_name(path);
    size_t k = (size_t)(name - path);
    struct dirent *e;
    DIR *d;
    folder_start(&folder, names);
    cursor = -1;
    v.pictures = 0;
    snprintf(folder_dir, sizeof(folder_dir), "%.*s", (int)(k > 1 ? k - 1 : k), path);
    d = opendir(folder_dir);
    if(!d) return;
    while((e = readdir(d)) != NULL) if(e->d_type == DT_REG) folder_add(&folder, e->d_name);
    closedir(d);
    folder_sort(&folder);
    cursor = folder_find(&folder, name);
    v.pictures = folder.count;
}

/* path shown: 0; or -1, the picture before staying, and the status bar saying why. list: path was chosen (not
   stepped to), so its folder is listed, and N and P step from it even when it is no picture */
static int show(const char *path, int list) {
    char full[VIEW_PATH], why[WIMG_ERR_MAX + 64];
    unsigned char *bytes;
    struct wimg m;
    struct stat st;
    size_t len;
    char kind[24];
    if(whole_path(path, full, sizeof(full))) {
        snprintf(v.status, sizeof(v.status), "%s: the path is too long", view_name(path));
        return -1;
    }
    if(list) {
        list_folder(full);
        v.file.index = v.file.count = 0;        /* the picture shown is none of these until the new one shows */
    }
    if(read_whole(full, &bytes, &len, &st, why, sizeof(why))) {
        snprintf(v.status, sizeof(v.status), "%s: %s", view_name(full), why);
        return -1;
    }
    kind_of(bytes, len, kind, sizeof(kind));
    if(wimg_decode(bytes, len, &m, why, sizeof(why))) {
        free(bytes);
        snprintf(v.status, sizeof(v.status), "%s: %s", view_name(full), why);
        return -1;
    }
    free(bytes);
    wimg_free(&pic);
    pic = m;
    memset(&v.file, 0, sizeof(v.file));
    snprintf(v.file.path, sizeof(v.file.path), "%s", full);
    snprintf(v.file.kind, sizeof(v.file.kind), "%s", kind);
    v.file.size = (unsigned)st.st_size;
    v.file.mtime = (unsigned)st.st_mtime;
    v.file.w = m.w;
    v.file.h = m.h;
    v.file.index = cursor >= 0 ? cursor + 1 : 0;
    v.file.count = cursor >= 0 ? folder.count : 0;
    view_picture(&v, m.w, m.h);
    v.status[0] = 0;
    shown++;
    return 0;
}

/* the folder's picture step away from the cursor, around the ends; one that is no picture is said, and the next step
   goes on from it */
static void step(int by) {
    char path[VIEW_PATH];
    int next = folder_step(&folder, cursor, by);
    if(next < 0) return;
    cursor = next;
    if(snprintf(path, sizeof(path), "%s/%s", strcmp(folder_dir, "/") ? folder_dir : "", folder_name(&folder, next)) >=
       (int)sizeof(path)) {
        snprintf(v.status, sizeof(v.status), "%s: the path is too long", folder_name(&folder, next));
        return;
    }
    if(show(path, 0)) {
        char said[VIEW_STATUS];
        snprintf(said, sizeof(said), "%s", v.status);
        snprintf(v.status, sizeof(v.status), "%.120s (%d of %d)", said, next + 1, folder.count);
    }
}

/**** the desktop's chooser ****/

#define OPEN_ID 1                               /* our WIN_DIALOG request's id, WIN_EV_DIALOG's x */

/* Open: the desktop's chooser, in the picture's folder (else where its last chooser was), for the kinds read here.
   Its answer comes as WIN_EV_DIALOG (opened()); what keeps it from coming up is said */
static void ask_open(struct gw *win) {
    struct win_dialog d;
    memset(&d, 0, sizeof(d));
    d.kind = WIN_DLG_OPEN;
    d.id = OPEN_ID;
    if(strlen(folder_dir) < sizeof(d.dir)) snprintf(d.dir, sizeof(d.dir), "%s", folder_dir);
    snprintf(d.types, sizeof(d.types), "bmp png gif jpg jpeg");
    if(winctl(win->fd, WIN_DIALOG, &d) == 0) return;
    snprintf(v.status, sizeof(v.status), "%s", errno == EBUSY ? "Open: the chooser is open already" :
             errno == EAGAIN ? "Open: the desktop is busy: try again" : errno == EACCES ? "Open: the window must have the focus" :
             errno == EINVAL ? "Open: this desktop has no chooser for programs" : "Open: the chooser could not be shown");
}

/* its answer: the path chosen, taken at once and shown; a chooser cancelled (or put away for another dialog) does
   nothing. A path is taken whatever request it answers: one left there would keep the next WIN_DIALOG busy */
static void opened(struct gw *win, const struct win_event *ev) {
    char path[WIN_PATH_MAX];
    struct win_buffer b;
    if(ev->value != 1) return;
    b.buf = path;
    b.size = sizeof(path);
    if(winctl(win->fd, WIN_TAKE, &b) < 0) {
        snprintf(v.status, sizeof(v.status), "Open: the path chosen could not be taken");
        return;
    }
    if(ev->x == OPEN_ID) show(path, 1);
}

/**** drawing ****/

/* at most cells characters, the last a dot when it goes on (the desktop's gd_name()) */
static void name(struct gw *win, int x, int y, const char *s, int cells, unsigned color) {
    char b[VIEW_STATUS + 8];
    if(cells > VIEW_STATUS) cells = VIEW_STATUS;
    if(cells < 1) return;
    if((int)strlen(s) <= cells) {
        gw_text(win, x, y, s, color);
        return;
    }
    snprintf(b, sizeof(b), "%.*s\xe2\x88\x99", cells - 1, s);
    gw_text(win, x, y, b, color);
}

static void draw_bar(struct gw *win) {
    struct view_tool t;
    int i, on, lit;
    gw_rect(win, 0, 0, win->w, VIEW_BAR_H + 2, GW_BAND);
    for(i = 0; i < VT_COUNT; i++) {
        view_tool(i, &t);
        on = view_tool_on(&v, i);
        lit = i == VT_DETAILS && v.details;
        gw_button(win, t.x, t.y, t.w, t.h, view_tool_label(i), lit ? GW_TEAL : on ? GW_SHELL : GW_BAND,
                  lit ? GW_PAPER : on ? GW_INK : GW_MUTED);
    }
    gw_rect(win, 0, VIEW_BAR_H + 2, win->w, 1, GW_LINE);
}

static void draw_details(struct gw *win, const struct view_room *r) {
    char lines[VIEW_DETAILS][VIEW_DETAIL];
    int n = view_details(&v, lines), i, w = 0, h, x, y, cells;
    if(n > (r->h - 26) / 18) n = (r->h - 26) / 18;      /* the lines the room has room for */
    if(n <= 0) return;
    for(i = 0; i < n; i++) if((int)strlen(lines[i]) > w) w = (int)strlen(lines[i]);
    cells = (r->w - 40) / 8;
    if(w > cells) w = cells;
    w = w * 8 + 24;
    h = n * 18 + 14;
    x = r->x + 12;
    y = r->y + 12;
    gw_rect(win, x, y, w, h, GW_PAPER);
    gw_frame(win, x, y, w, h, GW_LINE);
    for(i = 0; i < n; i++) name(win, x + 12, y + 8 + i * 18, lines[i], cells, i ? GW_INK : GW_TEAL);
}

static void draw_room(struct gw *win) {
    struct view_room r;
    view_room(&v, &r);
    gw_rect(win, r.x, r.y, r.w, r.h, GW_NAVY);
    if(v.pw > 0) view_paint(&v, pic.px, win->pixels, win->w);
    else {                                      /* what became of the last picture, and how to open one */
        int y = r.y + 16;
        if(v.status[0]) {
            name(win, r.x + 16, y, v.status, (r.w - 32) / 8, GW_CORAL);
            y += 22;
        }
        name(win, r.x + 16, y, "Open a picture: the toolbar's Open, or O.", (r.w - 32) / 8, GW_FOAM);
    }
    if(v.details) draw_details(win, &r);
}

static void draw_status(struct gw *win) {
    char left[VIEW_STATUS + VIEW_NAME + 64], right[32];
    int y = win->h - VIEW_STATUS_H, x;
    view_status(&v, left, sizeof(left), right, sizeof(right));
    gw_rect(win, 0, y, win->w, VIEW_STATUS_H, GW_BAND);
    gw_rect(win, 0, y, win->w, 1, GW_LINE);
    x = win->w - 23 - (int)strlen(right) * 8;    /* clear of the resize grip */
    name(win, 9, y + 3, left, (x - 18) / 8, GW_INK);
    gw_text(win, x, y + 3, right, GW_MUTED);
}

/* what each part shows, so that only a part that changed is drawn and presented */
static unsigned mix(unsigned h, unsigned x) { return (h ^ x) * 16777619u; }
static unsigned mix_text(unsigned h, const char *s) {
    while(*s) h = mix(h, (unsigned char)*s++);
    return mix(h, 0);
}

static unsigned bar_now(void) {
    unsigned h = 2166136261u;
    int i;
    for(i = 0; i < VT_COUNT; i++) h = mix(h, (unsigned)view_tool_on(&v, i));
    return mix(h, (unsigned)v.details);
}

static unsigned status_now(void) {
    char left[VIEW_STATUS + VIEW_NAME + 64], right[32];
    view_status(&v, left, sizeof(left), right, sizeof(right));
    return mix_text(mix_text(2166136261u, left), right);
}

static unsigned room_now(void) {
    char lines[VIEW_DETAILS][VIEW_DETAIL];
    unsigned h = 2166136261u;
    int x, y, w, ht, i, n;
    view_place(&v, &x, &y, &w, &ht);
    h = mix(mix(mix(mix(mix(h, shown), (unsigned)v.pw), (unsigned)x), (unsigned)y), (unsigned)w);
    h = mix(mix(h, (unsigned)ht), (unsigned)v.details);
    if(!v.pw) h = mix_text(h, v.status);
    if(v.details) for(n = view_details(&v, lines), i = 0; i < n; i++) h = mix_text(h, lines[i]);
    return h;
}

/**** the window ****/

int main(int argc, char **argv) {
    struct win_event ev;
    struct gw *win;
    char title[80], titled[80] = "";
    unsigned buttons = 0, bar = 0, room = 0, status = 0;
    int r, act, all = 1;
    if(argc > 2 || (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")))) {
        fprintf(stderr, "usage: wview [FILE]\n");
        return 2;
    }
    win = gw_open_limits("Image Viewer", VIEW_W, VIEW_H, VIEW_MIN_W, VIEW_MIN_H, 0, 0, WIN_RESIZABLE, GW_NAVY);
    if(!win) {
        fprintf(stderr, "wview: no window: %s\n", strerror(errno));
        return 2;
    }
    view_init(&v, win->w, win->h);
    v.now = uptime();
    if(argc == 2) show(argv[1], 1);
    for(;;) {
        unsigned b, s, m;
        view_title(&v, title, sizeof(title));
        if(strcmp(title, titled)) {
            gw_title(win, title);
            snprintf(titled, sizeof(titled), "%s", title);
        }
        b = bar_now();
        s = status_now();
        m = room_now();
        if(all || b != bar) draw_bar(win);
        if(all || m != room) draw_room(win);
        if(all || s != status) draw_status(win);
        if(all) r = gw_present_all(win);
        else {
            struct view_room vr;
            view_room(&v, &vr);
            r = 0;
            if(b != bar) r |= gw_present(win, 0, 0, win->w, VIEW_ROOM_Y);
            if(m != room) r |= gw_present(win, vr.x, vr.y, vr.w, vr.h);
            if(s != status) r |= gw_present(win, 0, win->h - VIEW_STATUS_H, win->w, VIEW_STATUS_H);
        }
        if(r < 0 && errno == EPIPE) break;
        bar = b;
        status = s;
        room = m;
        all = 0;
        r = gw_next(win, &ev, -1);
        if(r < 0) break;                        /* the desktop let it go, or a kill */
        if(r == 0) continue;
        v.now = uptime();
        act = VIEW_NOTHING;
        switch(ev.type) {
        case WIN_EV_CLOSE: act = VIEW_CLOSE; break;
        case WIN_EV_KEY:                        /* with Alt, a key is some other program's business */
            if((ev.mods & WIN_MOD_ALT) || ((ev.mods & WIN_MOD_REPEAT) && !view_repeats((int)ev.value))) break;
            act = view_key(&v, (int)ev.value);
            break;
        case WIN_EV_DIALOG: opened(win, &ev); break;
        case WIN_EV_POINTER:                    /* the left button: a press, a drag, a release */
            if((ev.value & WIN_BUTTON_LEFT) && !(buttons & WIN_BUTTON_LEFT)) act = view_press(&v, ev.x, ev.y);
            else if(ev.value & WIN_BUTTON_LEFT) view_drag(&v, ev.x, ev.y);
            else if(buttons & WIN_BUTTON_LEFT) view_release(&v);
            buttons = ev.value;
            break;
        case WIN_EV_WHEEL: view_wheel(&v, (int)ev.value); break;
        case WIN_EV_RESIZE:                     /* the image was made again at the new size, all background */
            view_resize(&v, win->w, win->h);
            all = 1;
            break;
        }
        if(act == VIEW_CLOSE) break;
        switch(act) {
        case VIEW_NEXT: step(1); break;
        case VIEW_PREVIOUS: step(-1); break;
        case VIEW_COPY:
            if(gw_clip(win, v.file.path, (unsigned)strlen(v.file.path)) == 0) snprintf(v.status, sizeof(v.status), "Copied the path");
            else snprintf(v.status, sizeof(v.status), "%s", errno == EACCES ? "Not copied: the window must have the focus" :
                          "Not copied");
            break;
        case VIEW_OPEN: ask_open(win); break;
        }
    }
    gw_close(win);
    wimg_free(&pic);
    return 0;
}
