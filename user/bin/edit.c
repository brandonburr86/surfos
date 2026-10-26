/*
SurfOS user program: edit
--------------------
File: edit.c    Date: 10/5/26
--------------------
edit <file>: a small full-screen text editor in the manner of nano. The whole
file lives in memory as an array of lines (files up to 1 MB). The screen is a
title line, the text, a status line and a help line. Lines do not wrap: the
line with the cursor scrolls sideways to keep it in view, the others show their
start, and a '>' in the last column says a line goes on.

Keys: arrows, Home, End, PgUp, PgDn, Backspace, Delete, Enter, Tab (a tab
character, shown as spaces up to the next multiple of 8); ^S save, ^Q quit
(asking first when there are unsaved changes), ^F find (Enter alone repeats the
last search), ^K cut the line (several in a row go together), ^U paste them,
^G go to a line, ^C where the cursor is, ^L draw the screen again.

Saving writes the text to a new file beside the file, fsyncs it and renames it
over the file. 10/17/26, as the desktop's Notepad saves (shell/guifs.c): the new
file is <file>.tmp, or <file>.1.tmp and so on when the user has a file of that
name (O_EXCL: theirs is never touched). It is deleted again when this save could
not write it, and when the rename failed before it changed anything. After an
I/O error the rename may have half happened, the new name written and the old
one not yet gone: the text stays in the new file (the status line names it),
but the file and it may share their clusters, and deleting or replacing either
would free the other's. So for the rest of the session nothing is renamed or
written over the file: each save makes another new file beside it (up to .9.tmp,
and none when nothing changed since the last), the help line says to have the
disk checked first, and edit says where the last copy is when it ends. The same
holds whenever edit finds such a name beside the file, of its size and time
(fs/fat.c's rename copies the entry; two empty files hold no clusters, so are no
pair), when it opens it and before each save.
Only where the file system cannot rename (ENOSYS), or no such name fits in a
path, is the file truncated and written in place.
The file keeps its line ends (LF, or CRLF when its first line has one) and
ends with a line end. The terminal is used only through ttyctl(TTY_RAW),
ttyctl(TTY_GETSIZE), the key sequences of raw mode and the ANSI subset that
include/surfos/tty.h lists, so the editor works on the screen and over the
serial line alike. Each refresh rewrites only the rows that changed.

10/25/26 (track A, A9): ^Z undoes the last change and ^Y does it again. A run of typing,
of Backspace or of Delete is one change (it ends when the cursor goes elsewhere or another
key changes the text), and so are a run of ^K, a paste and an Enter; the last 1,000 changes
are kept, a save keeps them too, and the text as it was saved counts as unmodified again
when undo or redo comes back to it. UTF-8 is text: a character is one cell, drawn as it is
where the console draws UTF-8 (TTY_CAP_UTF8) and as '?' elsewhere, and the arrows,
Backspace and Delete take a whole character; a byte that is not UTF-8 is a '?' of its own.
The rows of the screen are kept at 4 bytes a cell. The help line has no room for ^Z and ^Y.
edit draws on the alternate screen (TTY_CAP_ALTSCREEN, A8) and gives the shell's screen back
when it ends; a terminal without one is left clear, as before.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <surf.h>

#define MAX_FILE   (1024 * 1024)
#define TAB        8
#define CTRL(c)    ((c) & 0x1f)
#define HELP       "^S Save  ^Q Quit  ^F Find  ^K Cut line  ^U Paste  ^G Go to line"
#define UNSURE     "A file beside this one may share its data: check the disk (fsck) first"

enum {
    KEY_NONE = 0x100, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END,
    KEY_PGUP, KEY_PGDN, KEY_INS, KEY_DEL, KEY_ESC, KEY_EOF
};

struct line {
    char *s;                    /* not terminated */
    int len, cap;
};

static struct line *lines;      /* the text: nlines >= 1 */
static int nlines, lines_cap;
static struct line *cut;        /* ^K collects lines here, ^U inserts them */
static int ncut, cut_cap, cutting;

static const char *filename;
static int crlf;                /* the file's line ends are CR LF */
static int modified;
static int no_rename;           /* rename said ENOSYS once: write in place from then on */
static int unsure;              /* a rename may have half happened: the file and its .tmp may share their clusters */
static char copied[PATH_MAX];   /* since then: the last copy of the text beside the file */
static unsigned changes, copied_changes;    /* edits so far, and when that copy was made */
static unsigned saved_serial;   /* 10/25/26: the text as it was saved, as undo numbers the texts (below) */
static int open_change;         /* the last change undo keeps takes the next edit (below) */
static unsigned serial_now(void);

static int cy, cx;              /* cursor: line, byte in the line */
static int want_col = -1;       /* the screen column up and down aim for */
static int top;                 /* first line on the screen */
static int left, left_line = -1;    /* first column shown of the cursor's line, and that line */
static int rows, cols, text_rows;
static int utf8;                /* 10/25/26: the console draws UTF-8 (TTY_CAPS) */
#define ROW_BYTES (4 * cols + 1)    /* a row of the screen as kept: 4 bytes a cell (UTF-8) and its end */

static char msg[200];           /* for the status line until the next key */
static const char *prompt_label;    /* while asking: the question and the answer so far */
static char prompt_buf[128];
static char last_find[128];

/**** output: one buffer per refresh, one write ****/

static char *out;
static size_t out_len, out_cap;

static void out_add(const char *s, size_t n) {
    if(out_len + n > out_cap) {
        size_t cap = out_cap ? out_cap * 2 : 16384;
        char *p;
        while(cap < out_len + n) cap *= 2;
        p = realloc(out, cap);
        if(!p) return;                  /* this refresh is incomplete; ^L draws again */
        out = p;
        out_cap = cap;
    }
    memcpy(out + out_len, s, n);
    out_len += n;
}

static void out_str(const char *s) {
    out_add(s, strlen(s));
}

static void out_flush(void) {
    size_t done = 0;
    while(done < out_len) {
        ssize_t n = write(1, out + done, out_len - done);
        if(n <= 0) break;
        done += (size_t)n;
    }
    out_len = 0;
}

static void move_to(int row, int col) {         /* 0-based */
    char seq[24];
    snprintf(seq, sizeof(seq), "\x1b[%d;%dH", row + 1, col + 1);
    out_str(seq);
}

