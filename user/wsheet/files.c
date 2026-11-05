/*
SurfOS user program: wsheet
--------------------
File: files.c   Date: 11/05/26
--------------------
The Spreadsheet's files: its own text format, which keeps everything, and CSV, which keeps the
values. Both are read a piece at a time (main.c reads 64 KB at a go) into a new sheet, so the
sheet on the screen stays as it was until a file has been read whole; every count and length is
bounded, and a file that breaks a rule is refused with the line or field that broke it. A sheet
is written through a callback, which main.c points at saveutil.h's new file.

The sheet's own format, "SurfOS sheet" (.sheet), is UTF-8 text, a record a line, a line end LF
(CRLF read too):

    SurfOS sheet 1
    width B 120
    cell A1 general Total
    cell B1 fixed2 =SUM(B2:B9)
    cell C4 percent1

- The first line names the format and its version; a newer version is refused, saying so.
- "width <column> <pixels>" gives a column a width other than the default 80 (16 to 800).
- "cell <name> <format> <content>": the format is general, fixed0 to fixed10 or percent0 to
  percent10; the content is what the formula bar shows, which typed again makes the same cell:
  a formula with its =, a number in the fewest digits that give its value exactly, TRUE, FALSE,
  an error, or text, with a ' in front when it would otherwise be read as one of those. A cell
  with no content keeps a format alone. In the content a backslash begins \\, \n, \r, \t or \xHH
  (any other byte below 32, and 127); everything else is as it is.
- Blank lines and lines that begin with # are skipped. A cell given twice is refused.
The writer writes the widths, then the cells row by row.

CSV is RFC 4180's: fields separated by commas, records by line ends, a field in double quotes
when it holds a comma, a quote (doubled) or a line end. Reading takes CRLF, LF or CR alone, a
UTF-8 byte order mark at the start, and is lenient where RFC 4180 has no answer (a quote inside a
field that does not begin with one, text after a closing quote: kept as they are). A field that is
a number as JSON (RFC 8259) writes one becomes that number, so 007, 1,000 and .5 stay text as
they were; TRUE and FALSE become truth values; a field that begins with = is text, never a
formula. Saving writes the values (a formula's result) from A1 to the last cell that holds
something, every record with the same number of fields, each ending in CRLF; numbers in the
fewest digits that give their value exactly.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sheetpriv.h"

#define LINE_MAX_BYTES (4 * SH_INPUT_MAX + 64)  /* a line of the sheet's own format: every byte may be \xHH */
#define VERSION 1

enum { D_START, D_PLAIN, D_QUOTED, D_QUOTE, D_CR };

/**** delimited text ****/

int dsv_init(struct dsv *d, int sep, int (*cell)(void *ctx, int row, int col, const char *text, int len), void *ctx) {
    memset(d, 0, sizeof(*d));
    d->sep = sep;
    d->cell = cell;
    d->ctx = ctx;
    d->field = mem_alloc(SH_INPUT_MAX + 1);
    return d->field ? 0 : -1;
}

void dsv_free(struct dsv *d) {
    mem_free(d->field);
    d->field = NULL;
}

static int dsv_refuse(struct dsv *d, const char *why) {
    if(!d->failed) d->why = why;
    d->failed = 1;
    return -1;
}

static int field_end(struct dsv *d) {
    if(d->col >= SH_COLS) return dsv_refuse(d, "a row has more than 256 fields");
    if(d->row >= SH_ROWS) return dsv_refuse(d, "there are more than 65,536 rows");
    d->field[d->len] = 0;
    if(d->cell(d->ctx, d->row, d->col, d->field, d->len) < 0) return dsv_refuse(d, "a field was refused");
    if(d->col + 1 > d->cols) d->cols = d->col + 1;
    d->len = 0;
    d->col++;
    return 0;
}

static int record_end(struct dsv *d) {
    if(field_end(d) < 0) return -1;
    d->row++;
    d->rows = d->row;
    d->col = 0;
    return 0;
}

