/*
SurfOS Desktop Notepad
--------------------
File: guinote.c Date: 10/17/26
--------------------
One document in memory, at most GUI_NOTE_MAX bytes, edited as bytes. The cursor
and the selection always sit at the start of a character: a UTF-8 sequence by the
console's rules, or one byte (gd_char_len()). A character is drawn as its code
page 437 glyph ('?' when 437 has none, a byte that is not UTF-8 as the 437
character it is, as on the console), a tab reaches the next multiple of 8
columns, and lines do not wrap: the view scrolls sideways to the cursor. Every
change goes through replace(), which keeps the undo steps (typing in a row is one
step; a change too big to keep clears the history, never half of it). A file whose
every line ends in CRLF is edited with LF and saved with CRLF again, counted with
its CRs against the limit, so whatever Notepad saves it can open again; any other
file keeps its bytes as they are. A file with a NUL byte is binary and is not opened.
*/
#include "guipriv.h"
#include <string.h>
#include <stdio.h>

static const struct gui_tool tools[] = {
    { "New", ACT_NEW }, { "Open", ACT_OPEN }, { "Save", ACT_SAVE }, { "Save as", ACT_SAVE_AS }, { "Find", ACT_FIND },
    { "Undo", ACT_UNDO }, { "Copy", ACT_COPY }, { "Paste", ACT_PASTE },
};
#define TOOLS (int)(sizeof(tools) / sizeof(tools[0]))

/**** characters and lines ****/

static bool cont(u8 c) { return (c & 0xC0) == 0x80; }

static u32 char_len(const struct gui_note *n, u32 off) {
    return off < n->len ? gd_char_len(n->text + off, n->len - off) : 0;
}

/* off, or the end of the character it falls inside: an edit can join bytes into one character */
static u32 settle(const struct gui_note *n, u32 off) {
    u32 k, start, len;
    for(k = 1; k <= 3 && k <= off; k++) {
        start = off - k;
        if(cont((u8)n->text[start])) continue;
        len = char_len(n, start);
        return start + len > off ? start + len : off;
    }
    return off;
}

static u32 count_lf(const char *s, u32 len) {
    u32 i, lf = 0;
    for(i = 0; i < len; i++) if(s[i] == '\n') lf++;
    return lf;
}

static u32 next_char(const struct gui_note *n, u32 off) {
    return off < n->len ? off + char_len(n, off) : n->len;
}

static u32 prev_char(const struct gui_note *n, u32 off) {
    u32 k;
    for(k = 1; k <= 4 && k <= off; k++) {
        if(cont((u8)n->text[off - k])) continue;
        if(off - k + char_len(n, off - k) == off) return off - k;
        break;
    }
    return off ? off - 1 : 0;
}

static u8 glyph_at(const struct gui_note *n, u32 off) {
    u32 len;
    return gd_char(n->text + off, n->len - off, &len);
}

/* the columns the character at off takes when it starts at column col */
static int cells_at(const struct gui_note *n, u32 off, int col) {
    return n->text[off] == '\t' ? (col / TAB_W + 1) * TAB_W - col : 1;
}

static u32 line_begin(const struct gui_note *n, u32 off) {
    while(off > 0 && n->text[off - 1] != '\n') off--;
    return off;
}

static u32 line_finish(const struct gui_note *n, u32 off) {
    while(off < n->len && n->text[off] != '\n') off++;
    return off;
}

int note_line_of(const struct gui_note *n, u32 off) {
    int line = 0;
    u32 i;
    for(i = 0; i < off && i < n->len; i++) if(n->text[i] == '\n') line++;
    return line;
}

int note_lines(const struct gui_note *n) {
    return n->lf + 1;
}

u32 note_line_start(const struct gui_note *n, int line) {
    u32 i;
    for(i = 0; line > 0 && i < n->len; i++) if(n->text[i] == '\n') line--;
    return i;
}

int note_column(const struct gui_note *n, u32 off) {
    u32 i = line_begin(n, off);
    int col = 0;
    while(i < off) { col += cells_at(n, i, col); i = next_char(n, i); }
    return col;
}

/* the place in the line that starts at start for column col: the character that covers it, or
   with round the boundary nearest to it (a click in a tab's right half goes after the tab) */
static u32 at_column(const struct gui_note *n, u32 start, int col, bool round) {
    u32 i = start;
    int c = 0, w;
    while(i < n->len && n->text[i] != '\n') {
        w = cells_at(n, i, c);
        if(c + w > col) {
            if(round && (col - c) * 2 > w) i = next_char(n, i);
            break;
        }
        c += w;
        i = next_char(n, i);
    }
    return i;
}