/**** the terminal ****/

static int raw_mode;

static void restore_terminal(void) {   /* 10/25/26: the alternate screen cleared, then left */
    static const char bye[] = "\x1b[0m\x1b[2J\x1b[H\x1b[?25h\x1b[?1049l";
    if(!raw_mode) return;
    raw_mode = 0;
    write(1, bye, sizeof(bye) - 1);
    ttyctl(TTY_RAW, 0);
}

static void screen_size(void) {
    int r = ttyctl(TTY_GETSIZE, 0);
    cols = r > 0 ? (r >> 16) & 0xffff : 80;
    rows = r > 0 ? r & 0xffff : 25;
    if(cols < 16) cols = 16;
    if(cols > 512) cols = 512;
    if(rows < 4) rows = 4;
    if(rows > 256) rows = 256;
    text_rows = rows - 3;
}

/**** keys ****/

static unsigned char inbuf[64];
static int inpos, inlen;
static unsigned int lone_esc_at;    /* uptime of the last lone ESC, to mend a sequence split across reads */

static int next_byte(void) {
    if(inpos == inlen) {
        ssize_t n = read(0, inbuf, sizeof(inbuf));
        if(n <= 0) return -1;
        inpos = 0;
        inlen = (int)n;
    }
    return inbuf[inpos++];
}

/* the rest of ESC [ or ESC O: the parameter digits and the final letter */
static int sequence(void) {
    int c, num = 0;
    for(;;) {
        c = next_byte();
        if(c < 0) return KEY_EOF;
        if(c >= '0' && c <= '9') { num = num * 10 + (c - '0'); continue; }
        if(c == ';') { num = 0; continue; }     /* modifiers (ESC[1;5C): the key is what counts */
        break;
    }
    switch(c) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case '~':
        switch(num) {
        case 1: case 7: return KEY_HOME;
        case 4: case 8: return KEY_END;
        case 2: return KEY_INS;
        case 3: return KEY_DEL;
        case 5: return KEY_PGUP;
        case 6: return KEY_PGDN;
        }
    }
    return KEY_NONE;
}

static int get_key(void) {
    int c = next_byte();
    if(c < 0) return KEY_EOF;
    if((c == '[' || c == 'O') && lone_esc_at && uptime() - lone_esc_at < 250) {
        lone_esc_at = 0;                        /* "ESC" and "[A" came in two reads: still an arrow */
        return sequence();
    }
    lone_esc_at = 0;
    if(c != 0x1b) return c;
    if(inpos == inlen) {                        /* nothing came with it: the Escape key */
        lone_esc_at = uptime();
        if(!lone_esc_at) lone_esc_at = 1;
        return KEY_ESC;
    }
    c = next_byte();
    if(c == '[' || c == 'O') return sequence();
    inpos--;                                    /* ESC and then another key: Escape, then that key */
    return KEY_ESC;
}

/**** lines ****/

static int line_room(struct line *l, int need) {
    if(need <= l->cap) return 0;
    {
        int cap = l->cap ? l->cap : 16;
        char *p;
        while(cap < need) cap *= 2;
        p = realloc(l->s, (size_t)cap);
        if(!p) { snprintf(msg, sizeof(msg), "Out of memory"); return -1; }
        l->s = p;
        l->cap = cap;
    }
    return 0;
}

static int lines_room(int need) {
    if(need <= lines_cap) return 0;
    {
        int cap = lines_cap ? lines_cap : 64;
        struct line *p;
        while(cap < need) cap *= 2;
        p = realloc(lines, (size_t)cap * sizeof(struct line));
        if(!p) { snprintf(msg, sizeof(msg), "Out of memory"); return -1; }
        lines = p;
        lines_cap = cap;
    }
    return 0;
}

/* a new line at index at with the text s[0..len) */
static int insert_line(int at, const char *s, int len) {
    struct line l = { NULL, 0, 0 };
    if(lines_room(nlines + 1) || line_room(&l, len > 0 ? len : 1)) return -1;
    memcpy(l.s, s, (size_t)len);
    l.len = len;
    memmove(lines + at + 1, lines + at, (size_t)(nlines - at) * sizeof(struct line));
    lines[at] = l;
    nlines++;
    return 0;
}

static void delete_line(int at) {
    free(lines[at].s);
    memmove(lines + at, lines + at + 1, (size_t)(nlines - at - 1) * sizeof(struct line));
    nlines--;
}

/* 10/25/26 (A9): the bytes of the UTF-8 character at s (n bytes there); 1 for ASCII and for a byte that does not
   start one */
static int utf8_len(const unsigned char *s, int n) {
    int len, k;
    if(n < 2 || s[0] < 0xC2 || s[0] > 0xF4) return 1;
    len = s[0] < 0xE0 ? 2 : s[0] < 0xF0 ? 3 : 4;
    if(len > n) return 1;
    for(k = 1; k < len; k++)
        if((s[k] & 0xC0) != 0x80) return 1;
    if((s[0] == 0xE0 && s[1] < 0xA0) || (s[0] == 0xED && s[1] >= 0xA0) ||   /* too long a form; a surrogate */
       (s[0] == 0xF0 && s[1] < 0x90) || (s[0] == 0xF4 && s[1] >= 0x90)) return 1;   /* too long; past U+10FFFF */
    return len;
}

/* the bytes of the character at byte i of l */
static int char_len(const struct line *l, int i) {
    return utf8_len((const unsigned char *)l->s + i, l->len - i);
}

/* where the character before byte i (> 0) of l starts */
static int char_before(const struct line *l, int i) {
    int k = i - 1, n = 0;
    while(k > 0 && n < 3 && ((unsigned char)l->s[k] & 0xC0) == 0x80) {
        k--;
        n++;
    }
    return k + char_len(l, k) == i ? k : i - 1;
}

/* how many bytes of the string s its first max cells take; *cells: how many cells that is */
static int cells_bytes(const char *s, int max, int *cells) {
    int k = 0, c = 0, n = (int)strlen(s);
    while(k < n && c < max) {
        k += utf8_len((const unsigned char *)s + k, n - k);
        c++;
    }
    if(cells) *cells = c;
    return k;
}

static int cells_of(const char *s) {
    int c;
    cells_bytes(s, 1 << 30, &c);
    return c;
}

