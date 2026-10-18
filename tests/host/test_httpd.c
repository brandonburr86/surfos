/* Host-side unit tests for user/httpd/http.c, httpd's HTTP protocol: the end of a request head
   (CR LF and LF, empty lines before it, every prefix of a head, two heads one behind the other);
   the request line (methods, versions, spaces, the origin and absolute forms, the query cut off);
   the path (percent-decoding, "." and empty names, ".." refused also encoded, controls and
   backslashes refused, dot names hidden, UTF-8 kept, too long); the header fields (Host once in
   HTTP/1.1, Connection's tokens, keep-alive in HTTP/1.0, Content-Length and Transfer-Encoding as a
   body, If-None-Match, blanks before a colon, obs-fold, controls); the log's copies of the method
   and target; the response header's bytes; content types; ETags and If-None-Match lists; encoded
   paths; error pages; folder listings (order, escaping, encoded links, sizes, dates, the parent
   link, entries left out); and folder arguments made absolute. Built from the same file as the
   program and run by `make unittest`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "http.h"

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

/* the status http_parse() gives the head s (whole) */
static int parse(const char *s, struct http_req *req) {
    return http_parse(s, (int)strlen(s), req);
}

/* s as a GET of target with a Host field: its status and path */
static int get(const char *target, struct http_req *req) {
    char head[2048];
    snprintf(head, sizeof(head), "GET %s HTTP/1.1\r\nHost: surf\r\n\r\n", target);
    return parse(head, req);
}

static void test_head_end(void) {
    const char *h = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
    char two[128];
    int n = (int)strlen(h), i, early = 0;
    check(http_head_end(h, n) == n, "head end: CR LF CR LF");
    for(i = 0; i < n; i++) if(http_head_end(h, i)) early = 1;
    check(!early, "head end: no prefix of a head is whole");
    check(http_head_end("GET / HTTP/1.0\n\n", 16) == 16, "head end: LF LF");
    check(http_head_end("GET / HTTP/1.0\nA: b\n\r\n", 22) == 22, "head end: LF CR LF");
    check(http_head_end("\r\n\r\nGET / HTTP/1.0\r\n\r\n", 22) == 22, "head end: empty lines before the request line");
    check(http_head_end("\r\n\r\n", 4) == 0, "head end: empty lines alone are no head");
    check(http_head_end("", 0) == 0, "head end: nothing");
    snprintf(two, sizeof(two), "%s%s", h, "GET /b HTTP/1.1\r\nHost: a\r\n\r\n");
    check(http_head_end(two, (int)strlen(two)) == n, "head end: the first of two heads");
}

