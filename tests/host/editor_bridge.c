/* The bridge of tests/host/test_editor.c (10/25/26): surfsh's line editor and Tab, user/surfsh/editor.c and
   complete.c as they are (with parse.c and util.c, which they use), built for the host against SurfOS's user
   headers with the C library's and the system calls' names (tests/host/libsurf_names.h, sim_names.h), and what
   they need of the rest of surfsh and of the system:
   - a terminal: the test queues what each read answers, and the bytes the editor writes go through a small VT100
     (the ANSI subset of include/surfos/tty.h it uses: CR, LF, ESC[nA/B/C/D, ESC[K, ESC[2J, ESC[H, the console's
     pending wrap at the last column; SGR and ESC[?25h read and dropped) whose screen the test reads back, also as
     it was when the editor waited for a key (snapshots);
   - a prompt of "$ ", "> " to go on with a command (shell/prompt.c has tests of its own);
   - surfsh's commands for Tab: echo exit export history, the kernel's lsblk lspci, the programs cat cp; and a
     directory "." of notes.txt, news/, src/ (src/ holding main.c and Makefile).
   The test reaches it through tests/host/editor_sim.h. */
#include "../../user/surfsh/surfsh.h"
#include <surfos/prompt.h>
#include "editor_sim.h"

struct settings settings = { 1, 0, 5 };
int last_status, term_fd = 1, errno;
FILE *stdout;                                   /* fflush(stdout) before a plain line: nothing to flush */

/**** the screen ****/

#define COLS_MAX 200
#define ROWS_MAX 60
#define SNAPS    8

static char cell[ROWS_MAX][COLS_MAX][5];        /* each cell's character, UTF-8 */
static int cols = 80, rows = 25, cx, cy, wrap, colour;
static int esc, csi_len;                        /* esc: 1 after ESC, 2 inside ESC [ */
static char csi[40];
static char ch[5];
static int ch_len, ch_need;
static char out[1 << 17];                       /* what was written, terminated */
static int out_len;
static char snap[SNAPS][ROWS_MAX][COLS_MAX * 4 + 1];
static int snap_cursor[SNAPS], snaps;

static void blank_row(int r, int from) {
    int c;
    for(c = from; c < COLS_MAX; c++) strcpy(cell[r][c], " ");
}

static void line_feed(void) {
    if(cy < rows - 1) {
        cy++;
        return;
    }
    memmove(cell[0], cell[1], sizeof(cell[0]) * (size_t)(rows - 1));
    blank_row(rows - 1, 0);
}

static void put_char(const char *s, int n) {
    if(wrap) {                                  /* the pending wrap: this character goes on the next row */
        cx = 0;
        line_feed();
        wrap = 0;
    }
    memcpy(cell[cy][cx], s, (size_t)n);
    cell[cy][cx][n] = 0;
    if(cx == cols - 1) wrap = 1;
    else cx++;
}

static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static void do_csi(char final) {
    int n = atoi(csi[0] == '?' ? csi + 1 : csi), c;
    if(csi[0] == '?') return;                   /* ESC[?25h: the cursor shown */
    if(n < 1) n = 1;
    wrap = 0;
    switch(final) {
    case 'A': cy = clamp(cy - n, 0, rows - 1); break;
    case 'B': cy = clamp(cy + n, 0, rows - 1); break;
    case 'C': cx = clamp(cx + n, 0, cols - 1); break;
    case 'D': cx = clamp(cx - n, 0, cols - 1); break;
    case 'H': cx = cy = 0; break;               /* only ESC[H, home, is written */
    case 'K': blank_row(cy, cx); break;
    case 'J':
        for(c = 0; c < ROWS_MAX; c++) blank_row(c, 0);
        break;
    }
}

static void vt_byte(unsigned char b) {
    if(esc == 1) {
        esc = b == '[' ? 2 : 0;
        csi_len = 0;
        return;
    }
    if(esc == 2) {
        if(b >= 0x40 && b <= 0x7e) {
            csi[csi_len] = 0;
            esc = 0;
            if(b != 'm') do_csi((char)b);       /* colours: dropped */
        } else if(csi_len < (int)sizeof(csi) - 1) {
            csi[csi_len++] = (char)b;
        }
        return;
    }
    if(ch_need) {                               /* the rest of a UTF-8 character */
        ch[ch_len++] = (char)b;
        if(ch_len == ch_need) {
            put_char(ch, ch_len);
            ch_need = 0;
        }
        return;
    }
    if(b == 0x1b) {
        esc = 1;
    } else if(b == '\r') {
        cx = 0;
        wrap = 0;
    } else if(b == '\n') {
        line_feed();
        wrap = 0;
    } else if(b >= 0xc0) {
        ch[0] = (char)b;
        ch_len = 1;
        ch_need = b >= 0xf0 ? 4 : b >= 0xe0 ? 3 : 2;
    } else if(b >= 0x20 && b != 0x7f) {
        ch[0] = (char)b;
        put_char(ch, 1);
    }
}