/* the screen column where byte i of line l starts (10/25/26: a UTF-8 character is one cell) */
static int col_of(const struct line *l, int i) {
    int k, col = 0;
    for(k = 0; k < i && k < l->len; k += char_len(l, k)) col = l->s[k] == '\t' ? (col / TAB + 1) * TAB : col + 1;
    return col;
}

/* the byte where the character whose cell covers screen column col starts (or the end of the line) */
static int index_of(const struct line *l, int col) {
    int k, c = 0;
    for(k = 0; k < l->len; k += char_len(l, k)) {
        int next = l->s[k] == '\t' ? (c / TAB + 1) * TAB : c + 1;
        if(next > col) return k;
        c = next;
    }
    return l->len;
}

/**** loading and saving ****/

/* the lines of the file: the empty line after a final line end does not count */
static int text_lines(void) {
    return nlines - (nlines > 1 && !lines[nlines - 1].len);
}

static int load(const char *path) {
    struct stat st;
    char *buf, *p, *end;
    int fd, n = 0, total = 0;
    fd = open(path, O_RDONLY);
    if(fd < 0) {
        if(errno != ENOENT) return -1;
        snprintf(msg, sizeof(msg), "New file");
        return insert_line(0, "", 0);
    }
    if(fstat(fd, &st) < 0) { close(fd); return -1; }
    if(S_ISDIR(st.type)) { close(fd); errno = EISDIR; return -1; }
    if(st.size > MAX_FILE) { close(fd); errno = EFBIG; return -1; }
    buf = malloc(st.size + 1);
    if(!buf) { close(fd); errno = ENOMEM; return -1; }
    while(total <= MAX_FILE && (n = (int)read(fd, buf + total, st.size + 1 - total)) > 0) {
        total += n;
        if(total == (int)st.size + 1) break;    /* it grew while we read: too big now, or changing */
    }
    close(fd);
    if(n < 0) { free(buf); return -1; }
    if(total > (int)st.size) { free(buf); errno = EFBIG; return -1; }
    p = memchr(buf, '\n', (size_t)total);
    crlf = p && p > buf && p[-1] == '\r';
    for(p = buf, end = buf + total;;) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        int len = nl ? (int)(nl - p) : (int)(end - p);
        if(nl && crlf && len && p[len - 1] == '\r') len--;
        if(insert_line(nlines, p, len)) { free(buf); errno = ENOMEM; return -1; }
        if(!nl) break;
        p = nl + 1;
    }
    free(buf);
    n = text_lines();
    snprintf(msg, sizeof(msg), "Read %d line%s%s", n, n == 1 ? "" : "s", crlf ? " (CRLF line ends)" : "");
    return 0;
}

static int write_chunk(int fd, const char *p, int n) {
    ssize_t w = write(fd, p, (size_t)n);
    if(w == n) return 0;
    if(w >= 0) errno = ENOSPC;                  /* a short write: the disk is full */
    return -1;
}

/* every line and its line end; the last line gets one unless it is empty */
static int write_text(int fd) {
    static char chunk[8192];
    int used = 0, i;
    for(i = 0; i < nlines; i++) {
        const struct line *l = &lines[i];
        int k = 0, eol = (i < nlines - 1 || l->len > 0);
        while(k < l->len + (eol ? (crlf ? 2 : 1) : 0)) {
            if(used == (int)sizeof(chunk)) {
                if(write_chunk(fd, chunk, used)) return -1;
                used = 0;
            }
            if(k < l->len) chunk[used++] = l->s[k];
            else chunk[used++] = (crlf && k == l->len) ? '\r' : '\n';
            k++;
        }
    }
    return used ? write_chunk(fd, chunk, used) : 0;
}

/* the text into path, then fsync; how: O_EXCL for a new file (*made: this call made it, so it is
   ours to delete), or O_TRUNC for the file itself */
static int write_file(const char *path, int how, int *made) {
    int fd = open(path, O_WRONLY | O_CREAT | how), e;
    if(made) *made = fd >= 0;
    if(fd < 0) return -1;
    if(write_text(fd) < 0 || fsync(fd) < 0) {
        e = errno ? errno : EIO;
        close(fd);
        errno = e;
        return -1;
    }
    return close(fd);
}

/* a rename that failed with one of these changed nothing (the system call's check of its strings, fs/vfs.c's
   checks, and fs/fat.c's before it writes a name): its new file may go. After any other, EIO above all, the
   new name may be written and the old one not yet gone, both holding the same clusters, and deleting the
   .tmp would free the file's. */
static int nothing_changed(int e) {
    return e == ENOSYS || e == EXDEV || e == EROFS || e == EBUSY || e == EINVAL || e == ENOENT || e == EFAULT ||
           e == ENOTDIR || e == EISDIR || e == ENOTEMPTY || e == ENAMETOOLONG || e == ENOSPC || e == ENOMEM;
}

static int same_entry(const struct stat *a, const char *name) {
    struct stat b;
    return stat(name, &b) == 0 && S_ISREG(b.type) && b.size == a->size && b.mtime == a->mtime;
}

/* a name beside the file with its size and time, into twin: fs/fat.c's rename copies the entry, so after one
   that may have half happened (a save of edit's or the desktop's, or rx's) the two hold the same clusters; or
   the user made such a copy, and the care costs nothing. Beside a.txt: a.txt.tmp, a.txt.1.tmp to .9.tmp and
   a.txt.part; beside one of those, a.txt (and a.txt.1 too beside a.txt.1.tmp). A guess, not proof: a later
   write to either name, or a rename that left the old entry first in the directory, hides such a pair. */
static int twin_beside(char *twin, size_t size) {
    static const char *const ends[] = { ".tmp", ".part" };
    struct stat st;
    size_t n = strlen(filename), e;
    unsigned i;
    int k, w;
    if(stat(filename, &st) < 0 || !S_ISREG(st.type) || !st.size) return 0;   /* an empty file holds no clusters */
    for(k = 0; k <= 10; k++) {
        if(k < 10) w = snprintf(twin, size, k ? "%s.%d.tmp" : "%s.tmp", filename, k);
        else w = snprintf(twin, size, "%s.part", filename);
        if(w < (int)size && same_entry(&st, twin)) return 1;
    }
    for(i = 0; i < sizeof(ends) / sizeof(ends[0]); i++) {     /* the file is <name>.tmp, <name>.N.tmp, <name>.part */
        e = strlen(ends[i]);
        if(n <= e || n >= size || strcasecmp(filename + n - e, ends[i])) continue;   /* FAT's names ignore case */
        memcpy(twin, filename, n - e);
        twin[n - e] = 0;
        if(same_entry(&st, twin)) return 1;
        if(i == 0 && n - e >= 3 && twin[n - e - 2] == '.' && twin[n - e - 1] >= '1' && twin[n - e - 1] <= '9') {
            twin[n - e - 2] = 0;                        /* a.txt beside a.txt.1.tmp */
            if(same_entry(&st, twin)) return 1;
        }
    }
    return 0;
}

