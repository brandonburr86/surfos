/* The suite's host tests (docs/superpowers/plans/2026-10-17-gui-suite.md): an in-memory file system (struct gui_fs)
   and system (struct gui_sys) for the applications, a screen with guard pixels around it, and CHECK. Each track's
   test file includes this; tests/host/gui_fake.c is linked into each. */
#ifndef _TESTS_GUI_FAKE_H
#define _TESTS_GUI_FAKE_H
#include <sys/gui.h>
#include <stdio.h>
#include <string.h>
#include "../../shell/guipriv.h"

extern int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)
int fake_done(const char *name);                /* prints the count; the exit status */

/**** files ****/
#define FAKE_FILES 64
#define FAKE_POOL (2 * 1024 * 1024)
void fake_files_reset(void);                    /* / with /initrd (read-only) and /home */
void fake_file(const char *path, const char *data, u32 len);   /* made or replaced (its folder must be there) */
void fake_dir(const char *path);
const char *fake_data(const char *path, u32 *len);             /* NULL when there is no such file */
extern const struct gui_fs fake_fs;
extern int fake_fail_write;                     /* a negative errno: the next writes fail so */

/**** the system ****/
extern struct gui_task fake_tasks[GUI_TASK_MAX];
extern int fake_ntasks;
extern struct gui_memory fake_mem;
extern struct gui_machine fake_machine;
extern struct gui_disk fake_disks[GUI_DISK_MAX];
extern int fake_ndisks;
extern char fake_log[GUI_LOG_MAX];              /* the log's text; fake_log_written: how many bytes ever */
extern u32 fake_log_written;
extern int fake_killed;                         /* the last pid kill was asked for, 0 */
/* the Terminal's one session: what it "printed" (fed by the test), what was typed, its size, whether it runs */
extern char fake_term_out[8192], fake_term_in[8192];
extern u32 fake_term_out_len, fake_term_in_len;
extern int fake_term_cols, fake_term_rows, fake_term_open_count;
extern bool fake_term_running, fake_term_ended;
void fake_term_feed(const char *s, u32 n);      /* the session prints this */
/* 10/25/26: programs' windows (struct gui_sys's win_* calls): a window waits until its program presents; its pixels
   come from a pool, and every event the desktop sends it is kept (nev of them); run keeps the line it was given */
#define FAKE_WINS 12
#define FAKE_EVENTS 512
struct fake_win {
    bool used, ready, accepted, ended, closed;
    int handle;
    struct win_state st;
    u32 *px;                                    /* st.w x st.h */
    struct win_event ev[FAKE_EVENTS];
    int nev;
};
extern struct fake_win fake_wins[FAKE_WINS];
extern char fake_run_line[256];
extern int fake_run_count, fake_run_result;     /* run's calls; what it answers (a pid, or -errno) */
extern char fake_clip[64];                      /* 10/26/26: text a program put on the clipboard, until taken, */
extern int fake_clip_from;                      /* and the handle of its window */
int fake_win_open(int w, int h, u32 flags, u32 background, const char *title, int pid);   /* its number; -1: no room */
void fake_win_present(int i, u32 color);        /* its pixels all in color: it is ready */
void fake_win_size(int i, int w, int h, u32 color);   /* pixels of a new size, in color */
void fake_win_end(int i);                       /* its program closed it, or is gone */
int fake_win_count(int i, int type);            /* the events of that type sent to it */
const struct win_event *fake_win_last(int i, int type);   /* the last of them, NULL for none */
void fake_win_forget(int i);                    /* the events sent so far forgotten */
void fake_sys_reset(void);
extern const struct gui_sys fake_sys;

/**** a screen of width x height inside guard pixels ****/
void fake_screen(struct fb_surface *s, int width, int height);
bool fake_guards(int width, int height);         /* nothing drawn outside it */
u32 fake_pixel(int width, int x, int y);

/**** the desktop ****/
/* a desktop of width x height with the fakes, at time ms 5000, 2026-10-17 14:05:30 */
void fake_desktop(struct gui_state *g, int width, int height);
void fake_click(struct gui_state *g, int x, int y);              /* press and release the left button there */
void fake_type(struct gui_state *g, const char *s);
#endif
