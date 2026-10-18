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
    if(!shadow) {                               /* the screen, as main makes it, for a question */
        screen_size();
        shadow = calloc((size_t)rows, (size_t)cols + 1);
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