/* with such a name beside the file, this edit is as after a rename that may have half happened: 1, and the
   name's last part in twin */
static int find_twin(char *twin, size_t size) {
    const char *base;
    if(!twin_beside(twin, size)) return 0;
    unsure = 1;
    base = strrchr(twin, '/');
    if(base) memmove(twin, base + 1, strlen(base + 1) + 1);
    return 1;
}

/* after loading: such a pair is said at once */
static void look_beside(void) {
    char twin[PATH_MAX];
    if(find_twin(twin, sizeof(twin))) snprintf(msg, sizeof(msg), "%s may share this file's data: check the disk", twin);
}

static int save(void) {
    char tmp[PATH_MAX], twin[PATH_MAX];         /* the kernel's limit: a longer path is refused (EFAULT) */
    const char *what = filename, *base;
    struct stat st;
    int made = 0, kept = 0, k, e = ENAMETOOLONG, found = !unsure && find_twin(twin, sizeof(twin));   /* since opening */
    if(unsure && copied[0] && changes == copied_changes) {      /* no edit since the last copy: no other copy */
        snprintf(msg, sizeof(msg), "Not saved over the file; the text is in %s", copied);
        return 1;
    }
    if(!no_rename || unsure) {                  /* a copy beside the file needs no rename */
        for(k = 0; k < 10; k++) {
            if(snprintf(tmp, sizeof(tmp), k ? "%s.%d.tmp" : "%s.tmp", filename, k) >= (int)sizeof(tmp)) break;
            e = write_file(tmp, O_EXCL, &made) < 0 ? errno : 0;
            if(e != EEXIST) break;              /* a file the user has under that name: the next name */
        }
        if(e == ENAMETOOLONG && k > 0) e = EEXIST;      /* <file>.tmp is there, the next name is too long: not in place */
        what = tmp;
        base = strrchr(tmp, '/');
        if(e == 0 && unsure) {                  /* a copy beside the file, renamed over nothing */
            snprintf(copied, sizeof(copied), "%s", base ? base + 1 : tmp);
            copied_changes = changes;
            if(found) snprintf(msg, sizeof(msg), "Not saved over the file; a copy is in %s (%s may share its data)", copied, twin);
            else snprintf(msg, sizeof(msg), "Not saved over the file; a copy is in %s", copied);
            return 1;
        }
        if(e == 0) {
            if(rename(tmp, filename) == 0) goto done;
            e = errno;
            what = "rename";
            if(!nothing_changed(e)) {           /* perhaps half done: the text stays in the new file, if it is there */
                kept = stat(tmp, &st) == 0;
                unsure = 1;
                if(kept) {                      /* the text as it is now, in the copy said at the end */
                    snprintf(copied, sizeof(copied), "%s", base ? base + 1 : tmp);
                    copied_changes = changes;
                }
                goto fail;
            }
            kept = unlink(tmp) < 0 && errno != ENOENT;  /* it would not go: the text is in it */
            if(e != ENOSYS) goto fail;
            no_rename = 1;
        } else if(made) {
            unlink(tmp);                        /* made by this save: what of the text it got */
            goto fail;
        } else if(e != ENAMETOOLONG || unsure) goto fail;   /* nothing was made, nothing of the user's touched */
    }
    what = filename;                            /* no rename here, or no room for such a name: truncate the file and write it */
    if(write_file(filename, O_TRUNC, NULL) < 0) { e = errno; goto fail; }
done:
    modified = 0;
    saved_serial = serial_now();                /* 10/25/26: undo back to here is the saved text again */
    open_change = 0;                            /* and typing on starts a change of its own */
    snprintf(msg, sizeof(msg), "Wrote %d line%s", text_lines(), text_lines() == 1 ? "" : "s");
    return 0;
fail:                                           /* the reason first: a long name is cut off at the right */
    if(kept) {                                  /* the new file, beside the file */
        base = strrchr(tmp, '/');
        snprintf(msg, sizeof(msg), "Not saved: %s (%s). The text is in %s", strerror(e), what, base ? base + 1 : tmp);
    } else if(e == EEXIST && what == tmp && unsure)     /* no name left for a copy: nothing to move, the disk to check */
        snprintf(msg, sizeof(msg), "Not saved: no name is left for a copy beside the file");
    else if(e == EEXIST && what == tmp)         /* <file>.tmp, and the names after it, are the user's */
        snprintf(msg, sizeof(msg), "Not saved: %s (%s.tmp): move it out of the way", strerror(e), filename);
    else snprintf(msg, sizeof(msg), "Not saved: %s (%s)", strerror(e), what);
    return -1;
}

/**** the screen ****/

/* what each row shows now: its text and whether it is drawn inverse */
static char *shadow;            /* rows * (cols + 1) */
static unsigned char *shadow_inv;
static int shadow_ok;

static void draw_row(int row, const char *text, int inverse) {
    char *old = shadow + row * ROW_BYTES;
    int used, width = row == rows - 1 ? cols - 1 : cols;    /* never the last cell: no scrolling */
    int len = cells_bytes(text, width, &used);              /* 10/25/26: cells, not bytes (UTF-8) */
    if(shadow_ok && shadow_inv[row] == inverse && (int)strlen(old) == len && !memcmp(old, text, (size_t)len)) return;
    move_to(row, 0);
    if(inverse) {                                       /* a bar across the screen */
        int k;
        out_str("\x1b[7m");
        out_add(text, (size_t)len);
        for(k = used; k < width; k++) out_add(" ", 1);
        out_str("\x1b[0m");
    } else {
        out_add(text, (size_t)len);
        if(used < width) out_str("\x1b[K");
    }
    memcpy(old, text, (size_t)len);
    old[len] = 0;
    shadow_inv[row] = (unsigned char)inverse;
}

