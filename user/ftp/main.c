/*
SurfOS file transfer client: ftp
--------------------
File: main.c      Date: 11/01/26
--------------------
ftp [-n] [host [port]]: files to and from an FTP server, at a prompt or from a script on standard
input (ftp host < commands). With a host it connects at once and logs in as anonymous; -n leaves
the logging in to user. Transfers are binary until ascii says otherwise, and every data connection
is passive (session.c). A password is asked for, without echo, on the terminal itself: it is never
taken from a line, a script or the command line. The commands are in commands.c, and help lists
them. Standard output holds only listings and the bytes of get -: everything else goes to standard
error, so ftp host > file holds what was asked for.

A script stops at its first failed command: a cd that failed must not leave the next delete to run
somewhere else. At a terminal the next command is the user's to choose.

Ctrl+C stops the command that runs, and ftp goes on at the prompt; a script ends there, with
status 130, also when the key came between two commands or in no wait at all. At the prompt Ctrl+C
ends ftp, as it ends any program there.

Status 0 when every command worked, 1 when the server said no to one (or a mget left something
out), 2 for anything worse: a usage mistake, no connection, a connection or a file that failed, or
standard output that cannot be written, which also ends the run at a terminal.
*/

#include <surf.h>
#include "ftpclient.h"
#include "ftpsession.h"
#include "ftpcommands.h"

#define LINE_MAX_  1024                 /* a typed line */

int ftp_auto_login = 1;
static int stopped;

void ftp_note_stopped(void) {
    stopped = 1;
}

static int usage(void) {
    fprintf(stderr, "usage: ftp [-n] [host [port]]\n");
    return 2;
}

/* the end of the run, the connection closed: its status */
static int leave(int status) {
    fs_catch_interrupts(1);
    fs_close();
    return status;
}

/* a command is over, with status r: 1 when the run ends here, *worst its status. A Ctrl+C that no wait saw is read here */
static int over(int r, int terminal, int *worst) {
    if(r > *worst) *worst = r;
    fs_catch_interrupts(!terminal);
    if(fs_stopped()) stopped = 1;
    if(stopped && !terminal) {
        *worst = leave(130);
        return 1;
    }
    stopped = 0;
    if(fs_output_failed()) {
        *worst = leave(2);
        return 1;
    }
    if(r && !terminal) {
        fprintf(stderr, "ftp: that failed: the rest of the script does not run\n");
        leave(0);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    static char line[LINE_MAX_];
    int i = 1, worst = 0, terminal = isatty(0);
    stopped = 0;
    if(i < argc && !strcmp(argv[i], "-n")) {
        ftp_auto_login = 0;
        i++;
    }
    if(argc - i > 2 || (i < argc && argv[i][0] == '-')) return usage();
    fs_init();
    ftp_commands_init();
    fs_catch_interrupts(1);             /* a script's stays on: its Ctrl+C counts between commands too */
    if(i < argc && over(ftp_open(argv[i], i + 1 < argc ? argv[i + 1] : NULL), terminal, &worst)) return worst;
    fs_catch_interrupts(!terminal);
    while(!ftp_quitting()) {
        int n;
        if(terminal) fputs("ftp> ", stderr);
        fflush(stdout);
        n = readline(line, sizeof(line));
        fs_catch_interrupts(1);
        if(fs_stopped()) stopped = 1;
        if(stopped && !terminal) return leave(130);
        if(n < 0) {
            fs_close();
            over(FS_OK, terminal, &worst);
            break;
        }
        if(n >= LINE_MAX_ - 1) {        /* cut: a command run on what is left of a line could name another file */
            fprintf(stderr, "ftp: that line is too long: nothing was done\n");
            if(over(FS_TROUBLE, terminal, &worst)) return worst;
            continue;
        }
        if(over(ftp_run_line(line), terminal, &worst)) return worst;
    }
    if(fflush(stdout) == EOF && !worst) worst = 2;
    return worst;
}
