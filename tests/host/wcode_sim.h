/* tests/host/test_wcode_save.c and its bridge, tests/host/wcode_save_bridge.c (11/05/26): the simulated file system
   (sim_fs.h) and what the test asks of the Code Editor's files (user/wcode/fileio.c). Plain C, for the host's headers
   and SurfOS's alike. */
#ifndef WCODE_SIM_H
#define WCODE_SIM_H

#include "sim_fs.h"

int wcs_open(const char *path);                         /* a file read into a new document: 0, or -1 */
void wcs_new(void);                                     /* a new document with no file */
void wcs_type(const char *s);                           /* typed at the cursor, '\n' as Enter */
int wcs_save(const char *to);                           /* Save (to NULL) or Save as: 0, 1 (a copy beside), -1 */
const char *wcs_msg(void);                              /* the status bar's message */
int wcs_modified(void);
int wcs_unsure(void);                                   /* edit's care: nothing goes over the file */
int wcs_safe(void);                                     /* saved, or in the last copy beside the file */
const char *wcs_copied(void);                           /* the last copy beside the file */
const char *wcs_path(void);
int wcs_lang(void);
int wcs_crlf(void);
int wcs_rescue(char *out, unsigned size);               /* the window gone: the text beside the file */
void wcs_undo(void);
/* 11/09/26 (the review): which of two documents the calls are for (0 or 1); the caret, the text's bytes, Backspace;
   whether the last save was refused because the file changed on the disk since, and the person's "save over it" */
void wcs_use(int i);
void wcs_caret(unsigned pos);
unsigned wcs_len(void);
void wcs_back(void);
int wcs_disk_changed(void);
void wcs_overwrite(void);

#endif