/* the cells of line l from screen column from on; '>' in the last one when it goes on. 10/25/26: a UTF-8 character
   as its bytes where the console draws them, '?' where not, as for a control character, a C1 control (U+0080 to
   U+009F, which the console never draws) and a byte that is not UTF-8; dst holds 4 bytes a cell */
static void render_line(const struct line *l, char *dst, int from) {
    int k, col = 0, n = 0, len, used = 0, width = cols - 1;
    for(k = 0; k < l->len && col < from + width; k += len) {
        unsigned char c = (unsigned char)l->s[k];
        int next = c == '\t' ? (col / TAB + 1) * TAB : col + 1;
        len = char_len(l, k);
        for(; col < next; col++) {
            if(col < from || col >= from + width) continue;
            if(len > 1 && utf8 && !(c == 0xC2 && (unsigned char)l->s[k + 1] < 0xA0)) {
                memcpy(dst + n, l->s + k, (size_t)len);
                n += len;
            } else dst[n++] = c == '\t' ? ' ' : (c >= 32 && c < 127) ? (char)c : '?';
            used++;
        }
    }
    if(k < l->len || col > from + width) {      /* more to the right */
        while(used++ < width) dst[n++] = ' ';
        dst[n++] = '>';
    }
    while(n > 0 && dst[n - 1] == ' ') n--;      /* trailing blanks are what ESC[K leaves anyway */
    dst[n] = 0;
}

static void scroll_into_view(void) {
    int col = col_of(&lines[cy], cx);
    if(cy != left_line) { left = 0; left_line = cy; }  /* another line: it starts at its start */
    if(cy < top) top = cy;
    if(cy >= top + text_rows) top = cy - text_rows + 1;
    if(col < left) left = col > cols / 2 ? col - cols / 2 : 0;
    if(col >= left + cols - 1) left = col - cols / 2;
}

static void refresh(void) {
    char *row = malloc((size_t)ROW_BYTES + 1), info[64];
    int r, n, c, crow, ccol;
    if(!row) return;
    scroll_into_view();
    out_str("\x1b[?25l");
    if(!shadow_ok) out_str("\x1b[0m\x1b[2J");

    snprintf(row, (size_t)ROW_BYTES, " SurfOS edit  %s%s", filename, crlf ? "  (CRLF)" : "");
    if(modified) {                                      /* "Modified" at the right, over a long name if need be */
        n = cells_bytes(row, cols - 10, &c);
        while(c++ < cols - 10) row[n++] = ' ';
        strcpy(row + n, "Modified");
    }
    draw_row(0, row, 1);

    for(r = 0; r < text_rows; r++) {
        if(top + r < nlines) render_line(&lines[top + r], row, top + r == cy ? left : 0);
        else row[0] = 0;
        draw_row(1 + r, row, 0);
    }

    if(prompt_label) {
        snprintf(row, (size_t)ROW_BYTES, "%s%s", prompt_label, prompt_buf);
        draw_row(rows - 2, row, 0);
    } else {
        snprintf(info, sizeof(info), "Ln %d/%d  Col %d", cy + 1, nlines, col_of(&lines[cy], cx) + 1);
        snprintf(row, (size_t)ROW_BYTES, " %s", msg);
        n = cells_bytes(row, cols, &c);
        row[n] = 0;
        if(c + (int)strlen(info) + 2 <= cols - 1) {
            while(c++ < cols - 1 - (int)strlen(info)) row[n++] = ' ';
            strcpy(row + n, info);
        }
        draw_row(rows - 2, row, 0);
    }
    snprintf(row, (size_t)ROW_BYTES, "%s", unsure ? UNSURE : HELP);
    draw_row(rows - 1, row, unsure);
    shadow_ok = 1;

    if(prompt_label) {
        crow = rows - 2;
        ccol = cells_of(prompt_label) + cells_of(prompt_buf);
        if(ccol > cols - 1) ccol = cols - 1;
    } else {
        crow = 1 + cy - top;
        ccol = col_of(&lines[cy], cx) - left;
    }
    move_to(crow, ccol);
    out_str("\x1b[?25h");
    out_flush();
    free(row);
}

/**** the text's changes, and undoing them (10/25/26, track A, A9) ****/

/* Every change of the text is one of these, kept with what it took or gave, so that it can be undone and done again:
   bytes into a line or out of it, a line split in two or two joined, a whole line in or out (a paste, ^K) */
enum { OP_INS, OP_DEL, OP_SPLIT, OP_JOIN, OP_LINE_INS, OP_LINE_DEL };

struct op {
    int kind, y, x;             /* the line and the byte (OP_JOIN: where the second line begins in the first) */
    char *s;                    /* the bytes put in or taken out */
    int n;
};

struct change {                 /* what ^Z undoes: the ops of a key or a run of keys */
    struct op *ops;
    int nops, cap;
    int y0, x0, y1, x1;         /* the cursor before and after */
    unsigned serial;            /* the text after it, as a number: the saved text is saved_serial */
};

#define UNDO_MAX 1000

enum { K_NONE, K_TYPE, K_BACK, K_DEL, K_CUT, K_OTHER };    /* the change a key makes: runs of the same go together */

static struct change undos[UNDO_MAX], redos[UNDO_MAX];
static int nundo, nredo;
static int open_kind;           /* the kind of key the open change holds */
static int key_kind, key_changed;   /* the key being handled: its kind; it changed the text */
static unsigned serials;
static unsigned base_serial;    /* 10/25/26 (the programs' review, U2): the text with nothing left to undo, as a number: 0
                                   as read, a change's when it was pushed out of the 1,000, a new one when the history
                                   is forgotten; so undoing all that is left is the saved text only when it is */

static unsigned serial_now(void) {
    return nundo ? undos[nundo - 1].serial : base_serial;
}

static void free_change(struct change *c) {
    int i;
    for(i = 0; i < c->nops; i++) free(c->ops[i].s);
    free(c->ops);
    memset(c, 0, sizeof(*c));
}

static void forget_history(void) {
    while(nundo) free_change(&undos[--nundo]);
    while(nredo) free_change(&redos[--nredo]);
    open_change = 0;
    base_serial = ++serials;            /* 10/25/26 (U2): the text now, which no undo gives back */
}

static void set_modified(void) {
    modified = 1;
    changes++;
}