int dsv_feed(struct dsv *d, const char *bytes, int n) {
    int i;
    char c;
    for(i = 0; i < n; i++) {
        if(d->failed) return -1;
        c = bytes[i];
        if(!c) return dsv_refuse(d, "it has a NUL byte, so it is not text");
        switch(d->state) {
        case D_CR:
            d->state = D_START;
            if(c == '\n') continue;
            /* fall through: the CR alone ended the record */
        case D_START:
            if(c == '"') {
                d->state = D_QUOTED;
                continue;
            }
            /* fall through */
        case D_PLAIN:
            if(c == d->sep) {
                if(field_end(d) < 0) return -1;
                d->state = D_START;
            } else if(c == '\r' || c == '\n') {
                if(record_end(d) < 0) return -1;
                d->state = c == '\r' ? D_CR : D_START;
            } else {
                if(d->len >= SH_INPUT_MAX) return dsv_refuse(d, "a field is longer than 8,192 bytes");
                d->field[d->len++] = c;
                d->state = D_PLAIN;
            }
            break;
        case D_QUOTED:
            if(c == '"') d->state = D_QUOTE;
            else {
                if(d->len >= SH_INPUT_MAX) return dsv_refuse(d, "a field is longer than 8,192 bytes");
                d->field[d->len++] = c;
            }
            break;
        case D_QUOTE:                           /* "" is a quote; anything else ends the quotes */
            if(c == '"') {
                if(d->len >= SH_INPUT_MAX) return dsv_refuse(d, "a field is longer than 8,192 bytes");
                d->field[d->len++] = c;
                d->state = D_QUOTED;
            } else if(c == d->sep) {
                if(field_end(d) < 0) return -1;
                d->state = D_START;
            } else if(c == '\r' || c == '\n') {
                if(record_end(d) < 0) return -1;
                d->state = c == '\r' ? D_CR : D_START;
            } else {                            /* text after the closing quote: kept */
                if(d->len >= SH_INPUT_MAX) return dsv_refuse(d, "a field is longer than 8,192 bytes");
                d->field[d->len++] = c;
                d->state = D_PLAIN;
            }
            break;
        }
    }
    return d->failed ? -1 : 0;
}

/* the last record when no line end followed it; nothing after a last line end */
int dsv_end(struct dsv *d) {
    if(d->failed) return -1;
    if(d->state == D_CR || (d->state == D_START && d->col == 0)) return 0;
    return record_end(d);
}

/**** reading ****/

struct sh_reader {
    int kind, failed;
    char why[SH_WHY];
    long bytes;
    struct build *b;
    unsigned short width[SH_COLS];
    /* the sheet's own format */
    char *line;
    int len, lineno, header, any;
    char *content;                      /* a cell's content, unescaped */
    /* CSV */
    struct dsv d;
    int bom;                            /* bytes of a byte order mark seen at the start; -1 when past it */
};

/* the reading stops, and why says where: the line of a sheet's own file, the row of a CSV */
static int refuse(struct sh_reader *rd, const char *msg) {
    if(!rd->failed) {
        if(rd->kind == SH_NATIVE && rd->lineno) snprintf(rd->why, sizeof(rd->why), "Line %d: %s", rd->lineno, msg);
        else if(rd->kind == SH_CSV && rd->d.field)
            snprintf(rd->why, sizeof(rd->why), "Row %d of the CSV: %s", rd->d.row + 1, msg);
        else snprintf(rd->why, sizeof(rd->why), "%s", msg);
    }
    rd->failed = 1;
    return -1;
}

static int build_refused(struct sh_reader *rd, int e) {
    if(e == BUILD_FULL) return refuse(rd, "the file has more cells than a sheet keeps (300,000)");
    if(e == BUILD_TWICE) return refuse(rd, "a cell is in the file twice");
    return refuse(rd, "not enough memory for the sheet");
}

static int csv_field(void *ctx, int r, int c, const char *text, int len);

