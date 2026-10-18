/*
SurfOS Desktop: the Terminal's Screen
--------------------
File: guivt.c Date: 10/17/26
--------------------
The terminal of the desktop's Terminal (shell/guiterm.c): what the programs of a session print
on its pseudo-terminal, drawn into a screen of cells the way the console draws it
(kernel/console.c), so that surfsh's prompt and line editor, edit, the pager and the kernel's
commands look as they do on F1-F4. A pseudo-terminal's console passes on what is printed as it
was printed, for a terminal at the far end to draw, and follows that terminal's cursor column to
count the pager's rows ("stream consoles" in console.c). This is that terminal:

  * the ANSI subset of include/surfos/tty.h: ESC[H and ESC[row;colH (or f), ESC[nA/B/C/D, and
    ESC[nG, which the stream console follows too; ESC[J ESC[0J ESC[1J ESC[2J and the same of K,
    blanks in the colours in force, as on the console; ESC[m with 0 (plain), 1 (bright), 7
    (inverse), 30-37 and 40-47; 27 (inverse off), which the pager's question ends with in a
    stream, and 22, 39 and 49 and the bright colours 90-97 and 100-107 of other terminals; the
    256 and true colours of 38 and 48 are passed over whole; ESC[?25l and ESC[?25h hide and show
    the cursor;
  * anything else is read to its end and dropped, as the console's parser does, in the same
    states: a CSI sequence to its final byte, an ESC ] or ESC P string to BEL or ESC (at most 256
    bytes), ESC and one more byte; a control character ends a sequence and is one again;
  * the VT100 wrap: writing the last column leaves the cursor there, and the next character goes
    to the start of the next row; CR, a cursor move or an erase calls it off;
  * LF starts the next row at its first column (the pty passes it as it is), and after a full
    row it is one row, as on any terminal; at the bottom the screen moves up a line and its top
    line goes into the scrollback. CR goes to the first column, Backspace one to the left (the
    stream console erases with BS SP BS), Tab to the next multiple of 8, never past the last
    column nor at a pending wrap (as the stream console counts it); other control characters are
    dropped, as on the screen;
  * UTF-8 as the console decodes it (utf8_feed(), driver/cp437.c): a character is its code page
    437 cell, one the set lacks is '?', a byte that is not UTF-8 is the 437 character it is.

The colours are the console's attribute bytes; the window draws them in a palette of its own
(guiterm.c). A line that goes on in the next one (a wrap) is marked, so that copied text joins
them again. A new size does not reflow: lines are cut or padded. A screen that loses rows loses
those below the cursor first, then its top ones go into the scrollback; one that gains rows takes
lines back from the scrollback first, then adds blank ones; so the cursor's line stays on the
screen and the text at its bottom.
10/18/26 (the review): a line's wrap mark is the width it wrapped at, so copied text (vt_text()) joins
it there whatever the width is later, and a double-click's word (vt_word()) follows it into the next
line. A new size keeps a mark only while its line is whole: a screen row that loses cells beyond the
new width loses its mark, and so does the last line kept when the rows below the cursor go. The cells
written are counted (written: put() one, an erase or a new line each cell it blanks), and vt_take()
feeds bytes only while a budget of them lasts: a few bytes of erases can write a screenful each.
*/
#include "guivt.h"
#include <string.h>

enum { VT_NONE, VT_SEEN, VT_CSI, VT_STRING };   /* the console's states (ESC_NONE ...) */
#define VT_STRING_MAX 256

static const u8 ansi_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };      /* ANSI colour order -> VGA attribute order */

static int clamp(int n, int a, int b) { return n < a ? a : n > b ? b : n; }

/**** lines ****/

/* the place in the ring of line i of those kept */
static int place(const struct gui_vt *vt, int i) {
    return (int)((vt->first + (u32)i) % GUI_VT_LINES);
}

const struct gui_vt_cell *vt_line(const struct gui_vt *vt, int i) {
    return vt->cell[place(vt, i)];
}

int vt_wrapped(const struct gui_vt *vt, int i) {
    return vt->wrapped[place(vt, i)];
}

/* the screen's row y */
static struct gui_vt_cell *row(struct gui_vt *vt, int y) {
    return vt->cell[place(vt, vt_screen(vt) + y)];
}

