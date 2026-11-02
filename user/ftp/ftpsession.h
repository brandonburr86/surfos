/*
SurfOS file transfer client: ftp
--------------------
File: ftpsession.h  Date: 11/01/26
--------------------
A session with a server (session.c): the control connection, passive data connections, the files
beside them. Each call says what it did on the way, on standard error, and returns a status: FS_OK,
FS_NO when the server said no (a negative answer), FS_TROUBLE for anything else. commands.c turns typed lines
into these calls. tests/host/test_ftpcs.c runs both against a scripted server.
*/

#ifndef _FTPSESSION_H
#define _FTPSESSION_H

#define FS_OK      0
#define FS_NO      1
#define FS_TROUBLE 2

#define FS_NAMES_MAX  4096              /* names of one listing kept for mget */
#define FS_NAMES_BYTES (256 * 1024)     /* their bytes */

#define FS_CUT_LONG   1                 /* fs_names(): more came than is kept */
#define FS_CUT_SHORT  2                 /* the listing failed part-way: what came is not all there is */

struct fs_names {
    char **name;
    int count;
    int cut;                            /* FS_CUT_*: not the whole listing, and nothing is to be taken from it */
};

void fs_init(void);                     /* no connection, binary mode */
int fs_connected(void);
int fs_stopped(void);                   /* Ctrl+C stopped a command: asked once, then forgotten */

int fs_open(const char *host, int port);    /* a connection and the server's greeting; one open closes first */
int fs_login(const char *user);         /* NULL: anonymous, which never asks for a password */
int fs_close(void);                     /* QUIT, and the connection closed */

int fs_list(const char *verb, const char *path);        /* LIST or NLST, on standard output */
int fs_names(const char *dir, struct fs_names *out);    /* NLST: the last name of each line */
void fs_names_free(struct fs_names *n);

int fs_get(const char *remote, const char *local);      /* NULL local: standard output */
int fs_reget(const char *remote, const char *local);
int fs_put(const char *local, const char *remote);

int fs_cd(const char *path);
int fs_pwd(void);
int fs_verb(const char *verb, const char *arg);         /* DELE, MKD, RMD: success is a 2xx */
int fs_rename(const char *from, const char *to);
int fs_size(const char *path);

void fs_set_text(int text);             /* ascii or binary, from the next transfer on */
void fs_status(void);

/* Ctrl+C: on while a command runs, off at the prompt, where it ends ftp as it ends any program. Either call also reads
   a Ctrl+C that came since the last look, in no wait, and fs_stopped() then says so */
void fs_catch_interrupts(int on);

/* standard output could not be written: the run is over, whatever was still to do */
int fs_output_failed(void);

#endif