static void row_text(int r, char *buf) {
    int c, n = 0, end = 0;
    for(c = 0; c < cols; c++) {
        int k = (int)strlen(cell[r][c]);
        memcpy(buf + n, cell[r][c], (size_t)k);
        n += k;
        if(strcmp(cell[r][c], " ")) end = n;
    }
    buf[end] = 0;
}

/**** the keys ****/

#define READS 96

static struct { char bytes[600]; int len, at_ms, snapshot; } reads[READS];
static int nreads, next_read, read_pos, skips;
static unsigned int clock_ms = 1000;

void ed_reset(int c, int r, int col) {
    int i;
    cols = c;
    rows = r;
    colour = col;
    for(i = 0; i < ROWS_MAX; i++) blank_row(i, 0);
    cx = cy = wrap = esc = ch_need = 0;
    out_len = 0;
    out[0] = 0;
    nreads = next_read = read_pos = skips = snaps = 0;
}

void ed_keysn(const char *s, int n, int at_ms) {
    if(nreads == READS || n > (int)sizeof(reads[0].bytes)) for(;;) __builtin_trap();
    memcpy(reads[nreads].bytes, s, (size_t)n);
    reads[nreads].len = n;
    reads[nreads].at_ms = at_ms;
    reads[nreads].snapshot = 0;
    nreads++;
}

void ed_keys(const char *s) {
    ed_keysn(s, (int)strlen(s), -1);
}

void ed_snapshot(void) {
    if(nreads == READS) for(;;) __builtin_trap();
    reads[nreads].len = 0;
    reads[nreads].snapshot = 1;
    nreads++;
}

static void take_snapshot(void) {
    int r;
    if(snaps == SNAPS) return;
    for(r = 0; r < rows; r++) row_text(r, snap[snaps][r]);
    snap_cursor[snaps] = cy * 1000 + cx;
    snaps++;
}

ssize_t read(int fd, void *buf, size_t size) {
    int n;
    if(fd != 0) return -1;
    while(next_read < nreads && reads[next_read].snapshot) {
        take_snapshot();
        next_read++;
    }
    if(next_read == nreads) return 0;           /* no more keys: the end of the input */
    if(reads[next_read].at_ms >= 0 && !read_pos) clock_ms = (unsigned int)reads[next_read].at_ms;
    n = reads[next_read].len - read_pos;
    if(n > (int)size) n = (int)size;
    memcpy(buf, reads[next_read].bytes + read_pos, (size_t)n);
    read_pos += n;
    if(read_pos == reads[next_read].len) {
        next_read++;
        read_pos = 0;
    }
    return n;
}

ssize_t write(int fd, const void *buf, size_t n) {
    const unsigned char *b = buf;
    size_t i;
    if(fd != 1 && fd != 2) return (ssize_t)n;
    for(i = 0; i < n; i++) {
        vt_byte(b[i]);
        if(out_len < (int)sizeof(out) - 1) out[out_len++] = (char)b[i];
    }
    out[out_len] = 0;
    return (ssize_t)n;
}

int ttyctl(int op, unsigned int arg) {
    (void)arg;
    if(op == TTY_GETSIZE) return (cols << 16) | rows;
    if(op == TTY_SKIP) skips++;
    return 0;
}

unsigned int uptime(void) {
    return clock_ms;
}

int fflush(FILE *f) {
    (void)f;
    return 0;
}

void *sbrk(intptr_t incr) {
    static char heap[8 << 20];
    static size_t used;
    if(incr < 0 || used + (size_t)incr > sizeof(heap)) return (void *)-1;
    used += (size_t)incr;
    return heap + used - incr;
}

void _exit(int code) {
    (void)code;
    for(;;) __builtin_trap();
}

