/* The bridge of tests/host/test_wcode_save.c (11/05/26, the desktop applications, track code): user/wcode/fileio.c and
   the model it saves, as they are, built for the host against SurfOS's user headers with the C library's and the
   system calls' names (tests/host/libsurf_names.h, sim_names.h), on the in-memory file system of tests/host/sim_fs.c;
   the test drives them through these (tests/host/wcode_sim.h). */
#include <string.h>
#include "fileio.h"
#include "wcode_sim.h"

static struct wc_pool pool = { 0, WC_POOL_MAX };
static struct wc_doc doc;
static int live;

static void fresh(void) {
    if(live) wc_doc_free(&doc);
    if(wc_doc_init(&doc, &pool) < 0) __builtin_trap();
    live = 1;
}

int wcs_open(const char *path) {
    int r;
    fresh();
    r = wc_file_read(&doc, path);
    sim_clear_log();
    return r;
}

void wcs_new(void) {
    fresh();
    doc.untitled = 1;
}

void wcs_type(const char *s) {
    const char *p;
    for(p = s; *p; p++) {
        if(*p == '\n') wc_doc_enter(&doc);
        else wc_doc_type(&doc, p, 1);
    }
}

int wcs_save(const char *to) { return wc_file_save(&doc, to); }
const char *wcs_msg(void) { return doc.msg; }
int wcs_modified(void) { return wc_doc_modified(&doc); }
int wcs_unsure(void) { return doc.unsure; }
int wcs_safe(void) { return wc_doc_safe(&doc); }
const char *wcs_copied(void) { return doc.copied; }
const char *wcs_path(void) { return doc.path; }
int wcs_lang(void) { return doc.lang; }
int wcs_crlf(void) { return doc.crlf; }
int wcs_rescue(char *out, unsigned size) { return wc_file_rescue(&doc, out, size); }
void wcs_undo(void) { wc_doc_undo(&doc, 0); }