static void test_request_line(void) {
    struct http_req r;
    check(parse("GET / HTTP/1.1\r\nHost: a\r\n\r\n", &r) == 0 && r.method == HTTP_GET && r.minor == 1 && r.keep &&
          !strcmp(r.path, "/") && !r.body, "GET / HTTP/1.1 with Host: 0, keep-alive");
    check(parse("HEAD /x HTTP/1.1\r\nHost: a\r\n\r\n", &r) == 0 && r.method == HTTP_HEAD && !strcmp(r.path, "/x"), "HEAD");
    check(parse("POST /form HTTP/1.1\r\nHost: a\r\n\r\n", &r) == 405 && r.method == HTTP_OTHER &&
          !strcmp(r.method_name, "POST") && !strcmp(r.target, "/form"), "POST: 405, method and target kept for the log");
    check(parse("get / HTTP/1.1\r\nHost: a\r\n\r\n", &r) == 405, "methods are case-sensitive: get is another method");
    check(parse("OPTIONS * HTTP/1.1\r\nHost: a\r\n\r\n", &r) == 405, "OPTIONS *: 405");
    check(parse("GET * HTTP/1.1\r\nHost: a\r\n\r\n", &r) == 400, "GET *: 400");
    check(parse("GET / HTTP/2.0\r\nHost: a\r\n\r\n", &r) == 505, "HTTP/2.0: 505");
    check(parse("HEAD / HTTP/2.0\r\nHost: a\r\n\r\n", &r) == 505 && r.method == HTTP_HEAD && !r.keep,
          "HEAD with HTTP/2.0: 505, known as HEAD (no body for it)");
    check(parse("HEAD / HTTP/1.x\r\nHost: a\r\n\r\n", &r) == 400 && r.method == HTTP_HEAD, "HEAD with a bad version: 400, HEAD");
    check(parse("GET / HTTP/0.9\r\n\r\n", &r) == 505, "HTTP/0.9: 505");
    check(parse("GET / HTTP/1.2\r\nHost: a\r\n\r\n", &r) == 0 && r.minor == 2 && r.keep, "HTTP/1.2 is served as 1.x");
    check(parse("GET / HTTP/1.1x\r\nHost: a\r\n\r\n", &r) == 400, "HTTP/1.1x: 400");
    check(parse("GET / HTTP/11\r\nHost: a\r\n\r\n", &r) == 400, "HTTP/11: 400");
    check(parse("GET / http/1.1\r\nHost: a\r\n\r\n", &r) == 400, "http/1.1 in lower case: 400");
    check(parse("GET /\r\n\r\n", &r) == 400, "no version: 400");
    check(parse("GET  / HTTP/1.1\r\nHost: a\r\n\r\n", &r) == 400, "two spaces: 400");
    check(parse("GET / HTTP/1.1 \r\nHost: a\r\n\r\n", &r) == 400, "a space after the version: 400");
    check(parse("G(T / HTTP/1.1\r\nHost: a\r\n\r\n", &r) == 400, "a method that is no token: 400");
    check(parse("\r\n\r\nGET /after HTTP/1.1\r\nHost: a\r\n\r\n", &r) == 0 && !strcmp(r.path, "/after"),
          "empty lines before the request line are skipped");
    check(parse("GET /lf HTTP/1.1\nHost: a\n\n", &r) == 0 && !strcmp(r.path, "/lf"), "LF line ends");
    check(parse("GET /a\rb HTTP/1.1\r\nHost: a\r\n\r\n", &r) == 400, "a CR in the target: 400");
    check(http_parse("GET /\0 HTTP/1.1\r\nHost: a\r\n\r\n", 28, &r) == 400, "a NUL in the target: 400");
    check(parse("GET a/b HTTP/1.1\r\nHost: a\r\n\r\n", &r) == 400, "a target without its '/': 400");
    check(parse("GET https://h/x HTTP/1.1\r\nHost: a\r\n\r\n", &r) == 400, "https:// absolute form: 400");
}

