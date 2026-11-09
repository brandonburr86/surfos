/*
SurfOS chat client: wchat
--------------------
File: link.h    Date: 11/05/26
--------------------
The window and its helper. The connection is the helper's: wchat --helper HOST PORT tls|tcp,
started by the window with spawnx(), its descriptor 0 a pipe from the window and 1 a pipe to
it. The helper looks the name up, connects, and for tls runs Undertow's transport with its
certificate checks (user/undertow/transport.c, as user/fetch/ links it). So the window never
waits for the network and never runs TLS: a server that breaks TLS, or the helper, costs the
connection, never the window and its rooms. (Undertow's transport waits in its calls, up to 35
seconds to connect; a window that did that would not answer its close button.)

From the window to the helper: the bytes for the server as they are, whole lines (chat_send()).
The window closes its end to end the connection: the helper sends what it still has, closes the
connection and ends.

From the helper to the window: frames, each a head (struct link_head) and len bytes:

    LINK_UP     the connection is made: TLS's version ("TLSv1.3"), nothing for TCP
    LINK_DATA   bytes from the server, 1 to LINK_DATA_MAX
    LINK_DOWN   the connection ended, or never came: why, a sentence of LINK_TEXT_MAX at most

The helper's process has taken a server's TLS records, so what it sends is read as hostile: a
head counts only with the magic, a kind known, a length its kind allows, and in order (UP once and
first, DOWN once and last). Anything else is a broken link, and the window ends the connection.
link.c makes no system calls: tests/host/test_wchat.c runs it, as it does the model.
*/

#ifndef _WCHAT_LINK_H
#define _WCHAT_LINK_H

#define LINK_MAGIC    (0x31434857u + (unsigned)sizeof(struct link_head))    /* "WHC1": another build's is no frame */
#define LINK_DATA_MAX 16384             /* a TLS record's plaintext at most */
#define LINK_TEXT_MAX 512
#define LINK_TLS_MAX  15

enum { LINK_UP = 1, LINK_DATA, LINK_DOWN };

struct link_head {
    unsigned int magic, kind, len;
};

struct link_reader {
    unsigned char head[sizeof(struct link_head)];
    int have;                           /* bytes of the head so far */
    struct link_head h;
    int got;                            /* bytes of the body so far */
    int up, down;                       /* UP has come; DOWN has */
    char body[LINK_DATA_MAX + 1];
};

/* Bytes from the helper's pipe, n of them: 1 when a frame is whole (*kind, and r->body holding *len bytes and a
   NUL, until the next call), *used of in taken; 0 when every byte was taken and no frame is whole; -1 when they
   are no frames (the window ends the connection). memset() starts a struct link_reader. */
int link_read(struct link_reader *r, const char *in, int n, int *used, int *kind, int *len);

/* a frame (kind, len bytes of data) into out (size bytes): its length, or -1 when it does not fit or len is more
   than its kind may have */
int link_frame(char *out, int size, int kind, const void *data, int len);

/* Undertow's transport speaks of web pages: its sentence why as the Chat says it into out (size bytes): "TLS"
   for "HTTPS", and without the advice about http:// addresses and Undertow's clock command, which are not here */
void link_words(char *out, int size, const char *why);

#endif
