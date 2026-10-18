/* tests/host/test_edit.c and its bridge, tests/host/edit_bridge.c (10/17/26): the simulated file system
   (sim_fs.h) and what the test asks of the editor. Plain C, for the host's headers and SurfOS's alike. */
#ifndef EDIT_SIM_H
#define EDIT_SIM_H

#include "sim_fs.h"

void ed_start(const char *name);                        /* the editor as it starts on name; then the log is cleared */
void ed_type(const char *s);                            /* typed at the cursor, '\n' as Enter */
int ed_save(void);                                      /* ^S: 0, 1 (a copy beside the file), or -1 */
const char *ed_msg(void);                               /* the status line's message */
int ed_modified(void);
int ed_unsure(void);                                    /* a rename may have half happened: the help line warns */
const char *ed_copied(void);                            /* since then: where the text last went beside the file */
int ed_quit(void);                                      /* ^Q: 1 when edit ends; a question is cancelled (no input) */

#endif