static void test_targets(void) {
    struct http_req r;
    char t[400];
    check(get("/a/b?x=1&y=/../..#frag", &r) == 0 && !strcmp(r.path, "/a/b") && !r.slash, "the query is cut off, even with ..");
    check(get("/dir/", &r) == 0 && !strcmp(r.path, "/dir") && r.slash, "a trailing slash is noted, not kept");
    check(get("/dir", &r) == 0 && !r.slash, "no trailing slash");
    check(get("/dir%2F", &r) == 0 && !strcmp(r.path, "/dir") && r.slash, "an encoded trailing slash");
    check(get("http://surf:80/a/", &r) == 0 && !strcmp(r.path, "/a") && r.slash, "absolute form");
    check(get("HTTP://surf", &r) == 0 && !strcmp(r.path, "/"), "absolute form without a path, scheme in capitals");
    check(get("//a//b//", &r) == 0 && !strcmp(r.path, "/a/b") && r.slash, "repeated slashes");
    check(get("/a/./b/.", &r) == 0 && !strcmp(r.path, "/a/b"), "\".\" names are dropped");
    check(get("/a%20b", &r) == 0 && !strcmp(r.path, "/a b"), "%20");
    check(get("/%41%62", &r) == 0 && !strcmp(r.path, "/Ab"), "%41%62");
    check(get("/caf%C3%A9.html", &r) == 0 && !strcmp(r.path, "/caf\xc3\xa9.html"), "UTF-8 kept");
    check(get("/a%2Fb", &r) == 0 && !strcmp(r.path, "/a/b"), "%2F is a slash");
    check(get("/a+b", &r) == 0 && !strcmp(r.path, "/a+b"), "+ stays a plus in a path");
    check(get("/%zz", &r) == 400, "%zz: 400");
    check(get("/%2", &r) == 400, "%2 cut short: 400");
    check(get("/a%", &r) == 400, "% at the end: 400");
    check(get("/%00", &r) == 400, "%00: 400");
    check(get("/a%0d%0a", &r) == 400, "encoded CR LF: 400");
    check(get("/a%7f", &r) == 400, "DEL: 400");
    check(get("/a\\b", &r) == 400, "a backslash: 400");
    check(get("/a%5cb", &r) == 400, "an encoded backslash: 400");
    check(get("/../etc", &r) == 400, "/../: 400");
    check(get("/a/../b", &r) == 400, "a/../b: 400 (browsers resolve it first)");
    check(get("/a/..", &r) == 400, "/a/..: 400");
    check(get("/%2e%2e/x", &r) == 400, "%2e%2e: 400");
    check(get("/a/%2E%2E", &r) == 400, "%2E%2E at the end: 400");
    check(get("/..%2f..%2fetc", &r) == 400, "..%2f: 400");
    check(get("/.%2e/x", &r) == 400, ".%2e: 400");
    check(get("/...", &r) == 404, "... is a dot name: hidden (404)");
    check(get("/.git/config", &r) == 404, ".git: hidden (404)");
    check(get("/._index.html", &r) == 404, "._ (macOS): hidden");
    check(get("/a/.DS_Store", &r) == 404, ".DS_Store: hidden");
    check(get("/%2Ehidden", &r) == 404, "an encoded dot name: hidden");
    check(get("/a.b/c.d", &r) == 0 && !strcmp(r.path, "/a.b/c.d"), "dots inside names are plain");
    memset(t, 'n', sizeof(t));
    t[0] = '/';
    t[300] = 0;
    check(get(t, &r) == 414, "a 299-byte name: 414");
    t[255] = 0;
    check(get(t, &r) == 0 && strlen(r.path) == 255, "a 255-byte path fits");
    t[255] = 'n';
    t[256] = 0;
    check(get(t, &r) == 414, "a 256-byte path: 414 (no room for its NUL)");
    t[255] = 0;
    strcat(t, "/");
    check(get(t, &r) == 0 && strlen(r.path) == 255 && r.slash, "a 255-byte path with its slash fits");
    t[255] = 'n';
    t[256] = 'n';
    t[257] = 0;
    check(get(t, &r) == 414, "a 257-byte path: 414");
}

