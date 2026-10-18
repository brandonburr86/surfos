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