static void selection(const struct gui_note *n, u32 *a, u32 *b) {
    *a = n->anchor < n->cursor ? n->anchor : n->cursor;
    *b = n->anchor < n->cursor ? n->cursor : n->anchor;
}

bool note_has_selection(const struct gui_note *n) {
    return n->anchor != n->cursor;
}

void note_status(struct gui_state *g, const char *msg) {
    strlcpy(g->note.status, msg, sizeof(g->note.status));
    g->dirty = true;
}

int note_min_width(void) {
    return tool_span(tools, TOOLS);
}

void note_title(const struct gui_state *g, char *out, size_t n) {
    const struct gui_note *note = &g->note;
    snprintf(out, n, "%s%s - Notepad", note->modified ? "*" : "", note->path[0] ? gui_basename(note->path) : "Untitled");
}

/**** layout ****/

void note_layout(const struct gui_state *g, struct note_layout *l) {
    const struct gui_window *w = &g->win[GUI_NOTEPAD];
    int lines = note_lines(&g->note), digits = 1, k;
    for(k = lines; k >= 10; k /= 10) digits++;
    if(digits < 3) digits = 3;
    l->gutter = digits * 8 + 12;
    l->ty = TITLE_H + BAR_H + 4;
    l->status_y = w->h - STATUS_H - 1;
    l->find_y = g->note.finding ? l->status_y - 28 : l->status_y;
    l->rows = (l->find_y - l->ty - 2) / 16;
    if(l->rows < 1) l->rows = 1;
    l->tx = 2 + l->gutter + 6;
    l->bar_x = w->w - 2 - SCROLL_W;
    l->cols = (l->bar_x - l->tx - 2) / 8;
    if(l->cols < 1) l->cols = 1;
}

/* the last useful first line: the view ends with the text */
static int top_max(const struct gui_state *g) {
    struct note_layout l;
    int lines = note_lines(&g->note);
    note_layout(g, &l);
    return lines > l.rows ? lines - l.rows : 0;
}

/* scroll so that the cursor is in view */
static void show_cursor(struct gui_state *g) {
    struct gui_note *n = &g->note;
    struct note_layout l;
    int line = note_line_of(n, n->cursor), col = note_column(n, n->cursor);
    note_layout(g, &l);
    if(line < n->top) n->top = line;
    if(line >= n->top + l.rows) n->top = line - l.rows + 1;
    if(col < n->left) n->left = col > 8 ? col - 8 : 0;
    if(col >= n->left + l.cols) n->left = col - l.cols + 8;
    if(n->left < 0) n->left = 0;
    g->dirty = true;
}

/**** undo ****/

/* the oldest group of steps goes, to make room */
static void undo_drop_oldest(struct gui_note *n) {
    int k = 1, i;
    u32 bytes;
    while(k < n->nundo && n->undo[k].group == n->undo[0].group) k++;
    bytes = k < n->nundo ? n->undo[k].text : n->undo_used;
    memmove(n->undo_text, n->undo_text + bytes, n->undo_used - bytes);
    n->undo_used -= bytes;
    memmove(n->undo, n->undo + k, (n->nundo - k) * sizeof(n->undo[0]));
    n->nundo -= k;
    n->undo_at -= k;
    for(i = 0; i < n->nundo; i++) n->undo[i].text -= bytes;
    n->saved_at = n->saved_at >= k ? n->saved_at - k : -1;
}

/* before a change of bytes in all (its deletion and its insertion): what was undone goes, then the
   oldest groups until both of its steps fit. false: the change alone is too big to keep, and then
   nothing before it can be undone either */
static bool undo_room(struct gui_note *n, u32 bytes) {
    if(n->undo_at < n->nundo) {                         /* a change after undoing: what was undone goes */
        n->undo_used = n->undo_at ? n->undo[n->undo_at - 1].text + n->undo[n->undo_at - 1].len : 0;
        if(n->saved_at > n->undo_at) n->saved_at = -1;
        n->nundo = n->undo_at;
    }
    if(bytes > GUI_UNDO_TEXT) {
        n->nundo = n->undo_at = 0;
        n->undo_used = 0;
        n->saved_at = -1;
        return false;
    }
    while(n->nundo && (n->nundo + 2 > GUI_UNDO_STEPS || n->undo_used + bytes > GUI_UNDO_TEXT)) undo_drop_oldest(n);
    return true;
}

/* a change of len bytes at pos, inserted or deleted (s: the bytes), as an undo step; join: a key typed
   after another, which makes the last step grow, unless that step is where the file was saved */
