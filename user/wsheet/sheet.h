/*
SurfOS user program: wsheet
--------------------
File: sheet.h   Date: 11/05/26
--------------------
The Spreadsheet's model: everything of it that is not drawing or a system call, so that
tests/host/test_wsheet.c runs it on the host as it is (main.c is the window). A sheet has 256
columns (A to IV) and 65,536 rows, kept sparse: only a cell that holds something, or has a
format of its own, takes memory. A cell holds what was typed into it (a number, text, TRUE or
FALSE, an error, or a formula) and a number format. A formula's value is that of the last
recalculation, which every change ends with: the formulas in the order of what they refer to,
those in a cycle shown as #CYCLE!. Every change is one step that Undo takes back.

Rows and columns are counted from 0 here; the window shows them from 1 and from A.

The formula language, the files and the limits are described in the files that do them
(formula.c, eval.c, files.c) and in docs/GUI.md's section "Spreadsheet".
*/
#ifndef _WSHEET_SHEET_H
#define _WSHEET_SHEET_H

#define SH_COLS 256
#define SH_ROWS 65536
#define SH_INPUT_MAX 8192               /* bytes a cell holds as typed: text, or a formula with its '=' */
#define SH_TEXT_MAX 8192                /* bytes of text a formula may make */
#define SH_CELLS_MAX 300000             /* cells that hold something or have a format of their own */
#define SH_AREA_MAX 65536               /* cells a format may cover at once: a whole column */
#define SH_COPY_MAX 1048576             /* cells a copy may cover at once: 16 whole columns */
#define SH_FILE_MAX (32L * 1024 * 1024) /* bytes of a file read */
#define SH_WIDTH_DEFAULT 80             /* a column's width in pixels: ten characters of 8 */
#define SH_WIDTH_MIN 16
#define SH_WIDTH_MAX 800
#define SH_DECIMALS_MAX 10
#define SH_WHY 160                      /* a message's bytes */

/* what the model may take: memory in bytes, and the work of a recalculation in steps (an instruction, a cell
   looked at). The tests change them; sh_mem_fail makes the allocation that many from now fail (-1: none) */
#define SH_MEMORY (40L * 1024 * 1024)
#define SH_STEPS 20000000L
extern long sh_mem_max, sh_mem_used, sh_mem_blocks, sh_mem_fail, sh_steps_max;

/* a value */
enum { SH_EMPTY, SH_NUMBER, SH_TEXT, SH_BOOL, SH_ERROR };
/* the errors: the first seven are every spreadsheet's (OpenFormula's list); the last three are this one's: a
   cycle, a formula that does not parse, and a limit reached (the time or the memory of a recalculation) */
enum { SH_DIV0 = 1, SH_VALUE, SH_REF, SH_NAME, SH_NUM, SH_NA, SH_NULL, SH_CYCLE, SH_SYNTAX, SH_LIMIT };
#define SH_ERRORS 10

struct sh_value {
    int type;
    int b;                              /* SH_BOOL: 0 or 1; SH_ERROR: the error */
    double num;                         /* SH_NUMBER */
    const char *text;                   /* SH_TEXT: len bytes of UTF-8 and a NUL, valid until the sheet changes */
    int len;
};

/* number formats: General, a fixed number of decimals (0 to 10), or a percent with them */
#define SH_GENERAL 0
#define SH_FIXED(d) (0x10 | (d))
#define SH_PERCENT(d) (0x20 | (d))
#define SH_FMT_KIND(f) ((f) & 0xF0)
#define SH_FMT_DECIMALS(f) ((f) & 0x0F)

/* where a cell's text sits in it */
enum { SH_LEFT, SH_RIGHT, SH_CENTER };

struct sh_range { int r1, c1, r2, c2; };        /* inclusive, r1 <= r2 and c1 <= c2 */

struct sheet;
struct sh_clip;
struct sh_reader;

struct sheet *sh_new(void);             /* NULL when there is no memory */
void sh_free(struct sheet *s);

/* names: a column's letters, a cell's name ("B12"); reading them back (letters in either case, $ allowed) */
int sh_colname(int col, char *out);     /* out: 4 bytes; the length */
int sh_cellname(int row, int col, char *out);   /* out: 12 bytes */
int sh_parse_cell(const char *s, int *row, int *col);           /* 0, or -1 */
int sh_parse_range(const char *s, struct sh_range *r);          /* "A1:C3", or one cell: 0, or -1 */
const char *sh_error_name(int e);       /* "#DIV/0!" */