/* an op of the key being handled, with its bytes: into the open change, joined to its last op when it goes on from
   it (typing at its end, Backspace before it, Delete where it was), or into a new change; when memory runs out, the
   history is forgotten rather than kept with a gap */
static void record(int kind, int y, int x, const char *s, int n) {
    struct change *c;
    struct op *last;
    char *p;
    if(!open_change) {
        while(nredo) free_change(&redos[--nredo]);      /* a new change: what was undone cannot be done again */
        if(nundo == UNDO_MAX) {
            base_serial = undos[0].serial;      /* 10/25/26 (U2): undo stops at the text after it from now on */
            free_change(&undos[0]);
            memmove(undos, undos + 1, (UNDO_MAX - 1) * sizeof(struct change));
            nundo--;
            memset(&undos[nundo], 0, sizeof(struct change));
        }
        c = &undos[nundo++];
        memset(c, 0, sizeof(*c));
        c->y0 = cy;
        c->x0 = cx;
        c->serial = ++serials;
        open_change = 1;
        open_kind = key_kind;
    }
    c = &undos[nundo - 1];
    key_changed = 1;
    last = c->nops ? &c->ops[c->nops - 1] : NULL;
    if(last && last->y == y && n > 0 &&
       ((kind == OP_INS && last->kind == OP_INS && last->x + last->n == x) ||
        (kind == OP_DEL && last->kind == OP_DEL && (x + n == last->x || x == last->x)))) {
        p = realloc(last->s, (size_t)(last->n + n));
        if(!p) goto out_of_memory;
        if(kind == OP_DEL && x + n == last->x) {        /* Backspace: before what it took */
            memmove(p + n, p, (size_t)last->n);
            memcpy(p, s, (size_t)n);
            last->x = x;
        } else memcpy(p + last->n, s, (size_t)n);
        last->s = p;
        last->n += n;
        return;
    }
    if(c->nops == c->cap) {
        int cap = c->cap ? c->cap * 2 : 4;
        struct op *o = realloc(c->ops, (size_t)cap * sizeof(struct op));
        if(!o) goto out_of_memory;
        c->ops = o;
        c->cap = cap;
    }
    p = n > 0 ? malloc((size_t)n) : NULL;
    if(n > 0 && !p) goto out_of_memory;
    if(n > 0) memcpy(p, s, (size_t)n);
    c->ops[c->nops].kind = kind;
    c->ops[c->nops].y = y;
    c->ops[c->nops].x = x;
    c->ops[c->nops].s = p;
    c->ops[c->nops++].n = n;
    return;
out_of_memory:
    forget_history();
    snprintf(msg, sizeof(msg), "Out of memory: what ^Z could undo is forgotten");
}

/* the text itself, changed and nothing recorded; -1 without memory */
static int text_insert(int y, int x, const char *s, int n) {
    struct line *l = &lines[y];
    if(line_room(l, l->len + n)) return -1;
    memmove(l->s + x + n, l->s + x, (size_t)(l->len - x));
    memcpy(l->s + x, s, (size_t)n);
    l->len += n;
    return 0;
}

static void text_delete(int y, int x, int n) {
    struct line *l = &lines[y];
    memmove(l->s + x, l->s + x + n, (size_t)(l->len - x - n));
    l->len -= n;
}

static int text_split(int y, int x) {
    if(insert_line(y + 1, lines[y].s + x, lines[y].len - x)) return -1;
    lines[y].len = x;
    return 0;
}

static int text_join(int y) {
    struct line *a = &lines[y], *b = &lines[y + 1];
    if(line_room(a, a->len + b->len)) return -1;
    memcpy(a->s + a->len, b->s, (size_t)b->len);
    a->len += b->len;
    delete_line(y + 1);
    return 0;
}

/* an op done again (undo 0) or undone (undo 1) */
static int replay(const struct op *o, int undo) {
    switch(o->kind) {
    case OP_INS: if(!undo) return text_insert(o->y, o->x, o->s, o->n); text_delete(o->y, o->x, o->n); return 0;
    case OP_DEL: if(undo) return text_insert(o->y, o->x, o->s, o->n); text_delete(o->y, o->x, o->n); return 0;
    case OP_SPLIT: return undo ? text_join(o->y) : text_split(o->y, o->x);
    case OP_JOIN: return undo ? text_split(o->y, o->x) : text_join(o->y);
    case OP_LINE_INS: if(!undo) return insert_line(o->y, o->s, o->n); delete_line(o->y); return 0;
    case OP_LINE_DEL: if(undo) return insert_line(o->y, o->s, o->n); delete_line(o->y); return 0;
    }
    return 0;
}

/* ^Z (undo 1) and ^Y (undo 0) */
static void undo_redo(int undo) {
    struct change c;
    int i, failed = 0;
    if(undo ? !nundo : !nredo) {
        snprintf(msg, sizeof(msg), undo ? "Nothing to undo" : "Nothing to redo (^Y does again what ^Z undid)");
        return;
    }
    if(undo) {
        c = undos[--nundo];
        memset(&undos[nundo], 0, sizeof(c));
    } else {
        c = redos[--nredo];
        memset(&redos[nredo], 0, sizeof(c));
    }
    for(i = 0; i < c.nops && !failed; i++)
        failed = replay(&c.ops[undo ? c.nops - 1 - i : i], undo) < 0;
    if(failed) {                                /* the text is no longer what the history says */
        free_change(&c);
        forget_history();
        snprintf(msg, sizeof(msg), "Out of memory: what ^Z could undo is forgotten");
        set_modified();
        return;
    }
    if(undo) redos[nredo++] = c;
    else undos[nundo++] = c;
    cy = undo ? c.y0 : c.y1;
    cx = undo ? c.x0 : c.x1;
    if(cy >= nlines) cy = nlines - 1;
    if(cx > lines[cy].len) cx = lines[cy].len;
    open_change = 0;
    changes++;                                  /* the text is another (a copy beside the file is not it) */
    modified = serial_now() != saved_serial;
    snprintf(msg, sizeof(msg), "%s (%d more to undo, %d to redo)", undo ? "Undone" : "Done again", nundo, nredo);
}

/* before a key: does what it changes go with the change before (a run of the same keys, the cursor where that
   one left it)? */
