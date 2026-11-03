/*
SurfOS file transfer client: ftp
--------------------
File: ftpcommands.h  Date: 11/01/26
--------------------
The commands (commands.c), for main.c and tests/host/test_ftpcs.c.
*/

#ifndef _FTPCOMMANDS_H
#define _FTPCOMMANDS_H

extern int ftp_auto_login;              /* open logs in as anonymous: not with ftp -n */

void ftp_commands_init(void);

/* connect to host, on port_word (NULL: 21), and log in unless ftp -n said not to: a status */
int ftp_open(const char *host, const char *port_word);

/* a typed line, changed in place, run: its status (FS_OK, ...) */
int ftp_run_line(char *line);

/* quit was typed */
int ftp_quitting(void);

/* Ctrl+C stopped a command inside mget or mput, which looked at fs_stopped() */
void ftp_note_stopped(void);

#endif