/* reading */
void sh_value(const struct sheet *s, int row, int col, struct sh_value *v);
int sh_format(const struct sheet *s, int row, int col);
/* what the formula bar shows, which typed again makes the same cell: its length (out cut to size) */
int sh_input(const struct sheet *s, int row, int col, char *out, int size);
/* the value as the cell shows it in chars characters (0: as many as it takes), and where it sits: its length */
int sh_display(const struct sheet *s, int row, int col, int chars, char *out, int size, int *align);
/* why a formula shows #ERROR! or #NAME?: 1 with the reason, 0 when nothing is wrong with it */
int sh_problem(const struct sheet *s, int row, int col, char *out, int size);
int sh_is_formula(const struct sheet *s, int row, int col);
int sh_width(const struct sheet *s, int col);
/* the rows and columns up to the last that hold something (0 and 0 for an empty sheet) */
void sh_extent(const struct sheet *s, int *rows, int *cols);
int sh_cells(const struct sheet *s);    /* cells kept */
int sh_formulas(const struct sheet *s); /* of them formulas */
/* the sum, the count of numbers and the count of values in a range: the status bar's */
void sh_stats(const struct sheet *s, struct sh_range r, double *sum, int *numbers, int *values);
int sh_modified(const struct sheet *s);
unsigned sh_state(const struct sheet *s);       /* names what the sheet holds: the same again after an Undo */
void sh_saved(struct sheet *s);         /* the file holds what the sheet holds now */
int sh_limited(const struct sheet *s);  /* the last recalculation stopped at a limit: some cells say #LIMIT! */
const char *sh_why(const struct sheet *s);      /* why the last change was refused */

/* changes: each is one step of the history. 0, or -1 with sh_why() saying why, and nothing changed */
int sh_set(struct sheet *s, int row, int col, const char *text, int len);
int sh_clear(struct sheet *s, struct sh_range r);
int sh_set_format(struct sheet *s, struct sh_range r, int fmt);
int sh_set_width(struct sheet *s, int c1, int c2, int px);
int sh_insert_rows(struct sheet *s, int at, int n);
int sh_delete_rows(struct sheet *s, int at, int n);
int sh_insert_cols(struct sheet *s, int at, int n);
int sh_delete_cols(struct sheet *s, int at, int n);
/* the history: where says what the step changed. 0, or -1 (nothing to do, or no memory: sh_why()) */
int sh_undo(struct sheet *s, struct sh_range *where);
int sh_redo(struct sheet *s, struct sh_range *where);
const char *sh_undo_label(const struct sheet *s);       /* "typing", "paste", ...; NULL: nothing to undo */
const char *sh_redo_label(const struct sheet *s);

/* the clipboard: cells copied with their formulas, and their values as tab-separated text for the desktop's
   clipboard. A paste adjusts the formulas' relative references by how far they moved; text pasted from
   elsewhere is taken as typed, a field a cell */
struct sh_clip *sh_copy(struct sheet *s, struct sh_range r);   /* NULL: sh_why() */
const char *sh_clip_text(const struct sh_clip *c, int *len);
void sh_clip_size(const struct sh_clip *c, int *rows, int *cols);
void sh_clip_free(struct sh_clip *c);
int sh_paste(struct sheet *s, const struct sh_clip *c, int row, int col, struct sh_range *where);
int sh_paste_text(struct sheet *s, const char *text, int len, int row, int col, struct sh_range *where);

/* files: the sheet's own text format, which keeps the formulas, and CSV (RFC 4180), whose save keeps the values.
   A file is read a piece at a time into a new sheet; a sheet is written through put(), which returns 0, or -1
   to stop */
enum { SH_NATIVE, SH_CSV };
int sh_kind_of(const char *path);       /* SH_CSV for a .csv, SH_NATIVE for anything else */
struct sh_reader *sh_read_begin(int kind);      /* NULL: no memory */
int sh_read(struct sh_reader *rd, const char *bytes, int n);   /* 0, or -1 once it refuses */
struct sheet *sh_read_end(struct sh_reader *rd, char *why, int size);   /* the sheet, or NULL and why; rd goes */
void sh_read_cancel(struct sh_reader *rd);
int sh_write(const struct sheet *s, int kind, int (*put)(void *ctx, const char *bytes, int n), void *ctx);

#endif