static void key_begins(int kind) {
    key_kind = kind;
    key_changed = 0;
    if(!(open_change && kind != K_NONE && kind != K_OTHER && kind == open_kind && nundo &&
         cy == undos[nundo - 1].y1 && cx == undos[nundo - 1].x1)) open_change = 0;
}

/* after it: where the cursor ended, for the next key and for ^Y */
static void key_ends(void) {
    if(key_changed && nundo) {
        undos[nundo - 1].y1 = cy;
        undos[nundo - 1].x1 = cx;
    }
    if(key_kind == K_OTHER) open_change = 0;    /* an Enter, a paste: a change of its own */
}

/**** editing ****/

static void insert_char(int c) {
    char ch = (char)c;
    if(text_insert(cy, cx, &ch, 1)) return;
    record(OP_INS, cy, cx, &ch, 1);
    cx++;
    set_modified();
}

static void enter(void) {
    if(text_split(cy, cx)) return;
    record(OP_SPLIT, cy, cx, NULL, 0);
    cy++;
    cx = 0;
    set_modified();
}

/* line y gets line y + 1 appended */
static void join(int y) {
    int at = lines[y].len;
    if(text_join(y)) return;
    record(OP_JOIN, y, at, NULL, 0);
    set_modified();
}

static void backspace(void) {
    if(cx > 0) {                                /* 10/25/26: a whole character */
        int from = char_before(&lines[cy], cx);
        record(OP_DEL, cy, from, lines[cy].s + from, cx - from);
        text_delete(cy, from, cx - from);
        cx = from;
        set_modified();
    } else if(cy > 0) {
        int at = lines[cy - 1].len;
        join(cy - 1);
        if(lines[cy - 1].len >= at) { cy--; cx = at; }
    }
}

static void delete_char(void) {
    struct line *l = &lines[cy];
    if(cx < l->len) {
        int n = char_len(l, cx);
        record(OP_DEL, cy, cx, l->s + cx, n);
        text_delete(cy, cx, n);
        set_modified();
    } else if(cy < nlines - 1) {
        join(cy);
    }
}

static void cut_line(void) {
    struct line *l = &lines[cy];
    if(!cutting) {                              /* a new cut replaces what was collected before */
        while(ncut > 0) free(cut[--ncut].s);
    }
    if(ncut == cut_cap) {
        int cap = cut_cap ? cut_cap * 2 : 16;
        struct line *p = realloc(cut, (size_t)cap * sizeof(struct line));
        if(!p) { snprintf(msg, sizeof(msg), "Out of memory"); return; }
        cut = p;
        cut_cap = cap;
    }
    cut[ncut].s = malloc(l->len > 0 ? (size_t)l->len : 1);
    if(!cut[ncut].s) { snprintf(msg, sizeof(msg), "Out of memory"); return; }
    memcpy(cut[ncut].s, l->s, (size_t)l->len);
    cut[ncut].len = cut[ncut].cap = l->len;
    ncut++;
    if(nlines > 1) {
        record(OP_LINE_DEL, cy, 0, l->s, l->len);
        delete_line(cy);
    } else {                                    /* the only line: emptied */
        record(OP_DEL, 0, 0, l->s, l->len);
        lines[0].len = 0;
    }
    if(cy >= nlines) cy = nlines - 1;
    cx = 0;
    set_modified();
    snprintf(msg, sizeof(msg), "Cut %d line%s", ncut, ncut == 1 ? "" : "s");
}

static void paste(void) {
    int i;
    if(!ncut) { snprintf(msg, sizeof(msg), "Nothing cut yet (^K cuts a line)"); return; }
    for(i = 0; i < ncut; i++) {
        if(insert_line(cy, cut[i].s, cut[i].len)) break;
        record(OP_LINE_INS, cy, 0, cut[i].s, cut[i].len);
        cy++;
    }
    cx = 0;
    set_modified();
    snprintf(msg, sizeof(msg), "Pasted %d line%s", i, i == 1 ? "" : "s");
}

/**** questions on the status line ****/

/* an answer typed on the status line: 1 with Enter, 0 when cancelled (^C or Esc) */
static int ask(const char *label) {
    int k, n = 0;
    prompt_label = label;
    prompt_buf[0] = 0;
    for(;;) {
        refresh();
        k = get_key();
        if(k == '\r' || k == '\n') break;
        if(k == CTRL('c') || k == KEY_ESC || k == KEY_EOF) { prompt_label = NULL; snprintf(msg, sizeof(msg), "Cancelled"); return 0; }
        if((k == 0x7f || k == 0x08) && n > 0) {         /* 10/25/26: a whole UTF-8 character */
            do n--; while(n > 0 && ((unsigned char)prompt_buf[n] & 0xC0) == 0x80);
            prompt_buf[n] = 0;
        } else if(k >= 32 && k < 0x100 && k != 0x7f && n < (int)sizeof(prompt_buf) - 1) {
            prompt_buf[n++] = (char)k;
            prompt_buf[n] = 0;
        }
    }
    prompt_label = NULL;
    return 1;
}

/* y, n or cancel: 'y', 'n' or 0 */
static int ask_yes_no(const char *label) {
    int k;
    prompt_label = label;
    prompt_buf[0] = 0;
    for(;;) {
        refresh();
        k = get_key();
        if(k == 'y' || k == 'Y' || k == 'n' || k == 'N') { prompt_label = NULL; return tolower(k); }
        if(k == CTRL('c') || k == KEY_ESC || k == KEY_EOF) { prompt_label = NULL; snprintf(msg, sizeof(msg), "Cancelled"); return 0; }
    }
}

static int match_at(const struct line *l, int i, const char *s, int n) {
    int k;
    if(i + n > l->len) return 0;
    for(k = 0; k < n; k++) if(tolower((unsigned char)l->s[i + k]) != tolower((unsigned char)s[k])) return 0;
    return 1;
}