/* the screen's row y goes on in the next row from column width (0: it does not) */
static void set_wrapped(struct gui_vt *vt, int y, int width) {
    vt->wrapped[place(vt, vt_screen(vt) + y)] = (u8)width;
}

static void blank(struct gui_vt_cell *c, int from, int to, u8 attr) {
    for(; from < to; from++) {
        c[from].ch = ' ';
        c[from].attr = attr;
    }
}

/* a blank line after those kept; the oldest goes when the ring is full */
static void push_line(struct gui_vt *vt) {
    int i;
    if(vt->count < GUI_VT_LINES) i = vt->count++;
    else {
        vt->first = (vt->first + 1) % GUI_VT_LINES;
        vt->dropped++;
        i = vt->count - 1;
    }
    blank(vt->cell[place(vt, i)], 0, GUI_VT_COLS, VT_PLAIN);
    vt->wrapped[place(vt, i)] = 0;
    vt->written += GUI_VT_COLS;
}

void vt_reset(struct gui_vt *vt, int cols, int rows) {
    int i;
    memset(vt, 0, sizeof(*vt));
    vt->cols = clamp(cols, 1, GUI_VT_COLS);
    vt->rows = clamp(rows, 1, GUI_VT_ROWS);
    for(i = 0; i < vt->rows; i++) push_line(vt);
}

/**** the cursor and the cells ****/

/* the attribute a character is drawn in, as the console's ansi_attr() makes it for its own colours */
static u8 attr(const struct gui_vt *vt) {
    u8 fg, bg, t;
    if(!vt->fg && !vt->bg && !vt->bold && !vt->inverse) return VT_PLAIN;
    fg = vt->fg ? vt->fg - 1 : 7;
    bg = vt->bg ? vt->bg - 1 : 0;
    if(vt->bold) fg |= 0x08;
    if(vt->inverse) { t = fg; fg = bg; bg = t & 0x07; }    /* a text mode background has no bright bit */
    return (u8)(bg << 4 | fg);
}

/* the cursor moved by a sequence: on the screen, and a pending wrap is off */
static void moveto(struct gui_vt *vt, int x, int y) {
    vt->x = clamp(x, 0, vt->cols - 1);
    vt->y = clamp(y, 0, vt->rows - 1);
    vt->wrap = false;
}

/* a row down; at the bottom the screen moves up, and its top line goes into the scrollback */
static void down(struct gui_vt *vt) {
    if(vt->y + 1 < vt->rows) vt->y++;
    else push_line(vt);
}

static void newline(struct gui_vt *vt) {
    vt->wrap = false;
    vt->x = 0;
    down(vt);
}

/* a character at the cursor, the VT100 way (the console's put_char()) */
static void put(struct gui_vt *vt, u8 ch) {
    struct gui_vt_cell *c;
    if(vt->wrap) {
        set_wrapped(vt, vt->y, vt->cols);
        newline(vt);
    }
    c = &row(vt, vt->y)[vt->x];
    c->ch = ch;
    c->attr = attr(vt);
    vt->written++;
    if(vt->x + 1 < vt->cols) vt->x++;
    else vt->wrap = true;
}

/* the screen's cells [from, to), counted row by row, become blanks in the colours in force (ansi_blank()) */
static void erase(struct gui_vt *vt, int from, int to) {
    u8 a = attr(vt);
    int y, x0, x1;
    for(y = from / vt->cols; y < vt->rows && y * vt->cols < to; y++) {
        x0 = from > y * vt->cols ? from - y * vt->cols : 0;
        x1 = to < (y + 1) * vt->cols ? to - y * vt->cols : vt->cols;
        blank(row(vt, y), x0, x1, a);
        vt->written += (u32)(x1 - x0);
        if(x1 == vt->cols) set_wrapped(vt, y, 0);          /* its end is blank: it goes on no more */
    }
}