static void test_fields(void) {
    struct http_req r;
    char head[512];
    check(parse("GET / HTTP/1.1\r\n\r\n", &r) == 400, "HTTP/1.1 without Host: 400");
    check(parse("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", &r) == 400, "two Host fields: 400");
    check(parse("GET / HTTP/1.0\r\nHost: a\r\nHost: b\r\n\r\n", &r) == 400, "two Host fields in HTTP/1.0: 400");
    check(parse("GET / HTTP/1.0\r\n\r\n", &r) == 0 && !r.keep, "HTTP/1.0 without Host: 0, the connection closes");
    check(parse("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", &r) == 0 && r.keep, "HTTP/1.0 keep-alive");
    check(parse("GET / HTTP/1.0\r\nConnection: keep-alive, close\r\n\r\n", &r) == 0 && !r.keep,
          "HTTP/1.0 keep-alive and close: close wins");
    check(parse("GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n", &r) == 0 && !r.keep, "Connection: close");
    check(parse("GET / HTTP/1.1\r\nhost: a\r\nCONNECTION: Upgrade , CLOSE\r\n\r\n", &r) == 0 && !r.keep,
          "field names and tokens in any case, close in a list");
    check(parse("GET / HTTP/1.1\r\nHost: a\r\nConnection: closed\r\n\r\n", &r) == 0 && r.keep, "closed is not close");
    check(parse("GET / HTTP/1.1\r\nHost : a\r\n\r\n", &r) == 400, "a blank before the colon: 400");
    check(parse("GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n", &r) == 400, "obs-fold: 400");
    check(parse("GET / HTTP/1.1\r\nHost a\r\n\r\n", &r) == 400, "a field without a colon: 400");
    check(parse("GET / HTTP/1.1\r\n: a\r\nHost: a\r\n\r\n", &r) == 400, "an empty field name: 400");
    check(parse("GET / HTTP/1.1\r\nHost: a\r\nX: a\x01" "b\r\n\r\n", &r) == 400, "a control in a value: 400");
    check(parse("GET / HTTP/1.1\r\nHost: a\r\nX: a\tb\r\n\r\n", &r) == 0, "a tab in a value is fine");
    check(parse("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: abc\r\n\r\n", &r) == 400, "Content-Length: abc: 400");
    check(parse("GET / HTTP/1.1\r\nHost: a\r\nContent-Length:\r\n\r\n", &r) == 400, "an empty Content-Length: 400");
    check(parse("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n", &r) == 400, "Content-Length: -1: 400");
    check(parse("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n", &r) == 0 && !r.body && r.keep, "Content-Length: 0");
    check(parse("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\n", &r) == 0 && r.body && !r.keep,
          "a body: never read, the connection closes");
    check(parse("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n", &r) == 0 && r.body,
          "the same Content-Length twice");
    check(parse("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n", &r) == 400,
          "two Content-Lengths that differ: 400");
    check(parse("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 99999999999999\r\n\r\n", &r) == 400, "a huge Content-Length: 400");
    check(parse("GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", &r) == 0 && r.body && !r.keep,
          "Transfer-Encoding: a body");
    check(parse("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\n\r\n", &r) == 405 && !r.keep, "POST with a body: 405, closes");
    check(parse("GET / HTTP/1.1\r\nHost: a\r\nIf-None-Match: \"1-2\"  \r\n\r\n", &r) == 0 && !strcmp(r.tag, "\"1-2\""),
          "If-None-Match kept, blanks trimmed");
    check(parse("GET / HTTP/1.1\r\nHost: a\r\nIf-None-Match: \"1-2\"\r\nIf-None-Match: \"3-4\"\r\n\r\n", &r) == 0 && !r.tag[0],
          "two If-None-Match fields: none counts");
    memset(head, 0, sizeof(head));
    strcpy(head, "GET / HTTP/1.1\r\nHost: a\r\nIf-None-Match: ");
    memset(head + strlen(head), 'x', 200);
    strcat(head, "\r\n\r\n");
    check(parse(head, &r) == 0 && !r.tag[0], "a long If-None-Match: none counts");
    check(parse("GET /\x01" "x HTTP/1.1\r\nHost: a\r\n\r\n", &r) == 400 && !strcmp(r.target, "/?x"), "the log's target: controls as ?");
    memset(head, 0, sizeof(head));
    strcpy(head, "GET /");
    memset(head + 5, 'y', 100);
    strcat(head, " HTTP/1.1\r\nHost: a\r\n\r\n");
    check(parse(head, &r) == 0 && strlen(r.target) == 63, "the log's target is cut short");
    check(parse("VERYLONGMETHODNAME / HTTP/1.1\r\nHost: a\r\n\r\n", &r) == 405 && strlen(r.method_name) == 11,
          "the log's method is cut short");
}

static void test_header(void) {
    char out[512];
    struct http_resp ok = { 200, "text/css; charset=utf-8", 42, "\"2a-5\"", NULL, 1 };
    struct http_resp nm = { 304, NULL, -1, "\"2a-5\"", NULL, 0 };
    struct http_resp mv = { 301, NULL, 0, NULL, "/dir/", 1 };
    struct http_resp na = { 405, "text/html; charset=utf-8", 10, NULL, NULL, 0 };
    int n = http_header(out, sizeof(out), &ok);
    check(n == (int)strlen(out) && !strcmp(out, "HTTP/1.1 200 OK\r\nServer: SurfOS-httpd/1.0\r\n"
          "Content-Type: text/css; charset=utf-8\r\nContent-Length: 42\r\nETag: \"2a-5\"\r\n"
          "Cache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n"), "200: the header's bytes");
    http_header(out, sizeof(out), &nm);
    check(!strcmp(out, "HTTP/1.1 304 Not Modified\r\nServer: SurfOS-httpd/1.0\r\nETag: \"2a-5\"\r\n"
          "Cache-Control: no-cache\r\nConnection: close\r\n\r\n"), "304: no type, no length");
    http_header(out, sizeof(out), &mv);
    check(strstr(out, "HTTP/1.1 301 Moved Permanently\r\n") == out && strstr(out, "\r\nLocation: /dir/\r\n") &&
          strstr(out, "\r\nContent-Length: 0\r\n"), "301: Location, an empty body");
    http_header(out, sizeof(out), &na);
    check(strstr(out, "\r\nAllow: GET, HEAD\r\n") != NULL, "405: Allow");
    check(http_header(out, 40, &ok) == -1, "a header that does not fit: -1");
    check(!strcmp(http_reason(431), "Request Header Fields Too Large") && !strcmp(http_reason(599), "Unknown"), "reasons");
}

