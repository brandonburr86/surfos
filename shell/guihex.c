/*
SurfOS Desktop: Hex
--------------------
File: guihex.c Date: 10/17/26
--------------------
Any file's bytes, in hexadecimal and as characters, read-only. Only what the window shows is
read: a page of GUI_HEX_PAGE bytes from the first row shown, with gui_fs's read_at, from the
handlers and the tick (which reads again when the window grew), never while drawing; so a file
of any size can be shown, its offsets 32 bits (up to 4 GB). A row is the offset, 16 bytes in
hexadecimal and the same bytes as characters: printable ASCII as it is, any other byte as a grey
dot. The cursor is a byte, a drag selects several, and Copy puts them on the clipboard in
hexadecimal ("DE AD BE EF"); Copy offset puts the cursor's ("0x0000001F"). Go to offset takes
hexadecimal (0x1F00) or decimal (7936). Find looks for text or for bytes in hexadecimal from the
cursor on, a piece at a time in the tick, at most HEX_TICK bytes a frame (Esc stops it), with
Knuth-Morris-Pratt's table, which carries a match that runs across two pieces from one to the next.
Track MEDIA of docs/superpowers/plans/2026-10-17-gui-suite.md; the window manager reaches it
through gui_app_hex (struct gui_app, shell/guipriv.h).
*/
#include "gui_media.h"
#include <surfos/math64.h>
#include <string.h>
#include <stdio.h>

#define HEX_TICK (256 * 1024)           /* the most a frame reads of a search */
#define OFF_X 10                        /* the offsets' column, window-relative */
#define HEX_X (OFF_X + 8 * 8 + 16)      /* the first byte's two digits */

/* the toolbar's buttons and Hex's own dialogs */
enum { HA_OPEN = 1, HA_GOTO, HA_FIND, HA_NEXT, HA_COPY, HA_OFFSET };
static const struct gui_tool tools[] = {
    { "Open", HA_OPEN }, { "Go to", HA_GOTO }, { "Find", HA_FIND }, { "Find next", HA_NEXT }, { "Copy", HA_COPY },
    { "Copy offset", HA_OFFSET },
};
#define TOOLS (int)(sizeof(tools) / sizeof(tools[0]))

static const char *const help[] = {
    "#Hex",
    "Any file's bytes, read-only: the offset, 16 bytes in",
    "hexadecimal, and the bytes as characters (printable",
    "ASCII; a grey dot for any other byte). Open (or O)",
    "chooses a file, of any size up to 4 GB. The arrows, Page",
    "Up and Down, Home, End, the wheel and the scroll bar",
    "move. Click a byte, drag over several, or Ctrl+A for all;",
    "Ctrl+C copies them in hexadecimal, Copy offset the",
    "cursor's offset. Ctrl+L goes to an offset (0x1F00 is",
    "hexadecimal, 7936 decimal). Ctrl+F finds bytes from the",
    "selection on: text, or hexadecimal as DE AD BE EF or",
    "0xDEADBEEF; one byte is 0xFF (FF alone is text, and so is",
    "\"DE AD\" in quotes). N or Find next finds the next one,",
    "Esc stops a search.",
};

/**** where things are ****/

struct hex_layout { int head_y, list_y, rows, bar_x, status_y, pitch, chr_x; };

/* the bytes' pitch: as roomy as the window allows (24 pixels, the width of "00 "), down to 20 */
static void layout(const struct gui_state *g, struct hex_layout *l) {
    const struct gui_window *w = &g->win[GUI_HEX];
    l->head_y = TITLE_H + BAR_H + 3;
    l->list_y = l->head_y + 18;
    l->status_y = w->h - STATUS_H - 1;
    l->rows = (l->status_y - l->list_y) / 16;
    if(l->rows < 1) l->rows = 1;
    l->bar_x = w->w - 2 - SCROLL_W;
    l->pitch = HEX_X + 15 * 24 + 40 + 128 + 8 <= l->bar_x ? 24 : HEX_X + 15 * 22 + 40 + 128 + 8 <= l->bar_x ? 22 : 20;
    l->chr_x = HEX_X + 15 * l->pitch + 40;
}

static int min_width(void) {
    return HEX_X + 15 * 20 + 40 + 128 + 8 + SCROLL_W + 2;
}

