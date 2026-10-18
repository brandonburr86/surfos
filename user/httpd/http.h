/*
SurfOS web server: httpd
--------------------
File: http.h    Date: 10/17/26
--------------------
The HTTP/1.1 protocol of httpd (http.c): the request head, the path it names, the
response header, error pages and folder listings. http.c makes no system calls, so it is
tested on the host as it is (tests/host/test_httpd.c); the server (server.c) does the
reading, the files and the writing.
*/

#ifndef _HTTP_H
#define _HTTP_H

#define HTTP_HEAD_MAX 4096              /* a request head: the request line and the header fields */
#define HTTP_PATH_MAX 256               /* a decoded path with its NUL; a file's path too (VFS_PATH_MAX) */
#define HTTP_TAG_MAX  128               /* the If-None-Match value kept */
#define HTTP_SERVER   "SurfOS-httpd/1.0"

enum { HTTP_GET = 1, HTTP_HEAD, HTTP_OTHER };

struct http_req {
    int method;                         /* HTTP_GET, HTTP_HEAD or HTTP_OTHER */
    char method_name[12];               /* as sent (cut short), for the request log */
    char target[64];                    /* the request target as sent (cut short, controls as ?), for the log */
    int minor;                          /* HTTP/1.<minor> */
    int keep;                           /* the connection may stay open after the response (keep-alive) */
    int body;                           /* the request has a body, which is never read: the connection closes */
    int slash;                          /* the path ended in '/' */
    char path[HTTP_PATH_MAX];           /* decoded and normalised: "/" or "/a/b", never a '/' at its end */
    char tag[HTTP_TAG_MAX];             /* If-None-Match as sent, "" for none (or one too long to keep) */
};

struct http_resp {
    int status;
    const char *type;                   /* Content-Type; NULL: none */
    long length;                        /* Content-Length; -1: none */
    const char *etag;                   /* ETag, with its quotes; NULL: none */
    const char *location;               /* Location (a redirect); NULL: none */
    int keep;                           /* Connection: keep-alive, or close */
};

/* a folder's entry, for a listing */
struct http_entry {
    const char *name;
    int dir;
    unsigned int size, mtime;
};

/* The length of the request head at the start of buf (len bytes): up to and with the empty line that ends
   it; 0 while it is not complete. Empty lines before the request line are part of the head. */
int http_head_end(const char *buf, int len);

/* Parses the request head buf (len bytes, as http_head_end() measured it) into req: 0, or the status to
   answer with (400, 404, 405, 414, 505). req->keep and the fields the log uses are set either way, as far
   as the head was read. */
int http_parse(const char *buf, int len, struct http_req *req);

/* The status line and header fields of r into out (size bytes, NUL-terminated): their length; -1 when
   they do not fit. The text of the status (reason phrase): */
int http_header(char *out, int size, const struct http_resp *r);
const char *http_reason(int status);

/* The content type for a file name, by its extension (case does not matter) */
const char *http_type(const char *name);

/* The ETag of a file of size bytes changed at mtime: "<size>-<mtime>" in hex, with the quotes */
void http_etag(char *out, int size, unsigned int filesize, unsigned int mtime);
/* Does an If-None-Match value hold etag (or "*")? */
int http_etag_match(const char *header, const char *etag);

/* path with every byte percent-encoded but letters, digits, - . _ ~ and '/', into out: 0, or -1 when
   it does not fit */
int http_encode_path(char *out, int size, const char *path);

/* A small HTML page for an error status into out: its length (cut to fit) */
int http_error_page(char *out, int size, int status);

/* Folders first, then by name */
void http_sort(struct http_entry *e, int n);
/* The listing page of folder path ("/" or "/a/b") with its n entries (more: how many were left out) into a
   buffer from malloc() (*out, which the caller frees): its length; -1 without memory */
int http_listing(const char *path, const struct http_entry *e, int n, int more, char **out);

/* An absolute path ("/...") without "." and ".." (one above the root stays at the root), repeated or
   trailing slashes: "/" or "/a/b", into out: 0, or -1 when path is not absolute or it does not fit */
int http_normalize(const char *path, char *out, int size);

#endif