static void sgr(struct gui_vt *vt) {
    int i, p;
    for(i = 0; i <= vt->nparam && i < GUI_VT_PARAMS; i++) {
        p = vt->param[i];
        if(p == 0) { vt->fg = vt->bg = 0; vt->bold = vt->inverse = false; }
        else if(p == 1) vt->bold = true;
        else if(p == 7) vt->inverse = true;
        else if(p >= 30 && p <= 37) vt->fg = ansi_vga[p - 30] + 1;
        else if(p >= 40 && p <= 47) vt->bg = ansi_vga[p - 40] + 1;
        else if(p == 22) vt->bold = false;
        else if(p == 27) vt->inverse = false;
        else if(p == 39) vt->fg = 0;
        else if(p == 49) vt->bg = 0;
        else if(p >= 90 && p <= 97) vt->fg = ansi_vga[p - 90] + 9;     /* bright, whatever 1 says */
        else if(p >= 100 && p <= 107) vt->bg = ansi_vga[p - 100] + 1;  /* no bright background, as in text mode */
        else if((p == 38 || p == 48) && i + 1 <= vt->nparam && i + 1 < GUI_VT_PARAMS)
            i += vt->param[i + 1] == 5 ? 2 : vt->param[i + 1] == 2 ? 4 : 0;   /* 5;n or 2;r;g;b: not drawn */
        /* anything else (underline, blink, ...) is not drawn, as on the console */
    }
}

/* a whole CSI sequence has arrived (the console's ansi_csi()) */
static void csi(struct gui_vt *vt, int final) {
    int n = vt->param[0] ? vt->param[0] : 1;           /* a count of 0 means 1 */
    int here = vt->y * vt->cols + vt->x, line = vt->y * vt->cols;
    if(vt->mark == '?') {                               /* only ESC [ ? 25 l / h */
        if((final == 'l' || final == 'h') && vt->nparam == 0 && vt->param[0] == 25) vt->cursor_off = final == 'l';
        return;
    }
    if(vt->mark) return;
    switch(final) {
    case 'H':
    case 'f':
        moveto(vt, (vt->nparam >= 1 && vt->param[1] ? vt->param[1] : 1) - 1, n - 1);
        break;
    case 'A': moveto(vt, vt->x, vt->y - n); break;
    case 'B': moveto(vt, vt->x, vt->y + n); break;
    case 'C': moveto(vt, vt->x + n, vt->y); break;
    case 'D': moveto(vt, vt->x - n, vt->y); break;
    case 'G': moveto(vt, n - 1, vt->y); break;          /* the column (the stream console counts it) */
    case 'J':                                           /* erase in display: 0 below, 1 above, 2 all */
        vt->wrap = false;
        if(vt->param[0] == 0) erase(vt, here, vt->cols * vt->rows);
        else if(vt->param[0] == 1) erase(vt, 0, here + 1);
        else if(vt->param[0] == 2) erase(vt, 0, vt->cols * vt->rows);
        break;
    case 'K':                                           /* erase in line: 0 right, 1 left, 2 all */
        vt->wrap = false;
        if(vt->param[0] == 0) erase(vt, here, line + vt->cols);
        else if(vt->param[0] == 1) erase(vt, line, here + 1);
        else if(vt->param[0] == 2) erase(vt, line, line + vt->cols);
        break;
    case 'm':
        sgr(vt);
        break;
    }
}

/* One more byte while a sequence is open (the console's ansi_byte()). 1: it belonged to the
   sequence; 0: it ended the sequence without belonging to it, and is a byte of its own. */