static void test_types(void) {
    check(!strcmp(http_type("/www/index.html"), "text/html; charset=utf-8"), "html");
    check(!strcmp(http_type("INDEX.HTM"), "text/html; charset=utf-8"), "HTM in capitals");
    check(!strcmp(http_type("site/Style.CSS"), "text/css; charset=utf-8"), "CSS");
    check(!strcmp(http_type("app.js"), "text/javascript; charset=utf-8"), "js");
    check(!strcmp(http_type("logo.svg"), "image/svg+xml"), "svg");
    check(!strcmp(http_type("photo.JPEG"), "image/jpeg"), "JPEG");
    check(!strcmp(http_type("font.woff2"), "font/woff2"), "woff2");
    check(!strcmp(http_type("a.tar.gz"), "application/octet-stream"), "unknown: octet-stream");
    check(!strcmp(http_type("README"), "application/octet-stream"), "no extension");
    check(!strcmp(http_type("dir.css/file"), "application/octet-stream"), "a dot in a folder's name only");
    check(!strcmp(http_type("trailing."), "application/octet-stream"), "a trailing dot");
}

static void test_etags(void) {
    char tag[32];
    http_etag(tag, sizeof(tag), 0x1234, 0x5678abcd);
    check(!strcmp(tag, "\"1234-5678abcd\""), "etag: size and time in hex, quoted");
    check(http_etag_match("\"1234-5678abcd\"", tag), "If-None-Match: the tag");
    check(http_etag_match("\"x\", W/\"1234-5678abcd\"", tag), "If-None-Match: a weak tag in a list");
    check(http_etag_match(" *", tag), "If-None-Match: *");
    check(!http_etag_match("\"11234-5678abcd\"", tag), "If-None-Match: a longer tag is another");
    check(!http_etag_match("\"1234-5678abcd0\"", tag), "If-None-Match: a tag it begins is another");
    check(!http_etag_match("", tag), "If-None-Match: empty");
}

static void test_encode(void) {
    char out[64];
    check(http_encode_path(out, sizeof(out), "/a b/caf\xc3\xa9") == 0 && !strcmp(out, "/a%20b/caf%C3%A9"), "encode: space, UTF-8");
    check(http_encode_path(out, sizeof(out), "/?#%&+\"<'") == 0 && !strcmp(out, "/%3F%23%25%26%2B%22%3C%27"), "encode: reserved");
    check(http_encode_path(out, sizeof(out), "/A-z_0.9~") == 0 && !strcmp(out, "/A-z_0.9~"), "encode: plain stays");
    check(http_encode_path(out, 5, "/abcd") == -1 && http_encode_path(out, 6, "/abcd") == 0, "encode: the room it needs");
    check(http_encode_path(out, 4, "/ ") == -1, "encode: an escape that does not fit");
}

static void test_error_page(void) {
    char out[1024];
    int n = http_error_page(out, sizeof(out), 404);
    check(n == (int)strlen(out) && strstr(out, "<title>404 Not Found</title>") && strstr(out, "<h1>404 Not Found</h1>") &&
          strstr(out, "</html>"), "404 page");
    check(n > 512, "error pages are over 512 bytes (Chrome shows its own in place of shorter ones)");
    n = http_error_page(out, sizeof(out), 431);
    check(n > 512 && n < 1000 && strstr(out, "431 Request Header Fields Too Large"), "the longest reason fits the server's buffer");
    n = http_error_page(out, 64, 500);
    check(n == 63 && (int)strlen(out) == 63, "an error page cut to fit");
}

