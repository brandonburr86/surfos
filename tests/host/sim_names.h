/* Force-included after tests/host/libsurf_names.h where user/bin/edit.c or rx.c is built for the host
   (10/17/26): the system calls they make, renamed, so that tests/host/sim_fs.c stands in for them. Since
   stat is among them, struct stat is struct surf_stat in those files, the same layout everywhere. */
#define open    surf_open
#define close   surf_close
#define read    surf_read
#define fsync   surf_fsync
#define fstat   surf_fstat
#define stat    surf_stat
#define rename  surf_rename
#define unlink  surf_unlink
#define isatty  surf_isatty
#define ttyctl  surf_ttyctl
#define uptime  surf_uptime
#define fprintf surf_fprintf
#define stderr  surf_stderr
