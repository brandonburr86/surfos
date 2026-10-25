/*
SurfOS downloader: fetch
--------------------
File: fetch.h   Date: 10/24/26
--------------------
fetch downloads a web address (http:// or https://) into a file. The connection is
Undertow's (user/undertow/transport.c: TCP, or TLS with the certificate checked, never
skipped); the response is read here, as it comes, into the file. This header is the
part that makes no system calls (fetchhttp.c, host-tested in tests/host/test_fetch.c):
the response head, read as strictly as Undertow reads it (RFC 9112: obsolete line
folding, a CR inside a line, a blank before a colon, two different lengths, a length
together with Transfer-Encoding or Transfer-Encoding in an HTTP/1.0 response, and any
transfer coding but chunked are refused), the chunked coding undone a piece at a time,
Content-Range, the name a download is saved under, and sizes as text.
*/

#ifndef _FETCH_H
#define _FETCH_H

#define FX_HEAD_MAX   32768             /* a response head, its status line and header lines (Undertow's UT_HEADERS_MAX) */
#define FX_LOCATION   2049              /* a Location kept: Undertow's UT_URL_MAX bytes and the NUL */
#define FX_CHUNK_LINE 4096              /* a chunk's size line with its extensions (Undertow's CHUNK_LINE_MAX) */
#define FX_NAME_MAX   256               /* a file name made from an address, the NUL included */
#define FX_FILE_MAX   0x7fffffffL       /* the largest file: SurfOS's offsets are 31 bits */

struct fx_head {
    int status;                         /* 100..599 */
    int minor;                          /* HTTP/1.<minor> */
    char reason[64];                    /* the status line's text, what is not printable ASCII as '?' */
    long length;                        /* Content-Length, -1 when none */
    int chunked;                        /* Transfer-Encoding: chunked */
    int encoded;                        /* a Content-Encoding other than identity: the bytes are saved as they come */
    char encoding[32];                  /* its text, as the reason */
    long range_start, range_end;        /* Content-Range: bytes a-b/c; -1 when none, or for "bytes star/c" */
    long range_total;                   /* c; -1 when none or a star */
    int locations;                      /* Location fields */
    int location_long;                  /* one with FX_LOCATION bytes or more */
    int location_conflict;              /* two that differ */
    char location[FX_LOCATION];         /* the first, "" when none (or too long) */
};

/* The length of the response head at the start of buf (len bytes): through the empty line that ends
   it; 0 while it is not whole yet; -1 when FX_HEAD_MAX bytes came and it did not end. */
int fx_head_end(const char *buf, int len);

/* The head buf (len bytes, as fx_head_end() measured it) into h: 0, or -1 and *err, a sentence that
   ends without a full stop ("the response's Content-Length is malformed"). */
int fx_parse_head(const char *buf, int len, struct fx_head *h, const char **err);

/* the chunked transfer coding (RFC 9112 section 7.1), undone as the bytes come */
struct fx_chunks {
    int state;
    unsigned long left;                 /* bytes of the chunk still to come */
    int digits;                         /* digits of its size */
    int line;                           /* bytes of its size line, or of a trailer line */
    int trailer;                        /* bytes of the trailer */
};

/* Takes bytes of in (n of them): the body's bytes go to out (room for n bytes; *got of them), the rest
   is framing. Returns how many bytes of in it took: all of them, or fewer once the coding has ended
   (*done 1 after the last chunk and its trailer), what comes behind it left; -1 when the coding is
   broken (a size that is no hex number, of 2 GB or more, a size line over FX_CHUNK_LINE bytes, a chunk
   without its line end, a trailer over FX_HEAD_MAX bytes). memset() starts a struct fx_chunks. */
int fx_chunks_feed(struct fx_chunks *c, const char *in, int n, char *out, int *got, int *done);

/* The name to save a download under: the last name of the address's path (path from its '/', any query
   or fragment left out), percent-decoded, with '_' for the characters a FAT name cannot hold (\ / : * ?
   " < > | and controls); "index.html" when that leaves nothing, ".", or "..". */
void fx_name_of(const char *path, char *out, int size);

/* bytes as "512 bytes", "1.5 KB", "12.3 MB", "1.2 GB" (powers of 1024) into out */
void fx_size(char *out, int size, unsigned long bytes);

#endif