static void undo_record(struct gui_note *n, bool insert, u32 pos, const char *s, u32 len, bool join) {
    struct gui_undo *u = n->nundo ? &n->undo[n->nundo - 1] : NULL;
    if(!len) return;
    if(join && u && insert && u->insert && u->group == n->group && u->pos + u->len == pos && n->undo_at != n->saved_at) {
        memcpy(n->undo_text + n->undo_used, s, len);
        n->undo_used += len;
        u->len += len;
        return;
    }
    u = &n->undo[n->nundo++];
    u->pos = pos;
    u->len = len;
    u->text = n->undo_used;
    u->insert = insert;
    u->group = n->group;
    memcpy(n->undo_text + n->undo_used, s, len);
    n->undo_used += len;
    n->undo_at = n->nundo;
}

/* the bytes from..to become s (len of them), with no undo step */
static bool splice(struct gui_note *n, u32 from, u32 to, const char *s, u32 len) {
    if(n->len - (to - from) + len > GUI_NOTE_MAX) return false;
    n->lf = n->lf - count_lf(n->text + from, to - from) + count_lf(s, len);
    memmove(n->text + from + len, n->text + to, n->len - to);
    if(len) memcpy(n->text + from, s, len);
    n->len = n->len - (to - from) + len;
    return true;
}

/* every change to the text: the bytes from..to become s; typing: a key typed, which joins the
   step of the keys typed before it */
static bool replace(struct gui_state *g, u32 from, u32 to, const char *s, u32 len, bool typing) {
    struct gui_note *n = &g->note;
    bool join = typing && n->typing && from == to;
    u32 size = n->len - (to - from) + len;
    if(n->crlf) size += n->lf - count_lf(n->text + from, to - from) + count_lf(s, len);   /* the CRs it is saved with */
    if(size > GUI_NOTE_MAX) {
        note_status(g, "Notepad holds 256 KB at most");
        return false;
    }
    if(!join) n->group++;
    n->typing = typing;
    if(undo_room(n, (to - from) + len)) {
        undo_record(n, false, from, n->text + from, to - from, false);
        undo_record(n, true, from, s, len, join);
    }
    splice(n, from, to, s, len);
    n->cursor = n->anchor = settle(n, from + len);
    n->goal = -1;
    n->modified = n->saved_at != n->undo_at;
    g->dirty = true;
    return true;
}

void note_undo(struct gui_state *g, bool redo) {
    struct gui_note *n = &g->note;
    struct gui_undo *u;
    u32 group;
    if(redo ? n->undo_at >= n->nundo : n->undo_at <= 0) {
        note_status(g, redo ? "Nothing to redo" : "Nothing to undo");
        return;
    }
    group = n->undo[redo ? n->undo_at : n->undo_at - 1].group;
    while(redo ? n->undo_at < n->nundo && n->undo[n->undo_at].group == group :
                 n->undo_at > 0 && n->undo[n->undo_at - 1].group == group) {
        u = &n->undo[redo ? n->undo_at++ : --n->undo_at];
        if(u->insert != redo) {                         /* the bytes come out again */
            splice(n, u->pos, u->pos + u->len, NULL, 0);
            n->cursor = n->anchor = u->pos;
        } else {                                        /* and back in, selected */
            splice(n, u->pos, u->pos, n->undo_text + u->text, u->len);
            n->anchor = u->pos;
            n->cursor = u->pos + u->len;
        }
    }
    n->cursor = settle(n, n->cursor);
    n->anchor = settle(n, n->anchor);
    n->typing = false;
    n->goal = -1;
    n->group++;
    n->modified = n->saved_at != n->undo_at;
    n->status[0] = 0;
    show_cursor(g);
}

/**** the document ****/

void note_reset(struct gui_note *n) {
    n->path[0] = 0;
    n->len = n->lf = n->cursor = n->anchor = 0;
    n->top = n->left = 0;
    n->goal = -1;
    n->modified = n->crlf = n->readonly = n->selecting = n->finding = n->typing = false;
    n->status[0] = 0;
    n->nundo = n->undo_at = n->saved_at = 0;
    n->undo_used = 0;
    n->group++;
}

/* a file and a new file beside it that may share their clusters (gui_unsure()); past GUI_UNSURE pairs
   none is kept, and the desktop changes no file at all */
static void remember(struct gui_state *g, const char *file, const char *kept) {
    int i;
    for(i = 0; i < g->nunsure && i < GUI_UNSURE; i++) if(!strcmp(g->unsure[i], file) && !strcmp(g->kept[i], kept)) return;
    if(g->nunsure < GUI_UNSURE) {
        strlcpy(g->unsure[g->nunsure], file, GUI_PATH_MAX);
        strlcpy(g->kept[g->nunsure], kept, GUI_PATH_MAX);
    }
    g->nunsure++;
}

