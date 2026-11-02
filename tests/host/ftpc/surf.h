/* Host-side test shim (tests/host/test_ftpcs.c, 11/01/26): the SurfOS user API that the ftp client uses, mapped to
   the test's simulated kernel. The real C library does the rest, so the same files build with clang's sanitizers
   on the Mac. Everything from the host's headers comes first: the names below then rename only the client's own
   calls, and struct stat becomes SurfOS's struct, as saveutil.h expects it. */
#ifndef SIM_FTPC_SURF_H
#define SIM_FTPC_SURF_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "sim.h"

#undef PATH_MAX
#define PATH_MAX 256                    /* SurfOS's */
#undef S_IFREG
#undef S_IFDIR
#undef S_ISREG
#undef S_ISDIR
#define S_IFREG 1
#define S_IFDIR 2
#define S_ISREG(m) ((m) == S_IFREG)
#define S_ISDIR(m) ((m) == S_IFDIR)

#define TTY_RAW     1
#define TTY_INTR    6

#undef stat
#undef fflush
#undef fputc
#undef fputs
#undef printf
#undef fprintf
#undef vfprintf
#undef ferror
#undef strdup
#define ferror      sim_ferror
#define strdup      sim_strdup
#define procinfo    sim_procinfo
#define getpid      sim_getpid
#define stat        sim_stat
#define open        sim_open
#define close       sim_close
#define read        sim_read
#define write       sim_write
#define fsync       sim_fsync
#define ftruncate   sim_ftruncate
#define rename      sim_rename
#define unlink      sim_unlink
#define chdir       sim_chdir
#define getcwd      sim_getcwd
#define opendir     sim_opendir
#define readdir     sim_readdir
#define closedir    sim_closedir
#define DIR         void
#define poll(p, n, t) sim_poll(p, n, t)
#define ttyctl      sim_ttyctl
#define isatty      sim_isatty
#define uptime      sim_uptime
#define readline    sim_readline
#define tcp_connect sim_tcp_connect
#define net_resolve sim_net_resolve
#define ipparse     sim_ipparse
#define ipstr       sim_ipstr
#define printf      sim_printf
#define fprintf     sim_fprintf
#define vfprintf    sim_vfprintf
#define fputs       sim_fputs
#define fputc       sim_fputc
#define fflush      sim_fflush
#endif