struct sh_reader *sh_read_begin(int kind) {
    struct sh_reader *rd = mem_zalloc(sizeof(*rd));
    int i;
    if(!rd) return NULL;
    rd->kind = kind;
    for(i = 0; i < SH_COLS; i++) rd->width[i] = SH_WIDTH_DEFAULT;
    rd->b = build_new();
    if(!rd->b) goto fail;
    if(kind == SH_CSV) {
        if(dsv_init(&rd->d, ',', csv_field, rd) < 0) goto fail;
    } else {
        rd->line = mem_alloc(LINE_MAX_BYTES + 1);
        rd->content = mem_alloc(SH_INPUT_MAX + 1);
        if(!rd->line || !rd->content) goto fail;
    }
    return rd;
fail:
    sh_read_cancel(rd);
    return NULL;
}

void sh_read_cancel(struct sh_reader *rd) {
    if(!rd) return;
    build_free(rd->b);
    dsv_free(&rd->d);
    mem_free(rd->line);
    mem_free(rd->content);
    mem_free(rd);
}

static int csv_field(void *ctx, int r, int c, const char *text, int len) {
    struct sh_reader *rd = ctx;
    struct content ct;
    double v;
    int e;
    if(!len) return 0;
    memset(&ct, 0, sizeof(ct));
    if(num_read_json(text, len, &v)) {
        ct.kind = K_NUMBER;
        ct.num = v;
    } else if((len == 4 && !memcmp(text, "TRUE", 4)) || (len == 5 && !memcmp(text, "FALSE", 5))) {
        ct.kind = K_BOOL;
        ct.b = len == 4;
    } else {
        ct.text = mem_dup(text, len);
        if(!ct.text) return build_refused(rd, BUILD_NOMEM);
        ct.kind = K_TEXT;
        ct.len = len;
    }
    e = build_add(rd->b, r, c, &ct);
    return e ? build_refused(rd, e) : 0;
}

