/*
SurfOS user program: wcode
--------------------
File: fileio.h  Date: 11/05/26
--------------------
The Code Editor's files (fileio.c): reading one into a document and saving it, with the system
calls; tests/host/test_wcode_save.c runs it on the simulated file system of tests/host/sim_fs.c,
where writes, fsyncs and renames fail on request.

Saving follows the rules of the command-line suite and of edit (CLAUDE.md, user/bin/edit.c):
- a file the user has is replaced only through user/bin/saveutil.h: the text goes into a new file
  beside it (<file>.tmp, or .1.tmp to .9.tmp when those names are the user's), which is fsynced
  and renamed over it; it is never truncated, and until the rename it is as it was;
- a file Save as makes is O_CREAT | O_EXCL, and goes again when the save fails partway;
- edit's care over a rename that may have half happened: the file and the new one may then share
  their data, so for as long as the file is open nothing is renamed or written over it; each save
  puts a copy beside it instead (a new name, O_EXCL, none when nothing changed since the last),
  and the status bar says to have the disk checked. The same holds from the start when a name
  beside the file looks like its other half (su_twin_beside(): its size and time).
*/
#ifndef _WCODE_FILEIO_H
#define _WCODE_FILEIO_H

#include "doc.h"

/* path into d, an empty document: 0; or -1 with d->msg saying why (no such file, a folder, more than 16 MB, not
   text, changed while it was read, no memory) */
int wc_file_read(struct wc_doc *d, const char *path);
/* d into its own file (to NULL) or into to (Save as; the chooser has asked about replacing it): 0 saved; 1 not
   over the file, but a copy beside it (d->msg names it); -1 not saved (d->msg says why) */
int wc_file_save(struct wc_doc *d, const char *to);
/* the text of a document with unsaved changes put beside its file under a new name, when the window has gone and
   nothing can be asked: 0 with the name in where, -1 (no path, no name free, the disk) */
int wc_file_rescue(struct wc_doc *d, char *where, unsigned size);

#endif
