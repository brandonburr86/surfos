/* The bridge of tests/host/test_sedsave.c (11/01/26, the command-line suite): user/bin/sed.c as it is, built for the host against
   SurfOS's user headers with the C library's and the system calls' names (libsurf_names.h, sim_names.h, text_names.h), on the
   in-memory file system of tests/host/sim_fs.c, where writes, fsyncs and renames fail on request. Reads fail here. */
#include "text_names.h"
#include <unistd.h>
#include <errno.h>
#include <surf.h>

static int reads_left = -1;

/* the next but n read fails, -1 for none */
void sed_fail_read(int n) {
    reads_left = n;
}

static ssize_t read_for_test(int fd, void *buf, size_t n) {
    if(!reads_left) {
        reads_left = -1;
        errno = EIO;
        return -1;
    }
    if(reads_left > 0) reads_left--;
    return read(fd, buf, n);
}

void broken_pipe_exit(int on) {
    (void)on;
}

#undef read
#define read read_for_test
#define main sed_main
#include "../../user/bin/sed.c"
