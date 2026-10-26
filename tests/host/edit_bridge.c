/* The bridge of tests/host/test_edit.c (10/17/26): user/bin/edit.c as it is, built for the host against
   SurfOS's user headers with the C library's and the system calls' names (tests/host/libsurf_names.h,
   sim_names.h), on the in-memory file system of tests/host/sim_fs.c; the test drives the editor's own
   functions through these (tests/host/edit_sim.h). */
#define main    edit_main
#include "../../user/bin/edit.c"
#include "edit_sim.h"

void ed_start(const char *name) {
    int i;
    for(i = 0; i < nlines; i++) free(lines[i].s);
    nlines = 0;
    filename = name;
    crlf = modified = no_rename = unsure = 0;
    changes = copied_changes = 0;
    cy = cx = 0;
    msg[0] = 0;
    copied[0] = 0;
    forget_history();                           /* 10/25/26 (A9): undo's too */
    saved_serial = serial_now();                /* the text as read is the saved one (the review's U2: a new base) */
    if(!shadow) {                               /* the screen, as main makes it, for a question */
        screen_size();
        shadow = calloc((size_t)rows, (size_t)ROW_BYTES);
        shadow_inv = calloc((size_t)rows, 1);
    }
    shadow_ok = 0;
    if(load(name) == 0) look_beside();
    sim_clear_log();
}

void ed_type(const char *s) {
    for(; *s; s++) {
        if(*s == '\n') enter();
        else insert_char((unsigned char)*s);
    }
}

int ed_save(void) {
    return save();
}

const char *ed_msg(void) {
    return msg;
}

int ed_modified(void) {
    return modified;
}

int ed_unsure(void) {
    return unsure;
}

const char *ed_copied(void) {
    return copied;
}

int ed_quit(void) {
    return quit();
}

/* 10/25/26 (track A, A9) */
const int ed_left = KEY_LEFT, ed_right = KEY_RIGHT, ed_up = KEY_UP, ed_down = KEY_DOWN, ed_home = KEY_HOME,
          ed_end = KEY_END, ed_del = KEY_DEL;

void ed_key(int k) {
    msg[0] = 0;
    handle(k);
}

const char *ed_line(int y) {
    static char buf[4096];
    int n = y < nlines ? lines[y].len : 0;
    if(n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    memcpy(buf, y < nlines ? lines[y].s : "", (size_t)n);
    buf[n] = 0;
    return buf;
}

int ed_nlines(void) {
    return nlines;
}

void ed_cursor(int *y, int *x) {
    *y = cy;
    *x = cx;
}

int ed_col(void) {
    return col_of(&lines[cy], cx);
}

void ed_utf8(int on) {
    utf8 = on;
}

const char *ed_render(int y, int from) {
    static char buf[4 * 512 + 2];
    render_line(&lines[y], buf, from);
    return buf;
}

int ed_cells(const char *s, int max, int *cells) {
    return cells_bytes(s, max, cells);
}