static int esc_byte(struct gui_vt *vt, u8 c) {
    switch(vt->esc) {
    case VT_STRING:
        if(c == 0x07 || ++vt->param[0] >= VT_STRING_MAX) vt->esc = VT_NONE;
        else if(c == 0x1B) vt->esc = VT_SEEN;
        return 1;
    case VT_SEEN:
        if(c == '[') {
            vt->esc = VT_CSI;
            vt->mark = 0;
            vt->nparam = 0;
            vt->param[0] = 0;
            return 1;
        }
        if(c == ']' || c == 'P' || c == '_' || c == '^' || c == 'X') {
            vt->esc = VT_STRING;
            vt->param[0] = 0;
            return 1;
        }
        if(c == 0x1B || (c >= 0x20 && c <= 0x2F)) return 1;      /* ESC again, or ESC ( B and the like */
        vt->esc = VT_NONE;
        return c >= 0x30 && c < 0x80;                           /* ESC 7, ESC c, ...: dropped; a UTF-8 byte is printed */
    case VT_CSI:
        if(c >= '0' && c <= '9') {
            if(vt->nparam < GUI_VT_PARAMS && vt->param[vt->nparam] < 1000)
                vt->param[vt->nparam] = (u16)(vt->param[vt->nparam] * 10 + (c - '0'));
            return 1;
        }
        if(c == ';' || c == ':') {
            if(vt->nparam < GUI_VT_PARAMS && ++vt->nparam < GUI_VT_PARAMS) vt->param[vt->nparam] = 0;
            return 1;
        }
        if(c >= 0x3C && c <= 0x3F) {                              /* < = > ?: a private sequence */
            if(!vt->mark) vt->mark = c;
            return 1;
        }
        if(c >= 0x20 && c <= 0x2F) {                              /* intermediate bytes: not one of ours */
            vt->mark = 1;
            return 1;
        }
        if(c >= 0x40 && c <= 0x7E) {
            vt->esc = VT_NONE;
            csi(vt, c);
            return 1;
        }
        if(c == 0x1B) { vt->esc = VT_SEEN; return 1; }
        if(c < 0x20) { vt->esc = VT_NONE; return 0; }
        return 1;                                                 /* DEL, bytes above 0x7E */
    }
    vt->esc = VT_NONE;
    return 0;
}

/* one byte the session printed (the console's kputch()) */
static void byte(struct gui_vt *vt, u8 b) {
    u8 cells[UTF8_OUT_MAX];
    int n, i;
    if(vt->esc && esc_byte(vt, b)) return;
    if(b >= 0x80 || vt->utf.have) {                     /* UTF-8; a byte below 0x80 that cuts one short comes last */
        n = utf8_feed(&vt->utf, b, cells);
        if(b < 0x80) n--;
        for(i = 0; i < n; i++) put(vt, cells[i]);
        if(b >= 0x80) return;
    }
    switch(b) {
    case 0x1B:
        vt->esc = VT_SEEN;
        break;
    case '\n':
        newline(vt);
        break;
    case '\r':
        vt->wrap = false;
        vt->x = 0;
        break;
    case 0x08:
        if(vt->x > 0) vt->x--;
        vt->wrap = false;
        break;
    case '\t':
        if(!vt->wrap) vt->x = (vt->x + 8) & ~7;
        if(vt->x >= vt->cols) vt->x = vt->cols - 1;
        break;
    default:
        if(b >= 0x20 && b < 0x7F) put(vt, b);           /* other control characters: nothing, as on the screen */
        break;
    }
}

void vt_feed(struct gui_vt *vt, const char *s, u32 n) {
    u32 i;
    for(i = 0; i < n; i++) byte(vt, (u8)s[i]);
}

u32 vt_take(struct gui_vt *vt, const char *s, u32 n, u32 budget) {
    u32 start = vt->written, i;
    for(i = 0; i < n && vt->written - start < budget; i++) byte(vt, (u8)s[i]);
    return i;
}

void vt_fresh(struct gui_vt *vt) {
    vt->esc = VT_NONE;
    memset(&vt->utf, 0, sizeof(vt->utf));
    vt->fg = vt->bg = 0;
    vt->bold = vt->inverse = vt->cursor_off = false;
    if(vt->x || vt->wrap) newline(vt);
}

void vt_resize(struct gui_vt *vt, int cols, int rows) {
    int k, y, keep = vt->cols;
    cols = clamp(cols, 1, GUI_VT_COLS);
    rows = clamp(rows, 1, GUI_VT_ROWS);
    if(rows < vt->rows) {
        int cut = vt->rows - rows, below = vt->rows - 1 - vt->y, drop = cut < below ? cut : below;
        vt->count -= drop;                              /* the rows below the cursor go first, */
        vt->y -= cut - drop;                            /* then the top ones into the scrollback */
        if(drop) vt->wrapped[place(vt, vt->count - 1)] = 0;    /* 10/18/26: what it went on in is gone */
    } else if(rows > vt->rows) {
        int more = rows - vt->rows, back = vt->count - vt->rows, pull = more < back ? more : back;
        vt->y += pull;                                  /* lines come back from the scrollback first */
        for(k = pull; k < more; k++) push_line(vt);
    }
    vt->rows = rows;
    if(cols < keep) keep = cols;
    for(y = 0; y < rows; y++) {                         /* nothing beyond the width shows */
        blank(row(vt, y), keep, GUI_VT_COLS, VT_PLAIN);
        if(vt->wrapped[place(vt, vt_screen(vt) + y)] > keep) set_wrapped(vt, y, 0);   /* 10/18/26: cut, not whole */
    }
    vt->cols = cols;
    if(vt->x >= cols) {                                 /* as the stream console has it (console_stream_resize()) */
        vt->x = cols - 1;
        vt->wrap = false;
    }
    vt->y = clamp(vt->y, 0, rows - 1);
}