static void test_listing(void) {
    struct http_entry e[] = {
        { "zeta.txt", 0, 512, 0 },
        { "Beta", 1, 0, 1760000000u },
        { "<b>&\"x\".html", 0, 1536, 0 },
        { "alpha", 1, 0, 0 },
        { "my page.html", 0, 2u * 1024 * 1024 + 300 * 1024, 0 },
        { "Zebra.css", 0, 1023, 0 },
    };
    char *page = NULL;
    const char *a, *b, *beta, *z, *x;
    int n;
    http_sort(e, 6);
    check(!strcmp(e[0].name, "alpha") && !strcmp(e[1].name, "Beta") && e[2].name[0] == '<' &&
          !strcmp(e[3].name, "my page.html") && !strcmp(e[4].name, "Zebra.css") && !strcmp(e[5].name, "zeta.txt"),
          "sort: folders first, then names in any case");
    n = http_listing("/sites/a&b", e, 6, 0, &page);
    check(n > 0 && page && (int)strlen(page) == n, "listing made");
    if(!page) return;
    check(strstr(page, "<title>Index of /sites/a&amp;b/</title>") != NULL, "listing: the title, escaped");
    check(strstr(page, "<a href=\"../\">../</a>") != NULL, "listing: the parent");
    a = strstr(page, "href=\"alpha/\">alpha/</a>");
    beta = strstr(page, "href=\"Beta/\">Beta/</a>");
    x = strstr(page, "href=\"%3Cb%3E%26%22x%22.html\">&lt;b&gt;&amp;&quot;x&quot;.html</a>");
    b = strstr(page, "href=\"my%20page.html\">my page.html</a>");
    z = strstr(page, "href=\"Zebra.css\">Zebra.css</a>");
    check(a && beta && x && b && z && a < beta && beta < x && x < b && b < z, "listing: links encoded, names escaped, in order");
    check(strstr(page, ">512 B<") && strstr(page, ">1.5 KB<") && strstr(page, ">2.2 MB<") && strstr(page, ">1023 B<"),
          "listing: sizes");
    check(strstr(page, "2025-10-09 08:53") != NULL, "listing: a time");
    check(!strstr(page, "more entries"), "listing: none left out");
    free(page);
    page = NULL;
    n = http_listing("/", e, 0, 3, &page);
    check(n > 0 && page && !strstr(page, "href=\"../\"") && strstr(page, "<title>Index of /</title>") &&
          strstr(page, "3 more entries are not listed") && !strstr(page, "is empty"), "listing of the root: no parent, 3 left out");
    free(page);
    page = NULL;
    n = http_listing("/empty", e, 0, 0, &page);
    check(n > 0 && page && strstr(page, "This folder is empty."), "listing of an empty folder");
    free(page);
}

static void test_normalize(void) {
    char out[HTTP_PATH_MAX], longp[400];
    check(http_normalize("/initrd/www", out, sizeof(out)) == 0 && !strcmp(out, "/initrd/www"), "normalize: as it is");
    check(http_normalize("/data/./www/../site/", out, sizeof(out)) == 0 && !strcmp(out, "/data/site"), "normalize: . and ..");
    check(http_normalize("/..", out, sizeof(out)) == 0 && !strcmp(out, "/"), "normalize: .. at the root");
    check(http_normalize("/../../x//y", out, sizeof(out)) == 0 && !strcmp(out, "/x/y"), "normalize: above the root");
    check(http_normalize("//", out, sizeof(out)) == 0 && !strcmp(out, "/"), "normalize: //");
    check(http_normalize("/a/b/../../..", out, sizeof(out)) == 0 && !strcmp(out, "/"), "normalize: back to the root");
    check(http_normalize("/a/..b", out, sizeof(out)) == 0 && !strcmp(out, "/a/..b"), "normalize: ..b is a name");
    check(http_normalize("www", out, sizeof(out)) == -1, "normalize: relative: -1");
    memset(longp, 'p', sizeof(longp));
    longp[0] = '/';
    longp[sizeof(longp) - 1] = 0;
    check(http_normalize(longp, out, sizeof(out)) == -1, "normalize: too long: -1");
}

int main(void) {
    test_head_end();
    test_request_line();
    test_targets();
    test_fields();
    test_header();
    test_types();
    test_etags();
    test_encode();
    test_error_page();
    test_listing();
    test_normalize();
    printf("test_httpd: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