/* a name beside path with its size and time, into out: fs/fat.c's rename copies the entry, so after one
   that may have half happened the two hold the same clusters (or the user made such a copy: then the care
   costs nothing). Beside a.txt: a.txt.tmp and a.txt.1.tmp to .9.tmp; beside one of those, a.txt. */
static bool twin_of(struct gui_state *g, const char *path, const struct gui_entry *f, char *out, size_t n) {
    struct gui_entry e;
    bool device, readonly;
    size_t len = strlen(path);
    int k;
    for(k = 0; k <= 10; k++) {
        if(k < 10) {
            if(snprintf(out, n, k ? "%s.%d.tmp" : "%s.tmp", path, k) >= (int)n) continue;
        } else {                                        /* path is <file>.tmp or <file>.N.tmp */
            if(len < 5 || strcmp(path + len - 4, ".tmp")) return false;
            strlcpy(out, path, n);
            out[len - 4] = 0;
            if(len >= 7 && out[len - 6] == '.' && out[len - 5] >= '1' && out[len - 5] <= '9') out[len - 6] = 0;
        }
        if(g->fs->stat(g->fs->ctx, out, &e, &device, &readonly) == 0 && e.type == GUI_FILE &&
           e.size == f->size && e.mtime == f->mtime) return true;
    }
    return false;
}

int note_load(struct gui_state *g, const char *path) {
    struct gui_note *n = &g->note;
    struct gui_entry e;
    bool device = false, readonly = false;
    char line[GUI_STATUS], other[GUI_PATH_MAX], a[GUI_PATH_MAX], b[GUI_PATH_MAX];
    const char *name = gui_basename(path);
    u32 len = 0, i, lf = 0, crlf = 0, j;
    int r;
    if(!g->fs) return -ENOSYS;
    r = g->fs->stat(g->fs->ctx, path, &e, &device, &readonly);
    if(!r && e.type == GUI_DIR) r = -EISDIR;
    if(r) {
        snprintf(line, sizeof(line), "%s: %s", name, gui_error(g, r));
        dialog_message(g, "Cannot open the file", line, "");
        return r;
    }
    if(device) {
        snprintf(line, sizeof(line), "%s is a device, not a file.", name);
        dialog_message(g, "Cannot open the file", line, "Notepad opens text files.");
        return -EINVAL;
    }
    if(e.size > GUI_NOTE_MAX) {
        snprintf(line, sizeof(line), "%s has %u KB.", name, e.size / 1024);
        dialog_message(g, "Too large for Notepad", line, "Notepad opens files of up to 256 KB.");
        return -EFBIG;
    }
    r = g->fs->read(g->fs->ctx, path, n->text, GUI_NOTE_MAX, &len);
    if(!r && memchr(n->text, 0, len)) r = -EINVAL;
    if(r) {
        note_reset(n);                                  /* what was read is not kept */
        if(r == -EINVAL) {
            snprintf(line, sizeof(line), "%s has NUL bytes: it is not a text file.", name);
            dialog_message(g, "Cannot open the file", line, "Notepad opens text files.");
        } else if(r == -EFBIG) {
            snprintf(line, sizeof(line), "%s has more than 256 KB.", name);
            dialog_message(g, "Too large for Notepad", line, "");
        } else {
            snprintf(line, sizeof(line), "%s: %s", name, gui_error(g, r));
            dialog_message(g, "Cannot open the file", line, "");
        }
        return r;
    }
    note_reset(n);
    for(i = 0; i < len; i++) if(n->text[i] == '\n') { lf++; if(i && n->text[i - 1] == '\r') crlf++; }
    n->crlf = lf && crlf == lf;                         /* every line ends in CRLF: edited as LF */
    if(n->crlf) {
        for(i = j = 0; i < len; i++) if(!(n->text[i] == '\r' && i + 1 < len && n->text[i + 1] == '\n')) n->text[j++] = n->text[i];
        len = j;
    }
    n->len = len;
    n->lf = lf;
    strlcpy(n->path, path, sizeof(n->path));
    n->readonly = readonly;
    if(readonly) note_status(g, "Read-only volume: Save as keeps a copy elsewhere");
    else if(twin_of(g, path, &e, other, sizeof(other))) {   /* a save that failed half way, it may be */
        remember(g, path, other);
        gui_middle(name, a, sizeof(a), 28);
        gui_middle(gui_basename(other), b, sizeof(b), 28);
        snprintf(line, sizeof(line), "%s may share its data with %s: check the disk", a, b);
        note_status(g, line);
    } else {
        snprintf(line, sizeof(line), "Opened %s", name);
        note_status(g, line);
    }
    g->dirty = true;
    return 0;
}