static int byte_x(const struct hex_layout *l, int i) {
    return HEX_X + i * l->pitch + (i >= 8 ? 8 : 0);
}

static u32 rows_of(u32 size) {
    return size / 16 + (size % 16 != 0);
}

/* the last useful first byte: the view ends with the file */
static u32 top_max(const struct gui_state *g) {
    struct hex_layout l;
    u32 rows = rows_of(g->hex.size);
    layout(g, &l);
    return rows > (u32)l.rows ? (rows - l.rows) * 16 : 0;
}

/* the scroll bar's rows, scaled down until gd_thumb's arithmetic stays inside an int */
static int bar_shift(u32 rows) {
    int k = 0;
    while((rows >> k) > 65536) k++;
    return k;
}

/**** reading ****/

/* the page holds the rows shown, or reads them: after the view moved, or the window grew */
static void fill(struct gui_state *g) {
    struct gui_hex *h = &g->hex;
    struct hex_layout l;
    u32 want, end, len, size;
    int r, tries;
    if(!h->path[0]) return;
    layout(g, &l);
    want = (u32)l.rows * 16;
    for(tries = 0; tries < 2; tries++) {                /* again once, when the file was found shorter than the view */
        end = h->top >= h->size ? h->top : h->size - h->top < want ? h->size : h->top + want;
        if(h->page_ok && h->top >= h->page_off && end <= h->page_off + h->page_len) return;
        if(h->page_bad && h->bad_top == h->top) return; /* it failed here: not again every frame */
        len = size = 0;
        r = g->fs && g->fs->read_at ? g->fs->read_at(g->fs->ctx, h->path, h->top, (char *)h->page, GUI_HEX_PAGE, &len,
                                                      &size) : -ENOSYS;
        g->dirty = true;
        if(r) {
            h->page_ok = false;
            h->page_bad = true;
            h->bad_top = h->top;
            snprintf(h->status, sizeof(h->status), "Cannot read at 0x%08X: %s", h->top, gui_error(g, r));
            return;
        }
        h->page_bad = false;
        h->page_ok = true;
        h->page_off = h->top;
        h->page_len = len;
        if(size == h->size) return;
        h->size = size;                                 /* it changed since: the cursor stays inside it */
        if(h->cursor >= size) h->cursor = size ? size - 1 : 0;
        if(h->anchor >= size) h->anchor = h->cursor;
        if(h->top <= top_max(g)) return;
        h->top = top_max(g);
    }
}

/* the cursor's row in view, then the page read */
static void show_cursor(struct gui_state *g) {
    struct gui_hex *h = &g->hex;
    struct hex_layout l;
    u32 row = h->cursor / 16 * 16;
    layout(g, &l);
    if(row < h->top) h->top = row;
    if(row - h->top >= (u32)l.rows * 16) h->top = row - (u32)(l.rows - 1) * 16;   /* (top + rows * 16 may pass 4 GB) */
    if(h->top > top_max(g)) h->top = top_max(g);
    fill(g);
    g->dirty = true;
}

static void stop_search(struct gui_hex *h) {
    h->searching = false;
}

void hex_show(struct gui_state *g, const char *path) {
    struct gui_hex *h = &g->hex;
    struct gui_entry e;
    bool device = false, readonly = false;
    char line[80];
    int r;
    gui_open(g, GUI_HEX);
    if(!g->fs || !g->fs->read_at) return;
    r = g->fs->stat(g->fs->ctx, path, &e, &device, &readonly);
    if(!r && e.type == GUI_DIR) r = -EISDIR;
    if(!r && device) {
        snprintf(line, sizeof(line), "%s is a device, not a file.", gui_basename(path));
        dialog_message(g, "Cannot open the file", line, "Hex opens files.");
        return;
    }
    if(r) {
        snprintf(line, sizeof(line), "%s: %s", gui_basename(path), gui_error(g, r));
        dialog_message(g, "Cannot open the file", line, "");
        return;
    }
    stop_search(h);
    strlcpy(h->path, path, sizeof(h->path));
    h->size = e.size;
    h->top = h->cursor = h->anchor = 0;
    h->selecting = h->thumb = h->found = false;
    h->page_ok = h->page_bad = false;
    h->status[0] = 0;
    fill(g);
}

/**** moving ****/