/**** text ****/

static bool blank_cell(const struct gui_vt_cell *c) { return c->ch == ' ' || c->ch == 0; }

/* where line i's text ends: at the column it wrapped at, or after its last character, the cells beyond the
   screen's width counted (a line kept from a wider screen has them) */
static int line_end(const struct gui_vt *vt, int i) {
    const struct gui_vt_cell *c = vt_line(vt, i);
    int n = vt_wrapped(vt, i);
    if(n) return n;
    for(n = GUI_VT_COLS; n > 0 && blank_cell(&c[n - 1]); n--) ;
    return n;
}

u32 vt_text(const struct gui_vt *vt, int l0, int c0, int l1, int c1, char *out, u32 size, bool *cut) {
    char ch[CP437_UTF8_MAX];
    u32 n = 0, owed = 0;                                /* owed: line ends, written before the next character */
    int i, a, b, k, len;
    bool joined;
    *cut = false;
    if(l0 < 0) { l0 = 0; c0 = 0; }
    if(l1 >= vt->count) { l1 = vt->count - 1; c1 = vt->cols; }
    for(i = l0; i <= l1; i++) {
        const struct gui_vt_cell *c = vt_line(vt, i);
        joined = i < l1 && vt_wrapped(vt, i);           /* it goes on in the next line: no line end, blanks kept */
        a = i == l0 ? clamp(c0, 0, GUI_VT_COLS) : 0;
        b = i < l1 || c1 >= vt->cols ? line_end(vt, i) : clamp(c1, 0, GUI_VT_COLS);
        if(!joined) while(b > a && blank_cell(&c[b - 1])) b--;
        for(k = a; k < b; k++) {
            for(; owed; owed--) {
                if(n + 1 > size) { *cut = true; return n; }
                out[n++] = '\n';
            }
            len = cp437_utf8(c[k].ch, ch);
            if(n + (u32)len > size) { *cut = true; return n; }
            memcpy(out + n, ch, len);
            n += len;
        }
        if(i < l1 && !joined) owed++;
    }
    if(n && owed) {                                     /* blank lines at the end: one line end; nothing but */
        if(n + 1 > size) *cut = true;                   /* blanks: nothing at all (10/18/26) */
        else out[n++] = '\n';
    }
    return n;
}

static bool word_cell(u8 ch) {
    return ch != ' ' && ch != 0 && !strchr("\"'`()<>[]{},;|", ch);
}

void vt_word(const struct gui_vt *vt, int i, int col, int *l0, int *c0, int *l1, int *c1) {
    int l, x, w;
    *l0 = *l1 = i;
    *c0 = *c1 = clamp(col, 0, GUI_VT_COLS);
    if(i < 0 || i >= vt->count || col < 0 || col >= GUI_VT_COLS) return;
    if(!word_cell(vt_line(vt, i)[col].ch)) { *c1 = col + 1; return; }
    for(l = i, x = col;;) {                             /* to its start, back across the wraps before it */
        while(x > 0 && word_cell(vt_line(vt, l)[x - 1].ch)) x--;
        if(x > 0 || l == 0 || !(w = vt_wrapped(vt, l - 1)) || !word_cell(vt_line(vt, l - 1)[w - 1].ch)) break;
        l--;
        x = w - 1;
    }
    *l0 = l;
    *c0 = x;
    for(l = i, x = col + 1;;) {                         /* to its end, on across the wraps after it */
        w = vt_wrapped(vt, l);
        while(x < (w ? w : GUI_VT_COLS) && word_cell(vt_line(vt, l)[x].ch)) x++;
        if(x < w || !w || l + 1 >= vt->count || !word_cell(vt_line(vt, l + 1)[0].ch)) break;
        l++;
        x = 0;
    }
    *l1 = l;
    *c1 = x;
}
