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
them.

Ctrl+C stops the command that runs, and ftp goes on at the prompt; a script ends there, with
status 130. At the prompt Ctrl+C ends ftp, as it ends any program there.

Status 0 when every command worked, 1 when the server said no to one (or a mget left something
out), 2 for anything worse: a usage mistake, no connection, a connection or a file that failed.
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
    if(i < argc) {
        fs_catch_interrupts(1);
        worst = ftp_open(argv[i], i + 1 < argc ? argv[i + 1] : NULL);
        fs_catch_interrupts(0);
    }
    while(!ftp_quitting()) {
        int r, n;
        if(terminal) {
            printf("ftp> ");
            fflush(stdout);
        }
        if((n = readline(line, sizeof(line))) < 0) {
            fs_catch_interrupts(1);
            fs_close();
            fs_catch_interrupts(0);
            break;
        }
        if(n >= LINE_MAX_ - 1) {        /* cut: a command run on what is left of a line could name another file */
            fprintf(stderr, "ftp: that line is too long: nothing was done\n");
            worst = 2;
            continue;
        }
        fs_catch_interrupts(1);
        r = ftp_run_line(line);
        fs_catch_interrupts(0);
        if(r > worst) worst = r;
        if(fs_stopped()) stopped = 1;
        if(stopped) {
            if(!terminal) {
                fs_catch_interrupts(1);
                fs_close();
                return 130;
            }
            stopped = 0;
        }
    }
    return worst;
}