/* the cursor moved by delta bytes (within the file); page: the view moves with it */
static void move(struct gui_state *g, i64 delta, bool page) {
    struct gui_hex *h = &g->hex;
    i64 to = (i64)h->cursor + delta, last = h->size ? (i64)h->size - 1 : 0;
    i64 top = (i64)h->top + (page ? delta : 0);
    h->cursor = h->anchor = (u32)(to < 0 ? 0 : to > last ? last : to);
    if(page) h->top = (u32)(top < 0 ? 0 : top > (i64)top_max(g) ? (i64)top_max(g) : top) / 16 * 16;
    h->status[0] = 0;
    show_cursor(g);
}

static void scroll_to(struct gui_state *g, u32 top) {
    struct gui_hex *h = &g->hex;
    h->top = top > top_max(g) ? top_max(g) : top / 16 * 16;
    fill(g);
    g->dirty = true;
}

/**** finding ****/

static int digit(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* what to look for: "0x" and hexadecimal digits (spaces between pairs allowed), or two or more pairs of
   digits with single spaces between them, are bytes; text in double quotes, or anything else, is text.
   The length, 0 for nothing, -1 for digits that are not bytes, -2 for too long */
static int pattern(const char *s, u8 *out, u32 max) {
    u32 n = strlen(s), k = 0, i, pairs = 0;
    bool hex = n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    if(!n) return 0;
    if(n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        if(n - 2 > max) return -2;
        memcpy(out, s + 1, n - 2);
        return (int)(n - 2);
    }
    if(!hex) {                                          /* "DE AD BE EF" */
        for(i = 0; i + 1 < n && digit(s[i]) >= 0 && digit(s[i + 1]) >= 0 && (i + 2 == n || s[i + 2] == ' '); i += 3)
            pairs++;
        hex = pairs >= 2 && i >= n;
    }
    if(!hex) {
        if(n > max) return -2;
        memcpy(out, s, n);
        return (int)n;
    }
    for(i = s[0] == '0' && (s[1] == 'x' || s[1] == 'X') ? 2 : 0; i < n; ) {
        if(s[i] == ' ') { i++; continue; }
        if(i + 1 >= n || digit(s[i]) < 0 || digit(s[i + 1]) < 0) return -1;
        if(k >= max) return -2;
        out[k++] = (u8)(digit(s[i]) << 4 | digit(s[i + 1]));
        i += 2;
    }
    return k ? (int)k : -1;
}

/* Knuth-Morris-Pratt: fail[i], the longest proper prefix of pattern[0..i] that is also its suffix */
static void kmp_table(struct gui_hex *h) {
    u32 i, k = 0;
    h->fail[0] = 0;
    for(i = 1; i < h->plen; i++) {
        while(k && h->pattern[i] != h->pattern[k]) k = h->fail[k - 1];
        if(h->pattern[i] == h->pattern[k]) k++;
        h->fail[i] = (u8)k;
    }
}

static u32 percent(u32 part, u32 whole) {
    return whole ? (u32)div64_32((u64)part * 100, whole, NULL) : 100;
}

/* some more of the search: pieces of the file through the table, up to HEX_TICK bytes */
static void search_some(struct gui_state *g) {
    struct gui_hex *h = &g->hex;
    u32 budget = HEX_TICK, n, len, size, i, q = h->matched;
    int r;
    if(!h->plen) stop_search(h);                        /* nothing to look for: never a match of nothing */
    while(h->searching && budget) {
        n = budget < GUI_HEX_PIECE ? budget : GUI_HEX_PIECE;
        len = size = 0;
        r = g->fs && g->fs->read_at ? g->fs->read_at(g->fs->ctx, h->path, h->scan, (char *)h->piece, n, &len, &size)
                                    : -ENOSYS;
        if(r) {
            stop_search(h);
            snprintf(h->status, sizeof(h->status), "Find stopped at 0x%08X: %s", h->scan, gui_error(g, r));
            g->dirty = true;
            return;
        }
        if(size != h->size) {                           /* it changed since: the cursor stays inside it */
            h->size = size;
            if(h->cursor >= size) h->cursor = size ? size - 1 : 0;
            if(h->anchor >= size) h->anchor = h->cursor;
        }
        for(i = 0; i < len; i++) {
            u8 c = h->piece[i];
            while(q && h->pattern[q] != c) q = h->fail[q - 1];
            if(h->pattern[q] == c) q++;
            if(q < h->plen) continue;
            stop_search(h);                             /* found: the match selected, in view */
            h->found = true;
            h->found_at = h->scan + i + 1 - h->plen;
            h->anchor = h->found_at;
            h->cursor = h->found_at + h->plen - 1;
            snprintf(h->status, sizeof(h->status), "Found at 0x%08X", h->found_at);
            show_cursor(g);
            return;
        }
        h->matched = q;
        h->scan += len;
        budget = len < budget ? budget - len : 0;
        if(!len || h->scan >= size) {
            stop_search(h);
            snprintf(h->status, sizeof(h->status), "Not found from 0x%08X to the end", h->from);
            g->dirty = true;
            return;
        }
    }
    if(h->searching) snprintf(h->status, sizeof(h->status), "Searching\xF9 0x%08X (%u%%), Esc stops", h->scan,
                              percent(h->scan, h->size));
    g->dirty = true;
}

static void search(struct gui_state *g, u32 from) {
    struct gui_hex *h = &g->hex;
    h->searching = true;
    h->from = h->scan = from;
    h->matched = 0;
    search_some(g);                                     /* the first piece at once: a small file is done */
}

static u32 sel_start(const struct gui_hex *h) { return h->anchor < h->cursor ? h->anchor : h->cursor; }
static u32 sel_end(const struct gui_hex *h) { return h->anchor < h->cursor ? h->cursor : h->anchor; }

/* Find next: after the match that is selected (also when the last search found nothing more), else from the
   selection's start */
static void find_next(struct gui_state *g) {
    struct gui_hex *h = &g->hex;
    if(!h->plen || !h->path[0]) return;
    search(g, h->found && sel_start(h) == h->found_at ? h->found_at + 1 : sel_start(h));
}

/**** the clipboard ****/

static void copy(struct gui_state *g) {
    struct gui_hex *h = &g->hex;
    static const char digits[] = "0123456789ABCDEF";
    u32 a = sel_start(h), n = sel_end(h) - a + 1, done = 0, len, size, i, o = 0;
    int r;
    if(!h->path[0] || !h->size) return;
    if(n > GUI_CLIP_MAX / 3) {                          /* "XX " a byte, but the last: 43,690 bytes in 128 KB, within a piece */
        snprintf(h->status, sizeof(h->status), "Too much to copy: %u bytes at most", (u32)(GUI_CLIP_MAX / 3));
        g->dirty = true;
        return;
    }
    while(done < n) {                                   /* all of it first (it fits a piece): a failure changes nothing */
        len = size = 0;
        r = g->fs && g->fs->read_at ? g->fs->read_at(g->fs->ctx, h->path, a + done, (char *)h->piece + done, n - done,
                                                      &len, &size) : -ENOSYS;
        if(!r && !len) r = -EIO;                        /* the file is shorter now */
        if(r) {
            snprintf(h->status, sizeof(h->status), "Not copied: %s", gui_error(g, r));
            g->dirty = true;
            return;
        }
        done += len;
    }
    for(i = 0; i < n; i++) {
        if(o) g->clip[o++] = ' ';
        g->clip[o++] = digits[h->piece[i] >> 4];
        g->clip[o++] = digits[h->piece[i] & 15];
    }
    g->clip_len = o;
    snprintf(h->status, sizeof(h->status), "Copied %u byte%s", n, n == 1 ? "" : "s");
    g->dirty = true;
}

static void copy_offset(struct gui_state *g) {
    struct gui_hex *h = &g->hex;
    snprintf(g->clip, GUI_CLIP_MAX, "0x%08X", h->cursor);
    g->clip_len = strlen(g->clip);
    snprintf(h->status, sizeof(h->status), "Copied the offset 0x%08X", h->cursor);
    g->dirty = true;
}

/**** the Edit menu and the toolbar ****/

static bool can_edit(const struct gui_state *g, int a) {
    const struct gui_hex *h = &g->hex;
    bool file = h->path[0] != 0;
    switch(a) {
    case ACT_COPY: case ACT_SELECT_ALL: return file && h->size > 0;
    case ACT_FIND: case ACT_GOTO: return file;
    case ACT_FIND_NEXT: return file && h->plen > 0;
    }
    return false;
}

static void edit(struct gui_state *g, int a) {
    struct gui_hex *h = &g->hex;
    char line[24];
    switch(a) {
    case ACT_COPY: copy(g); break;
    case ACT_SELECT_ALL:
        h->anchor = 0;
        h->cursor = h->size ? h->size - 1 : 0;
        g->dirty = true;
        break;
    case ACT_FIND:
        dialog_input(g, gui_app_act(GUI_HEX, HA_FIND), "Find bytes", "Text, or hexadecimal bytes such as DE AD BE EF:",
                     h->find);
        break;
    case ACT_FIND_NEXT: find_next(g); break;
    case ACT_GOTO:
        snprintf(line, sizeof(line), "0x%08X", h->cursor);
        dialog_input(g, gui_app_act(GUI_HEX, HA_GOTO), "Go to offset", "", line);
        snprintf(g->dialog.text[0], sizeof(g->dialog.text[0]), "An offset, 0x0 to 0x%08X (or in decimal):",
                 h->size ? h->size - 1 : 0);
        break;
    }
}

static bool tool_on(const struct gui_state *g, int a) {
    switch(a) {
    case HA_OPEN: return g->fs && g->fs->read_at;
    case HA_GOTO: return can_edit(g, ACT_GOTO);
    case HA_FIND: return can_edit(g, ACT_FIND);
    case HA_NEXT: return can_edit(g, ACT_FIND_NEXT);
    case HA_COPY: case HA_OFFSET: return can_edit(g, ACT_COPY);
    }
    return false;
}

/* where the chooser opens: the file's folder, else the last chooser's */
static void chooser_dir(const struct gui_state *g, char *out, size_t n) {
    if(g->hex.path[0]) { strlcpy(out, g->hex.path, n); gui_parent(out); }
    else strlcpy(out, g->dialog.dir[0] ? g->dialog.dir : "/", n);
}

static void act(struct gui_state *g, int a) {
    char dir[GUI_PATH_MAX];
    if(!tool_on(g, a)) return;
    switch(a) {
    case HA_OPEN:
        chooser_dir(g, dir, sizeof(dir));
        dialog_chooser(g, GUI_DLG_OPEN, gui_app_act(GUI_HEX, HA_OPEN), dir, "");
        break;
    case HA_GOTO: edit(g, ACT_GOTO); break;
    case HA_FIND: edit(g, ACT_FIND); break;
    case HA_NEXT: edit(g, ACT_FIND_NEXT); break;
    case HA_COPY: edit(g, ACT_COPY); break;
    case HA_OFFSET: copy_offset(g); break;
    }
}

/* an offset typed: 0x and hexadecimal, or decimal; false when it is not one */
static bool offset_of(const char *s, u32 *out) {
    u64 v = 0;
    int k = 0, d;
    while(*s == ' ') s++;
    if(s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for(s += 2; (d = digit(*s)) >= 0; s++, k++) { v = v << 4 | (u32)d; if(v > 0xFFFFFFFFu) return false; }
    } else {
        for(; *s >= '0' && *s <= '9'; s++, k++) { v = v * 10 + (u32)(*s - '0'); if(v > 0xFFFFFFFFu) return false; }
    }
    while(*s == ' ') s++;
    *out = (u32)v;
    return k > 0 && !*s;
}

static void answer(struct gui_state *g, int a, int button, const char *text, const char *target) {
    struct gui_hex *h = &g->hex;
    u32 off;
    int n;
    (void)target;
    if(button != 0) return;
    switch(a) {
    case HA_OPEN: hex_show(g, text); break;
    case HA_GOTO:
        if(!h->path[0]) break;
        if(!offset_of(text, &off)) snprintf(h->status, sizeof(h->status), "Not an offset: %s", text);
        else if(off >= h->size) snprintf(h->status, sizeof(h->status), "0x%08X is past the end (%u bytes)", off, h->size);
        else {
            h->cursor = h->anchor = off;
            h->top = off / 16 * 16 > top_max(g) ? top_max(g) : off / 16 * 16;   /* its row at the top */
            snprintf(h->status, sizeof(h->status), "At 0x%08X", off);
            show_cursor(g);
        }
        g->dirty = true;
        break;
    case HA_FIND:
        if(!h->path[0]) break;
        strlcpy(h->find, text, sizeof(h->find));
        n = pattern(text, h->pattern, GUI_HEX_PATTERN);
        stop_search(h);                                 /* a new Find: the last one's search and match go */
        h->found = false;
        if(n <= 0) {
            if(n == -2) snprintf(h->status, sizeof(h->status), "Find takes %u bytes at most", GUI_HEX_PATTERN);
            else if(n < 0) strlcpy(h->status, "Hexadecimal bytes are pairs of digits", sizeof(h->status));
            else h->status[0] = 0;
            h->plen = 0;
            g->dirty = true;
            break;
        }
        h->plen = (u32)n;
        kmp_table(h);
        search(g, sel_start(h));
        break;
    }
}

/**** the window ****/

static void opened(struct gui_state *g) {
    struct gui_hex *h = &g->hex;
    stop_search(h);
    h->path[0] = h->status[0] = 0;
    h->size = h->top = h->cursor = h->anchor = 0;
    h->page_ok = h->page_bad = h->selecting = h->thumb = h->found = false;
}

static void closed(struct gui_state *g) {
    opened(g);                                          /* the file goes with the window, as Notepad's text does */
}

static void title_of(const struct gui_state *g, char *out, size_t n) {
    if(g->hex.path[0]) snprintf(out, n, "%s - Hex", gui_basename(g->hex.path));
    else strlcpy(out, "Hex", n);
}

static bool key(struct gui_state *g, int k) {
    struct gui_hex *h = &g->hex;
    struct hex_layout l;
    if(k == 27 && h->searching) {
        stop_search(h);
        snprintf(h->status, sizeof(h->status), "Find stopped at 0x%08X", h->scan);
        g->dirty = true;
        return true;
    }
    if(k == 'o' || k == 'O') { act(g, HA_OPEN); return true; }
    if(!h->path[0]) return false;
    layout(g, &l);
    switch(k) {
    case GUI_LEFT: move(g, -1, false); return true;
    case GUI_RIGHT: move(g, 1, false); return true;
    case GUI_UP: move(g, -16, false); return true;
    case GUI_DOWN: move(g, 16, false); return true;
    case GUI_PGUP: move(g, -(i64)l.rows * 16, true); return true;
    case GUI_PGDN: move(g, (i64)l.rows * 16, true); return true;
    case GUI_HOME: move(g, -(i64)h->cursor, false); return true;
    case GUI_END: move(g, (i64)h->size - h->cursor, false); return true;
    case 'n': case 'N': find_next(g); return true;
    }
    return false;
}

/* the byte under x, y (window-relative), in either column: false when there is none there; near: the nearest
   byte's of the row (a drag past the columns) */
static bool byte_at(const struct gui_state *g, const struct hex_layout *l, int x, int y, bool near, u32 *off) {
    const struct gui_hex *h = &g->hex;
    int row = y >= l->list_y ? (y - l->list_y) / 16 : -1, i = -1, k;
    u64 at;
    if(!near && (row < 0 || row >= l->rows)) return false;
    row = gui_clamp(row, 0, l->rows - 1);
    if(x >= l->chr_x - 8) i = (x - l->chr_x) / 8;       /* the characters */
    else for(k = 0; k < 16; k++) if(x < byte_x(l, k) + 16 + (l->pitch - 16) / 2 + (k == 7 ? 4 : 0)) { i = k; break; }
    if(i < 0 || i > 15) {
        if(!near) return false;
        i = gui_clamp(i, 0, 15);
    }
    at = (u64)h->top + (u32)row * 16 + (u32)i;
    if(at >= h->size) {
        if(!near || !h->size) return false;
        at = h->size - 1;
    }
    *off = (u32)at;
    return true;
}

static void press(struct gui_state *g, int x, int y) {
    struct gui_hex *h = &g->hex;
    struct hex_layout l;
    int i = tool_hit(tools, TOOLS, x, y), top, size, k, bar_h;
    u32 off, rows;
    if(i >= 0) { act(g, tools[i].act); return; }
    if(!h->path[0]) return;
    layout(g, &l);
    bar_h = l.status_y - l.list_y;
    if(x >= l.bar_x && y >= l.list_y && y < l.status_y) {   /* the scroll bar: hold the thumb, or page */
        rows = rows_of(h->size);
        k = bar_shift(rows);
        gd_thumb(bar_h, (int)((h->top / 16) >> k), (l.rows >> k) ? l.rows >> k : 1, (int)(rows >> k), &top, &size);
        if(y - l.list_y < top) scroll_to(g, h->top > (u32)l.rows * 16 ? h->top - (u32)l.rows * 16 : 0);
        else if(y - l.list_y >= top + size) scroll_to(g, h->top + (u32)l.rows * 16 < h->top ? top_max(g) : h->top + (u32)l.rows * 16);
        else { h->thumb = true; h->thumb_at = y - l.list_y - top; }
        return;
    }
    if(!byte_at(g, &l, x, y, false, &off)) return;
    h->cursor = h->anchor = off;
    h->selecting = true;
    h->status[0] = 0;
    g->dirty = true;
}

static void drag(struct gui_state *g, int x, int y) {
    struct gui_hex *h = &g->hex;
    struct hex_layout l;
    u32 off, rows;
    u64 to;
    int k, shown, first;
    layout(g, &l);
    if(h->thumb) {                                      /* the thumb's last place is the file's end, not a row near it */
        rows = rows_of(h->size);
        k = bar_shift(rows);
        shown = (l.rows >> k) ? l.rows >> k : 1;
        first = gd_scroll_to(l.status_y - l.list_y, y - l.list_y - h->thumb_at, shown, (int)(rows >> k));
        to = (u64)(u32)first << k << 4;
        scroll_to(g, first >= (int)(rows >> k) - shown || to > top_max(g) ? top_max(g) : (u32)to);
        return;
    }
    if(!h->selecting) return;
    if(y < l.list_y && h->top) scroll_to(g, h->top - 16);   /* past the edge: the view scrolls */
    if(y >= l.list_y + l.rows * 16 && h->top < top_max(g)) scroll_to(g, h->top + 16);
    if(byte_at(g, &l, x, y, true, &off) && off != h->cursor) { h->cursor = off; g->dirty = true; }
}

static void release(struct gui_state *g) {
    g->hex.selecting = g->hex.thumb = false;
}

static void wheel(struct gui_state *g, int rows) {
    struct gui_hex *h = &g->hex;
    i64 top = (i64)h->top + (i64)rows * 16;
    if(!h->path[0]) return;
    scroll_to(g, (u32)(top < 0 ? 0 : top > (i64)top_max(g) ? (i64)top_max(g) : top));
}

static void tick(struct gui_state *g) {
    if(g->hex.searching) search_some(g);
    fill(g);                                            /* the window may have grown: more rows to read */
}

/**** drawing ****/

static void hex2(const struct gui_draw *d, int x, int y, u8 b, u32 c) {
    static const char digits[] = "0123456789ABCDEF";
    gd_glyph(d, x, y, (u8)digits[b >> 4], c, 1);
    gd_glyph(d, x + 8, y, (u8)digits[b & 15], c, 1);
}

static void render(const struct gui_state *g, const struct gui_draw *d) {
    const struct gui_window *w = &g->win[GUI_HEX];
    const struct gui_hex *h = &g->hex;
    struct hex_layout l;
    bool on[TOOLS], focused = g->focus == GUI_HEX && !g->dialog.kind && !g->menu, cur;
    char b[GUI_STATUS + 48], right[24];
    int x0 = w->x, y0 = w->y, row, i, x, py, k, size_px;
    u32 a = sel_start(h), z = sel_end(h), off, rows, at, ink;
    u8 c;
    layout(g, &l);
    for(i = 0; i < TOOLS; i++) on[i] = tool_on(g, tools[i].act);
    gd_rect(d, x0 + 1, y0 + TITLE_H, w->w - 2, BAR_H + 2, BAND);
    tool_render(d, x0, y0, tools, TOOLS, on);
    gd_rect(d, x0 + 1, y0 + TITLE_H + BAR_H + 2, w->w - 2, 1, LINE);
    gd_rect(d, x0 + 1, y0 + l.head_y, w->w - 2, 18, BAND);         /* the columns' numbers */
    gd_text(d, x0 + OFF_X, y0 + l.head_y + 1, "Offset", MUTED);
    for(i = 0; i < 16; i++) {
        hex2(d, x0 + byte_x(&l, i), y0 + l.head_y + 1, (u8)i, MUTED);
        gd_glyph(d, x0 + l.chr_x + i * 8, y0 + l.head_y + 1, (u8)"0123456789ABCDEF"[i], MUTED, 1);
    }
    gd_rect(d, x0 + 1, y0 + l.list_y - 1, l.bar_x - 1, 1, LINE);
    gd_rect(d, x0 + 1, y0 + l.list_y, l.bar_x - 1, l.status_y - l.list_y, 0xFFFDF5);
    if(!h->path[0])
        gd_name(d, x0 + OFF_X, y0 + l.list_y + 8, g->fs && g->fs->read_at ? "Open a file: the toolbar's Open, or O." :
                "There are no files here to open.", (l.bar_x - 2 * OFF_X) / 8, MUTED);
    for(row = 0; h->path[0] && h->page_ok && row < l.rows; row++) {
        off = h->top + (u32)row * 16;
        if((u32)row * 16 >= h->size - h->top || h->top >= h->size) break;   /* the file's end (no wrap past 4 GB) */
        py = y0 + l.list_y + row * 16;
        snprintf(b, sizeof(b), "%08X", off);
        gd_text(d, x0 + OFF_X, py, b, h->cursor / 16 * 16 == off ? TEAL : MUTED);
        for(i = 0; i < 16 && (u32)i < h->size - off; i++) {
            at = off + (u32)i;
            if(at < h->page_off || at - h->page_off >= h->page_len) break;  /* not read (yet) */
            c = h->page[at - h->page_off];
            cur = at == h->cursor && focused;           /* the cursor, in the window in front */
            ink = cur ? PAPER : INK;
            x = x0 + byte_x(&l, i);
            if(at >= a && at <= z) {
                gd_rect(d, x - (l.pitch - 16) / 2, py, l.pitch, 16, cur ? TEAL : PICK);
                gd_rect(d, x0 + l.chr_x + i * 8, py, 8, 16, cur ? TEAL : PICK);
            }
            hex2(d, x, py, c, ink);
            if(c >= 32 && c < 127) gd_glyph(d, x0 + l.chr_x + i * 8, py, c, ink, 1);
            else gd_glyph(d, x0 + l.chr_x + i * 8, py, '.', cur ? PAPER : MUTED, 1);
        }
    }
    rows = rows_of(h->size);
    k = bar_shift(rows);
    size_px = l.status_y - l.list_y;
    gd_scrollbar(d, x0 + l.bar_x, y0 + l.list_y, size_px, (int)((h->top / 16) >> k), (l.rows >> k) ? l.rows >> k : 1,
                 (int)(rows >> k));
    gd_rect(d, x0 + 1, y0 + l.status_y, w->w - 2, STATUS_H, BAND);
    gd_rect(d, x0 + 1, y0 + l.status_y, w->w - 2, 1, LINE);
    right[0] = 0;
    if(!h->path[0]) strlcpy(b, h->status[0] ? h->status : "No file", sizeof(b));
    else {
        snprintf(right, sizeof(right), "%u bytes", h->size);
        if(z > a) snprintf(b, sizeof(b), "0x%08X (%u), %u selected  %s", h->cursor, h->cursor, z - a + 1, h->status);
        else snprintf(b, sizeof(b), "0x%08X (%u)  %s", h->cursor, h->cursor, h->status);
    }
    x = w->w - 24 - (int)strlen(right) * 8;             /* clear of the resize grip */
    gd_name(d, x0 + 10, y0 + l.status_y + 3, b, (x - 18) / 8, INK);
    gd_text(d, x0 + x, y0 + l.status_y + 3, right, MUTED);
}

/* 10/18/26: the arrows move the cursor in a file; with none they move the pointer */
static bool wants_arrows(const struct gui_state *g) { return g->hex.path[0] != 0; }

const struct gui_app gui_app_hex = {
    .title = "Hex", .icon = ICON_HEX, .shortcut = "", .w = 620, .h = 380,
    .resizable = true, .arrows = true, .keys = false,
    .min_width = min_width, .opened = opened, .closed = closed,
    .render = render, .key = key, .press = press, .drag = drag, .release = release, .wheel = wheel, .tick = tick,
    .title_of = title_of, .answer = answer,
    .help = help, .help_lines = (int)(sizeof(help) / sizeof(help[0])),
    .can_edit = can_edit, .edit = edit, .goto_label = "Go to offset\xF9",
    .wants_arrows = wants_arrows,
};