/**** the rest of surfsh, and the system ****/

char *getcwd(char *buf, size_t size) {
    snprintf(buf, size, "/");
    return buf;
}

pid_t getpid(void) {
    return 7;
}

int procinfo(int index, struct procinfo *pi) {
    (void)index;
    (void)pi;
    return -1;                                  /* no terminal of a telnet session */
}

const char *var_get(const char *name) {         /* 10/25/26 (A10): HOSTNAME for the prompt: none here */
    (void)name;
    return NULL;
}

int readline(char *buf, size_t size) {
    (void)buf;
    (void)size;
    return -1;
}

int prompt_render(const struct prompt_ctx *c, char *buf, int size) {
    return snprintf(buf, (size_t)size, "%s", c->continuation ? "> " : "$ ");
}

int prompt_columns(const char *rendered) {
    return display_cols(rendered, (int)strlen(rendered));
}

int use_unicode(void) {
    return 1;
}

int use_color(void) {
    return colour;
}

int display_cols(const char *s, int n) {
    int i, c = 0;
    for(i = 0; i < n; i++) if(((unsigned char)s[i] & 0xC0) != 0x80) c++;
    return c;
}

const struct builtin builtins[] = {
    { "echo", "", "", NULL, 0 }, { "exit", "", "", NULL, 1 }, { "export", "", "", NULL, 1 }, { "history", "", "", NULL, 0 },
};
const int nbuiltins = 4;

static struct kcmd_info kcmds[2] = { { "lsblk", "", "" }, { "lspci", "", "" } };

int kcmd_count(void) {
    return 2;
}

const struct kcmd_info *kcmd_entry(int i) {
    return &kcmds[i];
}

static void add_cand(struct cands *c, const char *name, int dir) {
    if(c->n == c->cap) {
        c->cap = c->cap ? 2 * c->cap : 8;
        c->v = realloc(c->v, (size_t)c->cap * sizeof(c->v[0]));
    }
    c->v[c->n].name = strdup(name);
    c->v[c->n].dir = dir;
    c->n++;
}

int program_names(struct cands *c) {
    add_cand(c, "cat", 0);
    add_cand(c, "cp", 0);
    return 0;
}

/* opendir() and readdir() over "." (notes.txt news/ src/) and src/ (main.c Makefile) */
static const char *const top[] = { "notes.txt", "news/", "src/", NULL };
static const char *const src[] = { "main.c", "Makefile", NULL };

struct _DIR {
    const char *const *names;
    int at;
    struct dirent de;
};

DIR *opendir(const char *path) {
    DIR *d;
    const char *const *names = !strcmp(path, ".") ? top : !strcmp(path, "src/") || !strcmp(path, "src") ? src : NULL;
    if(!names || !(d = malloc(sizeof(*d)))) return NULL;
    d->names = names;
    d->at = 0;
    return d;
}

struct dirent *readdir(DIR *d) {
    const char *name = d->names[d->at];
    size_t n;
    if(!name) return NULL;
    d->at++;
    n = strlen(name);
    d->de.type = name[n - 1] == '/' ? DT_DIR : DT_REG;
    if(d->de.type == DT_DIR) n--;
    memcpy(d->de.name, name, n);
    d->de.name[n] = 0;
    return &d->de;
}

int closedir(DIR *d) {
    free(d);
    return 0;
}

/**** what the test asks ****/

void ed_print(const char *s) {
    write(1, s, strlen(s));
}

int ed_line(char *buf, int size, int more) {
    return edit_line(buf, size, more);
}

const char *ed_row(int r) {
    static char text[COLS_MAX * 4 + 1];
    row_text(r, text);
    return text;
}

const char *ed_snap_row(int k, int r) {
    return k < snaps && r < rows ? snap[k][r] : "";
}

int ed_snap_cursor(int k) {
    return k < snaps ? snap_cursor[k] : -1;
}

int ed_cursor(void) {
    return cy * 1000 + cx;
}

const char *ed_output(void) {
    return out;
}

int ed_skips(void) {
    return skips;
}

int ed_pending(void) {
    return nreads - next_read;
}

void ed_history_add(const char *line) {
    history_add(line);
}

int ed_expand(const char *in, char *buf, int size) {
    return history_expand(in, buf, size);
}

int ed_expand_in(const char *in, char *buf, int size, int *quote) {
    return history_expand_in(in, buf, size, quote);
}
