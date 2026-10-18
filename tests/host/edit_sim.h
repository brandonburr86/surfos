/* tests/host/test_edit.c and its bridge, tests/host/edit_bridge.c (10/17/26): what the test asks of the
   simulated file system and of the editor. Plain C, for the host's headers and SurfOS's alike. */
#ifndef EDIT_SIM_H
#define EDIT_SIM_H

enum { SIM_OPEN, SIM_WRITE, SIM_FSYNC, SIM_CLOSE, SIM_RENAME, SIM_UNLINK, SIM_OPS };

/* how a failing rename leaves the names: as they were; half done (the new name written and the old one
   not yet gone, both holding the same data, as fs/fat.c can be left by an I/O error); all done. fs/fat.c
   can also lose only the old name's long-name entries, its short entry staying under the 8.3 alias: a
   case with no name to look for, which this simulation does not have. */
enum { SIM_NONE, SIM_HALF, SIM_DONE };

/* SurfOS's numbers (user/include/errno.h, Linux's), whatever the host's are; the bridge checks them */
#define SIM_ENOENT        2
#define SIM_EIO           5
#define SIM_ENOMEM       12
#define SIM_EFAULT       14
#define SIM_EBUSY        16
#define SIM_EEXIST       17
#define SIM_EXDEV        18
#define SIM_ENOTDIR      20
#define SIM_EISDIR       21
#define SIM_EINVAL       22
#define SIM_ENOSPC       28
#define SIM_EROFS        30
#define SIM_ENAMETOOLONG 36
#define SIM_ENOSYS       38
#define SIM_ENOTEMPTY    39

/* Names are compared ignoring ASCII case, as FAT does, and a path of 256 bytes or more is refused with
   EFAULT, as the kernel's system calls do (kernel/syscall.c, ARG_STR_MAX). */
void sim_reset(void);                                   /* no names, no failures, an empty log */
void sim_put(const char *name, const char *bytes);      /* a file */
void sim_mkdir(const char *name);
int sim_get(const char *name, char *buf, int size);     /* the file's bytes, terminated: their count; -1: none */
int sim_names(void);                                    /* how many names there are */
int sim_corrupt(const char *name);                      /* its data was freed under it, by its twin's deletion */
/* the next op on name (a rename: its from) fails with err; for a rename, how is SIM_NONE, SIM_HALF or
   SIM_DONE; for a write, how writes to name succeed first, and err 0 makes it a short one instead */
void sim_fail(int op, const char *name, int err, int how);
const char *sim_log(void);                              /* the calls since the last clear: "open a.txt.tmp excl\n" */
void sim_clear_log(void);

void ed_start(const char *name);                        /* the editor as it starts on name; then the log is cleared */
void ed_type(const char *s);                            /* typed at the cursor, '\n' as Enter */
int ed_save(void);                                      /* ^S: 0, or -1 */
const char *ed_msg(void);                               /* the status line's message */
int ed_modified(void);
int ed_unsure(void);                                    /* a rename may have half happened: the help line warns */

#endif