/* a name in at most cells cells: a longer one loses its middle (… there), so that its end (.1.tmp) shows */
void gui_middle(const char *name, char *out, size_t n, int cells) {
    u32 len = strlen(name), at = 0, o;
    int total = gd_cells(name), k;
    if(total <= cells) { strlcpy(out, name, n); return; }
    for(k = 0; k < (cells - 1) / 2 && at < len; k++) at += gd_char_len(name + at, len - at);
    o = at < n - 4 ? at : n - 4;
    memcpy(out, name, o);
    out[o++] = (char)0xF9;
    for(k = 0; k < total - cells + 1 && at < len; k++) at += gd_char_len(name + at, len - at);
    out[o] = 0;
    strlcat(out, name + at, n);
}

int note_save(struct gui_state *g, const char *path) {
    struct gui_note *n = &g->note;
    char line[80], why[80], kept[GUI_PATH_MAX], name[GUI_PATH_MAX];
    u32 bytes = n->len + (n->crlf ? n->lf : 0);
    struct gui_entry e;
    bool device, readonly;
    int r;
    if(!g->fs) return -ENOSYS;
    /* never over a file that may share its clusters; past GUI_UNSURE such saves only to a new name */
    if(gui_unsure(g, path) && (g->nunsure <= GUI_UNSURE || g->fs->stat(g->fs->ctx, path, &e, &device, &readonly) == 0)) {
        gui_unsure_line(g, path, line, sizeof(line));
        dialog_message(g, "Not saved", line, "Have the disk checked first (fsck).");
        return -EBUSY;
    }
    r = g->fs->write(g->fs->ctx, path, n->text, n->len, n->crlf, kept, sizeof(kept));
    if(r) {
        snprintf(line, sizeof(line), "%s: %s", gui_basename(path), gui_error(g, r));
        if(kept[0]) {                                   /* its rename may have half happened */
            remember(g, path, kept);
            if(g->fs->stat(g->fs->ctx, kept, &e, &device, &readonly) == 0) {   /* the dialog's 54 cells: 31, the name */
                gui_middle(gui_basename(kept), name, sizeof(name), 22);
                snprintf(why, sizeof(why), "Check the disk: the text is in %s.", name);
            } else strlcpy(why, "Have the disk checked first (fsck).", sizeof(why));
        }
        else strlcpy(why, r == -EROFS ? "That volume is read-only: choose Save as." :
                          r == -ENOSPC ? "The volume is full." :
                          r == -EEXIST ? "Move the .tmp files beside it out of the way." : "", sizeof(why));
        dialog_message(g, "Not saved", line, why);
        return r;
    }
    if(path != n->path) strlcpy(n->path, path, sizeof(n->path));
    n->readonly = false;
    n->saved_at = n->undo_at;
    n->modified = false;
    n->typing = false;
    n->group++;                                         /* typing after a save is a new step */
    snprintf(line, sizeof(line), "Saved %s (%u bytes)", gui_basename(n->path), bytes);
    note_status(g, line);
    return 0;
}

/**** editing ****/

void note_insert(struct gui_state *g, const char *s, u32 len) {
    u32 a, b;
    selection(&g->note, &a, &b);
    if(replace(g, a, b, s, len, false)) show_cursor(g);
}

void note_copy(struct gui_state *g, bool cut) {
    struct gui_note *n = &g->note;
    u32 a, b;
    selection(n, &a, &b);
    if(a == b) { note_status(g, "Select some text first"); return; }
    if(b - a > GUI_CLIP_MAX) { note_status(g, "Too much to copy: 128 KB at most"); return; }
    memcpy(g->clip, n->text + a, b - a);
    g->clip_len = b - a;
    if(cut) { if(replace(g, a, b, "", 0, false)) show_cursor(g); }
    else note_status(g, "Copied");
}

void note_paste(struct gui_state *g) {
    if(!g->clip_len) { note_status(g, "Nothing to paste"); return; }
    note_insert(g, g->clip, g->clip_len);
}

