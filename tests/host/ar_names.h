/* Force-included after tests/host/libsurf_names.h where gzip, tar and unzip are built for the host (11/01/26): the system
   calls and the stdio they use, renamed, so that tests/host/ar_sim.c stands in for them. Since stat is among them, struct
   stat is struct surf_stat there, SurfOS's layout. printf is a macro with arguments because stdio.h's format() attributes
   name it: only a call is renamed, and the attribute keeps its meaning. */
#define open     surf_open
#define close    surf_close
#define read     surf_read
#define lseek    surf_lseek
#define fsync    surf_fsync
#define sync     surf_sync
#define fstat    surf_fstat
#define stat     surf_stat
#define mkdir    surf_mkdir
#define rmdir    surf_rmdir
#define rename   surf_rename
#define unlink   surf_unlink
#define chdir    surf_chdir
#define getcwd   surf_getcwd
#define isatty   surf_isatty
#define opendir  surf_opendir
#define readdir  surf_readdir
#define closedir surf_closedir
#define fprintf  surf_fprintf
#define stderr   surf_stderr
#define stdout   surf_stdout
#define fputs    surf_fputs
#define printf(...) surf_printf(__VA_ARGS__)