static void find(void) {
    char label[160];
    int n, y, i, step;
    if(last_find[0]) snprintf(label, sizeof(label), "Find [%s]: ", last_find);
    else snprintf(label, sizeof(label), "Find: ");
    if(!ask(label)) return;
    if(prompt_buf[0]) snprintf(last_find, sizeof(last_find), "%s", prompt_buf);
    if(!last_find[0]) { snprintf(msg, sizeof(msg), "Cancelled"); return; }
    n = (int)strlen(last_find);
    y = cy;
    i = cx + 1;
    for(step = 0; step <= nlines; step++) {     /* every line once, then the start of this one */
        for(; i <= lines[y].len - n; i++) {
            if(match_at(&lines[y], i, last_find, n)) {
                if(step > 0 && y <= cy) snprintf(msg, sizeof(msg), "Search wrapped");
                cy = y;
                cx = i;
                want_col = -1;
                return;
            }
        }
        y = (y + 1) % nlines;
        i = 0;
    }
    snprintf(msg, sizeof(msg), "\"%s\" not found", last_find);
}

static void go_to_line(void) {
    long n;
    char *end;
    if(!ask("Go to line: ") || !prompt_buf[0]) return;
    n = strtol(prompt_buf, &end, 10);
    if(*end || n < 1) { snprintf(msg, sizeof(msg), "Not a line number: %s", prompt_buf); return; }
    if(n > nlines) n = nlines;
    cy = (int)n - 1;
    cx = 0;
    want_col = -1;
}

/* 1 when the editor should end */
static int quit(void) {
    int a;
    if(!modified || (copied[0] && changes == copied_changes)) return 1;   /* nothing, or all in the last copy */
    a = ask_yes_no("Save changes? (y/n/^C) ");
    if(a == 'n') return 1;
    if(a == 'y') return save() >= 0;                 /* a copy beside the file will do: main says where */
    return 0;
}

/**** moving ****/

static void vertical(int dy) {
    if(want_col < 0) want_col = col_of(&lines[cy], cx);
    cy += dy;
    if(cy < 0) cy = 0;
    if(cy >= nlines) cy = nlines - 1;
    cx = index_of(&lines[cy], want_col);
}

static int handle(int k) {
    int keep_col = 0, was_cut = 0;
    key_begins(k == 0x7f || k == 0x08 ? K_BACK : k == KEY_DEL ? K_DEL : k == CTRL('k') ? K_CUT :
               k == '\r' || k == '\n' || k == CTRL('u') ? K_OTHER :
               k == '\t' || (k >= 32 && k < 0x100 && k != 0x7f) ? K_TYPE : K_NONE);
    switch(k) {
    case KEY_UP:    vertical(-1); keep_col = 1; break;
    case KEY_DOWN:  vertical(1); keep_col = 1; break;
    case KEY_PGUP:  vertical(-(text_rows > 1 ? text_rows - 1 : 1)); keep_col = 1; break;
    case KEY_PGDN:  vertical(text_rows > 1 ? text_rows - 1 : 1); keep_col = 1; break;
    case KEY_LEFT:                                      /* 10/25/26: a whole UTF-8 character */
        if(cx > 0) cx = char_before(&lines[cy], cx);
        else if(cy > 0) { cy--; cx = lines[cy].len; }
        break;
    case KEY_RIGHT:
        if(cx < lines[cy].len) cx += char_len(&lines[cy], cx);
        else if(cy < nlines - 1) { cy++; cx = 0; }
        break;
    case KEY_HOME:  cx = 0; break;
    case KEY_END:   cx = lines[cy].len; break;
    case KEY_DEL:   delete_char(); break;
    case 0x7f:
    case 0x08:      backspace(); break;
    case '\r':
    case '\n':      enter(); break;
    case CTRL('s'): save(); break;
    case CTRL('q'): if(quit()) return 1; break;
    case CTRL('f'): find(); break;
    case CTRL('k'): cut_line(); was_cut = 1; break;
    case CTRL('u'): paste(); break;
    case CTRL('g'): go_to_line(); break;
    case CTRL('z'): undo_redo(1); break;               /* 10/25/26 (A9) */
    case CTRL('y'): undo_redo(0); break;
    case CTRL('c'):
        snprintf(msg, sizeof(msg), "Line %d of %d, column %d", cy + 1, nlines, col_of(&lines[cy], cx) + 1);
        break;
    case CTRL('l'): screen_size(); free(shadow); free(shadow_inv); shadow = NULL; shadow_ok = 0; break;
    case KEY_EOF:                                       /* no more input: nothing to ask with */
        return 2;
    default:
        if(k == '\t' || (k >= 32 && k < 0x100 && k != 0x7f)) insert_char(k);
        break;
    }
    if(!keep_col) want_col = -1;
    cutting = was_cut;
    key_ends();
    return 0;
}

int main(int argc, char **argv) {
    int r;
    if(argc != 2) { fprintf(stderr, "usage: edit <file>\n"); return 2; }
    filename = argv[1];
    if(!isatty(0) || !isatty(1)) { fprintf(stderr, "edit: needs the console for input and output\n"); return 1; }
    if(load(filename) < 0) {
        fprintf(stderr, "edit: %s: %s%s\n", filename, strerror(errno), errno == EFBIG ? " (the limit is 1 MB)" : "");
        return 1;
    }
    look_beside();
    screen_size();
    r = ttyctl(TTY_CAPS, 0);                    /* 10/25/26: UTF-8 drawn as it is where the console draws it */
    utf8 = r > 0 && (r & TTY_CAP_UTF8);
    r = ttyctl(TTY_RAW, 1);
    if(r < 0) {
        fprintf(stderr, "edit: the console has no raw mode (%s)\n", strerror(errno));
        return 1;
    }
    raw_mode = 1;
    write(1, "\x1b[?1049h", 8);            /* 10/25/26 (A9): the alternate screen; the first refresh clears it */
    atexit(restore_terminal);               /* every way out goes through exit() */
    for(;;) {
        if(!shadow) {
            shadow = calloc((size_t)rows, (size_t)ROW_BYTES);
            shadow_inv = calloc((size_t)rows, 1);
            if(!shadow || !shadow_inv) {
                free(shadow);
                free(shadow_inv);
                restore_terminal();
                fprintf(stderr, "edit: out of memory\n");
                _exit(1);
            }
            shadow_ok = 0;
        }
        refresh();
        r = get_key();
        msg[0] = 0;
        r = handle(r);
        if(r) break;
    }
    restore_terminal();
    if(copied[0])                               /* the text went beside the file, not into it */
        fprintf(stderr, "edit: %s was not saved over (check the disk); the last copy is in %s\n", filename, copied);
    if(r == 2) {
        fprintf(stderr, "edit: the input ended%s\n", modified ? "; the changes were not saved" : "");
        return 1;
    }
    return 0;
}