void note_select_all(struct gui_state *g) {
    g->note.anchor = 0;
    g->note.cursor = g->note.len;
    g->note.typing = false;
    g->dirty = true;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* the next match after the selection (letters in either case), wrapping round to the top */
bool note_find(struct gui_state *g, const char *what) {
    struct gui_note *n = &g->note;
    u32 len = strlen(what), a, b, i, k, pass;
    char msg[GUI_STATUS];
    if(!len) return false;
    selection(n, &a, &b);
    for(pass = 0; pass < 2; pass++) {
        u32 from = pass ? 0 : b, limit = pass ? b : n->len;      /* then the matches that start before it */
        for(i = from; i < limit && i + len <= n->len; i++) {
            for(k = 0; k < len && lower(n->text[i + k]) == lower(what[k]); k++) ;
            if(k < len || settle(n, i) != i || settle(n, i + len) != i + len) continue;   /* whole characters only */
            n->anchor = i;
            n->cursor = i + len;
            n->typing = false;
            n->goal = -1;
            note_status(g, pass ? "Found it again from the top" : "");
            show_cursor(g);
            return true;
        }
    }
    snprintf(msg, sizeof(msg), "Not found: %s", what);
    note_status(g, msg);
    return false;
}

void note_goto(struct gui_state *g, int line) {
    struct gui_note *n = &g->note;
    int lines = note_lines(n);
    if(line < 1) line = 1;
    if(line > lines) line = lines;
    n->cursor = n->anchor = note_line_start(n, line - 1);
    n->goal = -1;
    n->typing = false;
    show_cursor(g);
}

/* Up, Down, Page Up and Page Down: to the same column of another line */
static void vertical(struct gui_state *g, int delta, bool page) {
    struct gui_note *n = &g->note;
    int line = note_line_of(n, n->cursor), lines = note_lines(n), col, target;
    col = n->goal >= 0 ? n->goal : note_column(n, n->cursor);
    target = gui_clamp(line + delta, 0, lines - 1);
    if(page) n->top = gui_clamp(n->top + delta, 0, top_max(g));
    if(target == line && page) n->cursor = delta < 0 ? 0 : n->len;
    else n->cursor = at_column(n, note_line_start(n, target), col, false);
    n->anchor = n->cursor;
    n->goal = col;
    n->typing = false;
}

void note_key(struct gui_state *g, int key) {
    struct gui_note *n = &g->note;
    struct note_layout l;
    u32 a, b, to, k;
    char c;
    if(n->finding) {
        if(key == 27) n->finding = false;
        else if(key == '\r' || key == '\n') note_find(g, n->find.text);
        else field_key(&n->find, key, g->clip, g->clip_len);
        g->dirty = true;
        return;
    }
    note_layout(g, &l);
    selection(n, &a, &b);
    n->status[0] = 0;
    switch(key) {
    case GUI_LEFT: to = a != b ? a : prev_char(n, n->cursor); break;
    case GUI_RIGHT: to = a != b ? b : next_char(n, n->cursor); break;
    case GUI_HOME:                                      /* the line's text, then its start */
        to = line_begin(n, n->cursor);
        for(k = to; k < n->len && (n->text[k] == ' ' || n->text[k] == '\t'); k++) ;
        if(n->cursor != k) to = k;
        break;
    case GUI_END: to = line_finish(n, n->cursor); break;
    case GUI_UP: case GUI_DOWN: vertical(g, key == GUI_UP ? -1 : 1, false); show_cursor(g); return;
    case GUI_PGUP: case GUI_PGDN: vertical(g, key == GUI_PGUP ? 1 - l.rows : l.rows - 1, true); show_cursor(g); return;
    case GUI_DELETE:
        if(a != b) replace(g, a, b, "", 0, false);
        else if(n->cursor < n->len) replace(g, n->cursor, next_char(n, n->cursor), "", 0, false);
        show_cursor(g);
        return;
    case 8: case 127:
        if(a != b) replace(g, a, b, "", 0, false);
        else if(n->cursor) replace(g, prev_char(n, n->cursor), n->cursor, "", 0, false);
        show_cursor(g);
        return;
    case '\r': case '\n':
        replace(g, a, b, "\n", 1, true);
        n->typing = false;                              /* each line is a step of its own */
        show_cursor(g);
        return;
    case 27:
        n->anchor = n->cursor;
        g->dirty = true;
        return;
    default:
        if(key != '\t' && (key < 32 || key > 255 || key == 127)) return;
        c = (char)key;
        replace(g, a, b, &c, 1, true);
        show_cursor(g);
        return;
    }
    n->cursor = n->anchor = to;
    n->goal = -1;
    n->typing = false;
    show_cursor(g);
}

/**** the pointer ****/

/* a toolbar button can be used now */
static bool tool_on(const struct gui_state *g, int act) {
    const struct gui_note *n = &g->note;
    switch(act) {
    case ACT_OPEN: case ACT_SAVE: case ACT_SAVE_AS: return g->fs != NULL;
    case ACT_UNDO: return n->undo_at > 0;
    case ACT_COPY: return n->anchor != n->cursor;
    case ACT_PASTE: return g->clip_len > 0;
    }
    return true;
}

static u32 offset_at(const struct gui_state *g, const struct note_layout *l, int x, int y) {
    const struct gui_note *n = &g->note;
    int row = y >= l->ty ? (y - l->ty) / 16 : -1;
    int line = gui_clamp(n->top + row, 0, note_lines(n) - 1);
    int col = n->left + (x >= l->tx ? (x - l->tx + 4) / 8 : 0);
    return at_column(n, note_line_start(n, line), col, true);
}

static bool word(u8 c) {
    return c == '_' || c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

void note_press(struct gui_state *g, int x, int y) {
    struct gui_note *n = &g->note;
    const struct gui_window *w = &g->win[GUI_NOTEPAD];
    struct note_layout l;
    u32 off, a, b;
    int i, top, size;
    note_layout(g, &l);
    i = tool_hit(tools, TOOLS, x, y);
    if(i >= 0) { if(tool_on(g, tools[i].act)) gui_act(g, tools[i].act); return; }
    if(n->finding && y >= l.find_y && y < l.status_y) {
        if(x >= w->w - 34) n->finding = false;
        else if(x >= w->w - 100) note_find(g, n->find.text);
        g->dirty = true;
        return;
    }
    if(y < l.ty - 2 || y >= l.find_y) return;
    if(x >= l.bar_x) {                                  /* the scroll bar: hold the thumb, or page */
        gd_thumb(l.find_y - l.ty + 2, n->top, l.rows, note_lines(n), &top, &size);
        if(y - (l.ty - 2) < top) n->top = gui_clamp(n->top - l.rows, 0, top_max(g));
        else if(y - (l.ty - 2) >= top + size) n->top = gui_clamp(n->top + l.rows, 0, top_max(g));
        else { g->scroll = GUI_NOTEPAD; g->scroll_at = y - (l.ty - 2) - top; }
        g->dirty = true;
        return;
    }
    n->finding = false;                                 /* a click in the text: the keys are the text's again */
    off = offset_at(g, &l, x, y);
    n->typing = false;
    n->goal = -1;
    if(x < l.tx - 4) {                                  /* the line numbers: the whole line */
        n->anchor = line_begin(n, off);
        n->cursor = line_finish(n, off);
        if(n->cursor < n->len) n->cursor++;
        g->dirty = true;
        return;
    }
    if(off == n->click_at && g->now - n->click_ms <= DOUBLE_MS) {   /* a double-click: the word */
        a = b = off;
        if(b < n->len && word((u8)n->text[b])) {
            while(a > 0 && word((u8)n->text[a - 1])) a--;
            while(b < n->len && word((u8)n->text[b])) b++;
        } else b = next_char(n, b);
        n->anchor = a;
        n->cursor = b;
        n->click_ms = g->now - DOUBLE_MS - 1;
        g->dirty = true;
        return;
    }
    n->click_at = off;
    n->click_ms = g->now;
    n->cursor = n->anchor = off;
    n->selecting = true;
    g->dirty = true;
}

void note_drag(struct gui_state *g, int x, int y) {
    struct gui_note *n = &g->note;
    struct note_layout l;
    int col;
    if(!n->selecting) return;
    note_layout(g, &l);
    if(y < l.ty && n->top > 0) n->top--;                /* past the edge: the text scrolls */
    if(y >= l.ty + l.rows * 16 && n->top < top_max(g)) n->top++;
    n->cursor = offset_at(g, &l, x, y);
    if(x < l.tx && n->left > 0) n->left--;              /* sideways too, as far as the line goes */
    if(x >= l.tx + l.cols * 8 && note_column(n, line_finish(n, n->cursor)) > n->left + l.cols) n->left++;
    n->cursor = offset_at(g, &l, x, y);
    col = note_column(n, n->cursor);
    if(col < n->left) n->left = col;
    if(col > n->left + l.cols) n->left = col - l.cols;
    g->dirty = true;
}

void note_scroll(struct gui_state *g, int y) {
    struct note_layout l;
    note_layout(g, &l);
    g->note.top = gd_scroll_to(l.find_y - l.ty + 2, y - (l.ty - 2) - g->scroll_at, l.rows, note_lines(&g->note));
    g->dirty = true;
}

void note_wheel(struct gui_state *g, int lines) {
    g->note.top = gui_clamp(g->note.top + lines, 0, top_max(g));
    g->dirty = true;
}

/**** drawing ****/

void note_render(const struct gui_state *g, const struct gui_draw *d) {
    const struct gui_window *w = &g->win[GUI_NOTEPAD];
    const struct gui_note *n = &g->note;
    struct note_layout l;
    struct gui_draw text;
    bool on[TOOLS], focused = g->focus == GUI_NOTEPAD && !g->dialog.kind && !g->menu && !n->finding;
    char b[GUI_STATUS + 32], right[48];
    int x0 = w->x, y0 = w->y, row, col, lines = note_lines(n), cur_line, cur_col, x, i;
    u32 off, a, z, k, len = 0;
    note_layout(g, &l);
    selection(n, &a, &z);
    for(i = 0; i < TOOLS; i++) on[i] = tool_on(g, tools[i].act);
    gd_rect(d, x0 + 1, y0 + TITLE_H, w->w - 2, BAR_H + 2, BAND);
    tool_render(d, x0, y0, tools, TOOLS, on);
    gd_rect(d, x0 + 1, y0 + TITLE_H + BAR_H + 2, w->w - 2, 1, LINE);
    gd_rect(d, x0 + 1, y0 + l.ty - 1, l.gutter + 2, l.find_y - l.ty + 1, BAND);
    gd_rect(d, x0 + l.gutter + 3, y0 + l.ty - 1, l.bar_x - l.gutter - 3, l.find_y - l.ty + 1, 0xFFFDF5);
    text = gd_clip(d, x0 + l.tx - 1, y0 + l.ty, l.cols * 8 + 2, l.rows * 16);
    cur_line = note_line_of(n, n->cursor);
    cur_col = note_column(n, n->cursor);
    off = note_line_start(n, n->top);
    for(row = 0; row < l.rows && n->top + row < lines; row++) {
        int py = y0 + l.ty + row * 16;
        snprintf(b, sizeof(b), "%d", n->top + row + 1);
        gd_text(d, x0 + 2 + l.gutter - 6 - (int)strlen(b) * 8, py, b, n->top + row == cur_line ? TEAL : MUTED);
        col = 0;
        for(k = off; k < n->len && n->text[k] != '\n'; k += len) {
            int cells = cells_at(n, k, col);
            len = char_len(n, k);
            if(col >= n->left + l.cols) { k = line_finish(n, k); break; }
            if(col + cells > n->left) {
                x = x0 + l.tx + (col - n->left) * 8;
                if(k >= a && k < z) gd_rect(&text, x, py, cells * 8, 16, PICK);
                if(n->text[k] != '\t') gd_glyph(&text, x, py, glyph_at(n, k), INK, 1);
            }
            col += cells;
        }
        if(k < n->len && k >= a && k < z && col >= n->left && col < n->left + l.cols)
            gd_rect(&text, x0 + l.tx + (col - n->left) * 8, py, 8, 16, PICK);      /* a selected line end */
        if(focused && n->top + row == cur_line && cur_col >= n->left && cur_col <= n->left + l.cols)
            gd_rect(&text, x0 + l.tx + (cur_col - n->left) * 8 - 1, py, 2, 16, INK);
        if(k >= n->len) break;
        off = k + 1;
    }
    gd_scrollbar(d, x0 + l.bar_x, y0 + l.ty - 2, l.find_y - l.ty + 2, n->top, l.rows, lines);
    if(n->finding) {
        gd_rect(d, x0 + 1, y0 + l.find_y, w->w - 2, 28, BAND);
        gd_rect(d, x0 + 1, y0 + l.find_y, w->w - 2, 1, LINE);
        gd_text(d, x0 + 10, y0 + l.find_y + 6, "Find:", INK);
        field_render(d, &n->find, x0 + 56, y0 + l.find_y + 3, w->w - 56 - 108, !g->dialog.kind && g->focus == GUI_NOTEPAD);
        gd_button(d, x0 + w->w - 100, y0 + l.find_y + 3, 60, 22, "Next", TEAL, PAPER);
        gd_button(d, x0 + w->w - 34, y0 + l.find_y + 3, 24, 22, "x", SHELL, INK);
    }
    gd_rect(d, x0 + 1, y0 + l.status_y, w->w - 2, STATUS_H, BAND);
    gd_rect(d, x0 + 1, y0 + l.status_y, w->w - 2, 1, LINE);
    snprintf(right, sizeof(right), "%u bytes  %s%s", n->len, n->crlf ? "CRLF" : "LF", n->readonly ? "  read-only" : "");
    if(a != z) snprintf(b, sizeof(b), "Ln %d, Col %d (%u selected)  %s", cur_line + 1, cur_col + 1, z - a, n->status);
    else snprintf(b, sizeof(b), "Ln %d, Col %d  %s", cur_line + 1, cur_col + 1, n->status);
    x = w->w - 24 - (int)strlen(right) * 8;                /* clear of the resize grip */
    gd_name(d, x0 + 10, y0 + l.status_y + 3, b, (x - 18) / 8, INK);
    gd_text(d, x0 + x, y0 + l.status_y + 3, right, MUTED);
}
