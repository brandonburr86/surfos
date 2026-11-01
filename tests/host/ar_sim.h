/* The world of tests/host/ar_sim.c (11/01/26): gzip, tar and unzip as they are, built against SurfOS's user headers with
   libsurf's names (tests/host/libsurf_names.h, ar_names.h), run on an in-memory file system that behaves as FAT under
   SurfOS's kernel: names ignore ASCII case and refuse what fs/fat.c's validate_name() refuses; a path of 256 bytes or
   more is EFAULT; lseek stops at 2 GB; a program has 16 descriptors; a folder must exist before anything goes in it.
   A test sets up files and standard input, runs a tool, and reads back its status, its output and what is on the disk.
   Any change to the disk can be made to fail, once or from then on (the disk pulled out), and the disk can fill up.
   Plain C, for the host's headers. */
#ifndef AR_SIM_H
#define AR_SIM_H

#include <stddef.h>

typedef int (*ars_tool)(int argc, char **argv);

/* SurfOS's numbers (user/include/errno.h), whatever the host's are */
#define ARS_ENOENT    2
#define ARS_EIO       5
#define ARS_ENOSPC   28
#define ARS_EROFS    30

void ars_reset(void);                                   /* an empty disk with / and cwd /, no failures, stdin empty */
void ars_put(const char *path, const void *bytes, size_t n);    /* a file, with the folders above it */
void ars_puts(const char *path, const char *text);
void ars_mkdir(const char *path);                       /* a folder, with those above it */
void ars_device(const char *path);                      /* a file the kernel's devfs would own */
/* a file of head bytes, hole zero bytes, then tail bytes: a file of 3 GB costs no memory */
void ars_put_hole(const char *path, const void *bytes, size_t head, unsigned long long hole, size_t tail);
const unsigned char *ars_get(const char *path, size_t *n);      /* a file's bytes, or NULL */
int ars_is(const char *path, const char *text);         /* does the file hold exactly this text? */
int ars_is_dir(const char *path);
int ars_exists(const char *path);
int ars_count(void);                                    /* files and folders, / not counted */
const char *ars_list(void);                             /* every name, one a line, in the order made */
int ars_all_under(const char *dir);                     /* every name made since ars_mark() is inside dir */
void ars_mark(void);
void ars_chdir(const char *path);
void ars_same_time(const char *a, const char *b);       /* b gets a's mtime: with one size, what a half-done rename leaves */
void ars_lie_size(const char *path, unsigned extra);    /* stat and fstat say this many bytes more: a file that shrank since */

void ars_stdin(const void *bytes, size_t n, int is_pipe, size_t chunk);     /* reads give at most chunk bytes */
void ars_ttys(int in, int out);                         /* are descriptors 0, and 1 and 2, terminals? */
void ars_stdout_closes_after(size_t n);                 /* the reader of the pipe goes: the program dies at a write, 141 */

const unsigned char *ars_stdout(size_t *n);             /* since the last run began */
const char *ars_stderr(void);

void ars_fail_change(int nth, int err);                 /* the nth change of the next run (1 on) fails once */
void ars_pull_at(int nth);                              /* from the nth change on all fail, EIO: the disk is gone */
void ars_quota(size_t bytes);                           /* writes past this many bytes in all fail: a full disk */
void ars_rename_half(void);                             /* the next rename fails with EIO, half done: both names hold the data */
int ars_changes(void);                                  /* changes the last run made */
int ars_open_files(void);                               /* descriptors open when the last run ended, 0 to 2 not counted */
int ars_most_open(void);                                /* the most open at once, in the last run */
int ars_long_paths(void);                               /* calls given a path of 256 bytes or more in the last run */

/* Runs tool with these arguments, argv[0] first, NULL after the last: its status, 141 for the pipe above */
int ars_run(ars_tool tool, const char *const *argv);
/* the same, the arguments split at spaces */
int ars_cmd(ars_tool tool, const char *line);

#endif