static int hex(int c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* the content of a cell line, its escapes undone, into rd->content: its length, or -1 */
static int unescape(struct sh_reader *rd, const char *s, int len) {
    int i, n = 0, h, l;
    char c;
    for(i = 0; i < len; i++) {
        c = s[i];
        if(c == '\\') {
            if(++i >= len) return refuse(rd, "a \\ ends the line");
            switch(s[i]) {
            case '\\': c = '\\'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'x':
                if(i + 2 >= len) return refuse(rd, "\\x needs two hexadecimal digits");
                h = hex((unsigned char)s[i + 1]);
                l = hex((unsigned char)s[i + 2]);
                if(h < 0 || l < 0) return refuse(rd, "\\x needs two hexadecimal digits");
                c = (char)(h * 16 + l);
                if(!c) return refuse(rd, "a cell cannot hold a NUL character");
                i += 2;
                break;
            default:
                return refuse(rd, "a \\ that begins no escape");
            }
        }
        if(n >= SH_INPUT_MAX) return refuse(rd, "a cell holds 8,192 bytes at most");
        rd->content[n++] = c;
    }
    rd->content[n] = 0;
    return n;
}

/* the next word of s from *i, which ends at a space or the end: its length (0 for none) */
static int next_word(const char *s, int len, int *i, int *start) {
    int j = *i;
    *start = j;
    while(j < len && s[j] != ' ') j++;
    *i = j;
    return j - *start;
}

static int read_format(const char *s, int n, int *fmt) {
    int d = 0, k, kind;
    if(n == 7 && !memcmp(s, "general", 7)) {
        *fmt = SH_GENERAL;
        return 0;
    }
    if(n > 5 && !memcmp(s, "fixed", 5)) {
        kind = SH_FIXED(0);
        k = 5;
    } else if(n > 7 && !memcmp(s, "percent", 7)) {
        kind = SH_PERCENT(0);
        k = 7;
    } else return -1;
    if(n - k > 2 || (n - k == 2 && s[k] == '0')) return -1;
    for(; k < n; k++) {
        if(s[k] < '0' || s[k] > '9') return -1;
        d = d * 10 + (s[k] - '0');
    }
    if(d > SH_DECIMALS_MAX) return -1;
    *fmt = kind | d;
    return 0;
}

static int native_line(struct sh_reader *rd, char *s, int len) {
    char name[16];
    struct content ct;
    int i = 0, start, n, row, col, fmt, px, e;
    rd->lineno++;
    if(len && s[len - 1] == '\r') len--;
    if(!len || s[0] == '#') return 0;
    rd->any = 1;
    if(!rd->header) {
        if(len == 14 && !memcmp(s, "SurfOS sheet 1", 14)) {
            rd->header = 1;
            return 0;
        }
        if(len > 13 && !memcmp(s, "SurfOS sheet ", 13))
            return refuse(rd, "the file was made by a newer Spreadsheet: this one reads version 1");
        rd->lineno = 0;
        return refuse(rd, "This is not a SurfOS sheet: its first line is not \"SurfOS sheet 1\"");
    }
    n = next_word(s, len, &i, &start);
    if(n == 5 && !memcmp(s, "width", 5)) {
        if(i >= len) return refuse(rd, "a width needs a column and pixels");
        i++;
        n = next_word(s, len, &i, &start);
        if(n < 1 || n > 2 || i >= len) return refuse(rd, "a width needs a column and pixels");
        memcpy(name, s + start, (size_t)n);
        memcpy(name + n, "1", 2);
        if(sh_parse_cell(name, &row, &col) < 0 || memchr(name, '$', (size_t)n))
            return refuse(rd, "no such column");
        i++;
        n = next_word(s, len, &i, &start);
        if(n < 1 || n > 3 || i != len) return refuse(rd, "a width is 16 to 800 pixels");
        for(px = 0; start < i; start++) {
            if(s[start] < '0' || s[start] > '9') return refuse(rd, "a width is 16 to 800 pixels");
            px = px * 10 + (s[start] - '0');
        }
        if(px < SH_WIDTH_MIN || px > SH_WIDTH_MAX) return refuse(rd, "a width is 16 to 800 pixels");
        rd->width[col] = (unsigned short)px;
        return 0;
    }
    if(n != 4 || memcmp(s, "cell", 4)) return refuse(rd, "not a line of a sheet");
    if(i >= len) return refuse(rd, "a cell needs its name and format");
    i++;
    n = next_word(s, len, &i, &start);
    if(n < 2 || n > 7 || i >= len) return refuse(rd, "a cell needs its name and format");
    memcpy(name, s + start, (size_t)n);
    name[n] = 0;
    if(memchr(name, '$', (size_t)n) || sh_parse_cell(name, &row, &col) < 0)
        return refuse(rd, "no such cell");
    i++;
    n = next_word(s, len, &i, &start);
    if(read_format(s + start, n, &fmt) < 0) return refuse(rd, "no such format");
    n = 0;
    if(i < len) {                               /* one space, then the content to the end of the line */
        n = unescape(rd, s + i + 1, len - i - 1);
        if(n < 0) return -1;
    }
    if(content_parse(&ct, rd->content, n, fmt) < 0) return build_refused(rd, BUILD_NOMEM);
    e = build_add(rd->b, row, col, &ct);
    return e ? build_refused(rd, e) : 0;
}

static const char bom[3] = { (char)0xEF, (char)0xBB, (char)0xBF };

int sh_read(struct sh_reader *rd, const char *bytes, int n) {
    int i;
    if(rd->failed) return -1;
    rd->bytes += n;
    if(rd->bytes > SH_FILE_MAX) return refuse(rd, "The file is bigger than 32 MB");
    if(rd->kind == SH_CSV) {
        while(rd->bom >= 0 && n > 0) {          /* a byte order mark at the start is no data */
            if(bytes[0] != bom[rd->bom]) {      /* none after all: what looked like its start is data */
                int held = rd->bom;
                rd->bom = -1;
                if(held) dsv_feed(&rd->d, bom, held);
                break;
            }
            bytes++;
            n--;
            if(++rd->bom == 3) rd->bom = -1;
        }
        if(!rd->d.failed && dsv_feed(&rd->d, bytes, n) < 0 && !rd->failed)
            return refuse(rd, rd->d.why);
        return rd->failed ? -1 : 0;
    }
    for(i = 0; i < n; i++) {
        if(!bytes[i]) {
            rd->lineno++;
            return refuse(rd, "a NUL byte: this is not a sheet");
        }
        if(bytes[i] == '\n') {
            rd->line[rd->len] = 0;
            if(native_line(rd, rd->line, rd->len) < 0) return -1;
            rd->len = 0;
            continue;
        }
        if(rd->len >= LINE_MAX_BYTES) {
            rd->lineno++;
            return refuse(rd, "the line is too long");
        }
        rd->line[rd->len++] = bytes[i];
    }
    return 0;
}

struct sheet *sh_read_end(struct sh_reader *rd, char *why, int size) {
    struct sheet *s = NULL;
    if(!rd->failed) {
        if(rd->kind == SH_CSV) {
            if(rd->bom > 0) dsv_feed(&rd->d, bom, rd->bom);     /* a file that was only part of one */
            if(dsv_end(&rd->d) < 0 && !rd->failed) refuse(rd, rd->d.why);
        } else if(rd->len) {
            rd->line[rd->len] = 0;
            native_line(rd, rd->line, rd->len);
        }
        if(!rd->failed && rd->kind == SH_NATIVE && rd->any && !rd->header) {
            rd->lineno = 0;
            refuse(rd, "This is not a SurfOS sheet: its first line is not \"SurfOS sheet 1\"");
        }
    }
    if(!rd->failed) {
        s = sh_new();
        if(!s) refuse(rd, "Not enough memory for the sheet");
        else if(build_finish(rd->b, s, rd->why, sizeof(rd->why)) < 0) {
            rd->failed = 1;
            sh_free(s);
            s = NULL;
        } else {
            memcpy(s->width, rd->width, sizeof(s->width));
            recalc(s);
        }
    }
    if(!s) snprintf(why, (size_t)size, "%s", rd->why);
    sh_read_cancel(rd);
    return s;
}

int sh_kind_of(const char *path) {
    int n = (int)strlen(path);
    return n >= 4 && !strcasecmp(path + n - 4, ".csv") ? SH_CSV : SH_NATIVE;
}

/**** writing ****/

struct writer {
    int (*put)(void *ctx, const char *bytes, int n);
    void *ctx;
    char buf[4096];
    int len, failed;
};

static void flush(struct writer *w) {
    if(w->len && !w->failed && w->put(w->ctx, w->buf, w->len) < 0) w->failed = 1;
    w->len = 0;
}

static void out(struct writer *w, const char *s, int n) {
    while(n > 0 && !w->failed) {
        int k = (int)sizeof(w->buf) - w->len;
        if(k > n) k = n;
        memcpy(w->buf + w->len, s, (size_t)k);
        w->len += k;
        s += k;
        n -= k;
        if(w->len == (int)sizeof(w->buf)) flush(w);
    }
}

static void outs(struct writer *w, const char *s) {
    out(w, s, (int)strlen(s));
}

static void escaped(struct writer *w, const char *s, int n) {
    char e[8];
    int i, start = 0;
    for(i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if(c >= 32 && c != 127 && c != '\\') continue;
        out(w, s + start, i - start);
        if(c == '\\') outs(w, "\\\\");
        else if(c == '\n') outs(w, "\\n");
        else if(c == '\r') outs(w, "\\r");
        else if(c == '\t') outs(w, "\\t");
        else {
            snprintf(e, sizeof(e), "\\x%02X", c);
            outs(w, e);
        }
        start = i + 1;
    }
    out(w, s + start, n - start);
}

static void format_word(int fmt, char *out, int size) {
    if(SH_FMT_KIND(fmt) == SH_FIXED(0)) snprintf(out, (size_t)size, "fixed%d", SH_FMT_DECIMALS(fmt));
    else if(SH_FMT_KIND(fmt) == SH_PERCENT(0)) snprintf(out, (size_t)size, "percent%d", SH_FMT_DECIMALS(fmt));
    else snprintf(out, (size_t)size, "general");
}

static void write_native(const struct sheet *s, struct writer *w) {
    static char input[SH_INPUT_MAX + 64];
    char line[64], name[16], fmt[16];
    int i, j, n;
    snprintf(line, sizeof(line), "SurfOS sheet %d\n", VERSION);
    outs(w, line);
    for(i = 0; i < SH_COLS; i++) {
        if(s->width[i] == SH_WIDTH_DEFAULT) continue;
        sh_colname(i, name);
        snprintf(line, sizeof(line), "width %s %d\n", name, s->width[i]);
        outs(w, line);
    }
    for(i = 0; i < s->nrows; i++)
        for(j = 0; j < s->rows[i].n; j++) {
            const struct cell *c = &s->rows[i].cells[j];
            sh_cellname(s->rows[i].r, c->col, name);
            format_word(c->c.fmt, fmt, sizeof(fmt));
            snprintf(line, sizeof(line), "cell %s %s", name, fmt);
            outs(w, line);
            n = content_input(&c->c, input, sizeof(input));
            if(n > (int)sizeof(input) - 1) n = (int)sizeof(input) - 1;
            if(n) {
                outs(w, " ");
                escaped(w, input, n);
            }
            outs(w, "\n");
        }
}

static void csv_field_out(struct writer *w, const struct sheet *s, const struct cell *c) {
    struct sh_value v;
    char num[48];
    const char *t;
    int n, i, start;
    cell_value(s, c, &v);
    switch(v.type) {
    case SH_NUMBER: n = num_shortest(v.num, num, sizeof(num)); t = num; break;
    case SH_TEXT: t = v.text; n = v.len; break;
    case SH_BOOL: t = v.b ? "TRUE" : "FALSE"; n = v.b ? 4 : 5; break;
    case SH_ERROR: t = sh_error_name(v.b); n = (int)strlen(t); break;
    default: return;
    }
    if(!memchr(t, ',', (size_t)n) && !memchr(t, '"', (size_t)n) && !memchr(t, '\r', (size_t)n) &&
       !memchr(t, '\n', (size_t)n)) {
        out(w, t, n);
        return;
    }
    outs(w, "\"");
    for(i = start = 0; i < n; i++)
        if(t[i] == '"') {
            out(w, t + start, i + 1 - start);
            outs(w, "\"");
            start = i + 1;
        }
    out(w, t + start, n - start);
    outs(w, "\"");
}

static void write_csv(const struct sheet *s, struct writer *w) {
    int rows, cols, r, c, ri = 0, ci;
    sh_extent(s, &rows, &cols);
    for(r = 0; r < rows && !w->failed; r++) {
        const struct row *rw = NULL;
        while(ri < s->nrows && s->rows[ri].r < r) ri++;
        if(ri < s->nrows && s->rows[ri].r == r) rw = &s->rows[ri];
        for(c = 0, ci = 0; c < cols; c++) {
            if(c) outs(w, ",");
            while(rw && ci < rw->n && rw->cells[ci].col < c) ci++;
            if(rw && ci < rw->n && rw->cells[ci].col == c) csv_field_out(w, s, &rw->cells[ci]);
        }
        outs(w, "\r\n");
    }
}

int sh_write(const struct sheet *s, int kind, int (*put)(void *ctx, const char *bytes, int n), void *ctx) {
    static struct writer w;
    memset(&w, 0, sizeof(w));
    w.put = put;
    w.ctx = ctx;
    if(kind == SH_CSV) write_csv(s, &w);
    else write_native(s, &w);
    flush(&w);
    return w.failed ? -1 : 0;
}
