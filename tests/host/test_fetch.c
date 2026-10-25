/* Host-side unit tests for user/fetch/fetchhttp.c, what fetch reads of an HTTP response: the end of a head (CR LF
   and LF, every prefix, a head too long), the status line (versions, codes, the reason made safe, a NUL or a CR in
   it), the fields as Undertow reads them (Content-Length once, twice the same, as a list, twice different, not a
   number, 2 GB; chunked alone, twice, after another coding, empty, in an HTTP/1.0 response, with a length;
   Content-Encoding; Location once, twice the same, twice different, too long; Content-Range in its three forms
   and broken ones; obsolete line folding, a CR or a NUL inside a line, a field name with a blank or another
   character that is no token), the chunked coding in every piece size and at random (extensions, trailers, LF
   alone, sizes in either case, the end and what comes behind it, broken framing, a size of 2 GB, a size line
   or a trailer too long), the names a download is saved under, and sizes as text. Built from the same file as
   the program and run by `make unittest`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fetch.h"

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

static const char *why;

/* the head s (a C string) parsed */
static int parse(const char *s, struct fx_head *h) {
    why = NULL;
    return fx_parse_head(s, (int)strlen(s), h, &why);
}

/* the head s of n bytes (NULs in it) parsed */
static int parse_n(const char *s, int n, struct fx_head *h) {
    why = NULL;
    return fx_parse_head(s, n, h, &why);
}

#define PARSE_LIT(lit, h) parse_n(lit, (int)sizeof(lit) - 1, h)

static void test_head_end(void) {
    const char *s = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc";
    static char big[FX_HEAD_MAX + 10];
    int n = (int)strlen(s) - 3, i, early = 0;
    check(fx_head_end(s, (int)strlen(s)) == n, "head end: CR LF CR LF, the body after it");
    for(i = 0; i < n; i++) if(fx_head_end(s, i)) early = 1;
    check(!early, "head end: no prefix of a head is whole");
    check(fx_head_end("HTTP/1.0 200 OK\n\n", 17) == 17, "head end: LF LF");
    check(fx_head_end("HTTP/1.0 200 OK\nA: b\n\r\nx", 24) == 23, "head end: LF CR LF");
    memset(big, 'x', sizeof(big));
    check(fx_head_end(big, FX_HEAD_MAX - 1) == 0 && fx_head_end(big, FX_HEAD_MAX) == -1, "head end: too long");
}

static void test_status(void) {
    struct fx_head h;
    check(parse("HTTP/1.1 200 OK\r\n\r\n", &h) == 0 && h.status == 200 && h.minor == 1 && !strcmp(h.reason, "OK") &&
          h.length == -1 && !h.chunked && !h.encoded && h.range_start == -1 && h.range_total == -1 && !h.locations &&
          !h.location[0] && why == NULL, "status: 200 OK, nothing else");
    check(parse("HTTP/1.0 404 Not Found\n\n", &h) == 0 && h.status == 404 && h.minor == 0 &&
          !strcmp(h.reason, "Not Found"), "status: HTTP/1.0 404 with LF alone");
    check(parse("HTTP/1.1 204\r\n\r\n", &h) == 0 && h.status == 204 && !h.reason[0], "status: no reason");
    check(parse("HTTP/1.1 200 a\x1b[2Jb\xc3\xa9\r\n\r\n", &h) == 0 && !strcmp(h.reason, "a?[2Jb??"),
          "status: what is not printable ASCII in the reason is ?");
    check(parse("HTTP/2 200 OK\r\n\r\n", &h) == -1 && parse("HTTP/1.1 99 X\r\n\r\n", &h) == -1 &&
          parse("HTTP/1.1 600 X\r\n\r\n", &h) == -1 && parse("HTTP/1.1 20a X\r\n\r\n", &h) == -1 &&
          parse("http/1.1 200 OK\r\n\r\n", &h) == -1 && parse("HTTP/1.1 2000 OK\r\n\r\n", &h) == -1 &&
          parse("HTTP/1.1  200 OK\r\n\r\n", &h) == -1 && parse("\r\nHTTP/1.1 200 OK\r\n\r\n", &h) == -1 &&
          parse("", &h) == -1, "status: what is no HTTP/1.x status line");
    check(why && strstr(why, "status line"), "status: a refusal says why");
    check(parse("HTTP/1.1 200 O\rK\r\n\r\n", &h) == -1 && PARSE_LIT("HTTP/1.1 200 O\0K\r\n\r\n", &h) == -1,
          "status: a CR or a NUL inside the line");
}

static void test_fields(void) {
    struct fx_head h;
    static char loc[FX_LOCATION + 64];
    check(parse("HTTP/1.1 200 OK\r\nContent-Length: 1234\r\n\r\n", &h) == 0 && h.length == 1234, "length");
    check(parse("HTTP/1.1 200 OK\r\ncontent-LENGTH:   5  \r\n\r\n", &h) == 0 && h.length == 5, "length: any case, blanks");
    check(parse("HTTP/1.1 200 OK\r\nContent-Length: 7\r\nContent-Length: 7\r\n\r\n", &h) == 0 && h.length == 7,
          "length: twice the same");
    check(parse("HTTP/1.1 200 OK\r\nContent-Length: 7, 7 ,7\r\n\r\n", &h) == 0 && h.length == 7,
          "length: a list of the same value");
    check(parse("HTTP/1.1 200 OK\r\nContent-Length: 7\r\nContent-Length: 8\r\n\r\n", &h) == -1 &&
          strstr(why, "two different") && parse("HTTP/1.1 200 OK\r\nContent-Length: 7, 8\r\n\r\n", &h) == -1,
          "length: two different values");
    check(parse("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n", &h) == -1 &&
          parse("HTTP/1.1 200 OK\r\nContent-Length: 1 2\r\n\r\n", &h) == -1 &&
          parse("HTTP/1.1 200 OK\r\nContent-Length: 12abc\r\n\r\n", &h) == -1 &&
          parse("HTTP/1.1 200 OK\r\nContent-Length: 7,\r\n\r\n", &h) == -1 &&
          parse("HTTP/1.1 200 OK\r\nContent-Length:\r\n\r\n", &h) == -1 && strstr(why, "malformed"),
          "length: malformed");
    check(parse("HTTP/1.1 200 OK\r\nContent-Length: 2147483648\r\n\r\n", &h) == -1 && strstr(why, "2 GB") &&
          parse("HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999\r\n\r\n", &h) == -1 && strstr(why, "2 GB"),
          "length: 2 GB or more");
    check(parse("HTTP/1.1 200 OK\r\nContent-Length: 2147483647\r\n\r\n", &h) == 0 && h.length == 2147483647L,
          "length: the last below 2 GB");
    check(parse("HTTP/1.1 200 OK\r\nContent-Length: 4294967296\r\n\r\n", &h) == -1 && strstr(why, "2 GB") &&
          parse("HTTP/1.1 200 OK\r\nContent-Length: 21474836470\r\n\r\n", &h) == -1 && strstr(why, "2 GB") &&
          parse("HTTP/1.1 200 OK\r\nContent-Length: 4294967301\r\n\r\n", &h) == -1,
          "length: what wraps around 32 bits is 2 GB or more, not a small number");
    check(parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", &h) == 0 && h.chunked, "chunked");
    check(parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked \r\n\r\n", &h) == 0 && h.chunked, "chunked: any case");
    check(parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", &h) == -1 &&
          parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n", &h) == -1 &&
          parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked;q=1\r\n\r\n", &h) == -1 &&
          parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, chunked\r\n\r\n", &h) == -1 &&
          parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n", &h) == -1 &&
          parse("HTTP/1.1 200 OK\r\nTransfer-Encoding:\r\n\r\n", &h) == -1 && strstr(why, "transfer coding"),
          "chunked: anything else refused");
    check(parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n", &h) == -1 &&
          parse("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n", &h) == -1 &&
          strstr(why, "both"), "chunked and a length: refused");
    check(parse("HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", &h) == -1 && strstr(why, "HTTP/1.0"),
          "chunked in an HTTP/1.0 response: refused");
    check(parse("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n", &h) == 0 && h.encoded && !strcmp(h.encoding, "gzip") &&
          parse("HTTP/1.1 200 OK\r\nContent-Encoding: identity\r\n\r\n", &h) == 0 && !h.encoded,
          "Content-Encoding: gzip is one, identity none");
    check(parse("HTTP/1.1 301 Moved\r\nLocation: /new/place?x=1 \r\n\r\n", &h) == 0 && h.locations == 1 &&
          !strcmp(h.location, "/new/place?x=1"), "Location");
    check(parse("HTTP/1.1 301 Moved\r\nLocation: /a\r\nLocation: /a\r\n\r\n", &h) == 0 && h.locations == 2 &&
          !h.location_conflict && parse("HTTP/1.1 301 Moved\r\nLocation: /a\r\nLocation: /b\r\n\r\n", &h) == 0 &&
          h.location_conflict && !strcmp(h.location, "/a"), "Location: twice the same, twice different");
    memcpy(loc, "HTTP/1.1 302 X\r\nLocation: ", 26);
    memset(loc + 26, 'a', FX_LOCATION);
    strcpy(loc + 26 + FX_LOCATION, "\r\n\r\n");
    check(parse(loc, &h) == 0 && h.location_long && !h.location[0], "Location: too long, known and not kept");
    loc[26 + FX_LOCATION - 1] = '\r';
    loc[26 + FX_LOCATION] = '\n';
    check(parse(loc, &h) == 0 && !h.location_long && strlen(h.location) == FX_LOCATION - 1,
          "Location: the longest kept");
    check(parse("HTTP/1.1 206 Partial\r\nContent-Range: bytes 100-199/1000\r\n\r\n", &h) == 0 &&
          h.range_start == 100 && h.range_end == 199 && h.range_total == 1000, "Content-Range: a-b/c");
    check(parse("HTTP/1.1 206 Partial\r\nContent-Range: bytes 5-9/*\r\n\r\n", &h) == 0 && h.range_start == 5 &&
          h.range_end == 9 && h.range_total == -1, "Content-Range: the total not known");
    check(parse("HTTP/1.1 416 Range\r\nContent-Range: bytes */1000\r\n\r\n", &h) == 0 && h.range_start == -1 &&
          h.range_total == 1000, "Content-Range: a 416's");
    check(parse("HTTP/1.1 206 X\r\nContent-Range: bytes 9-5/10\r\n\r\n", &h) == 0 && h.range_start == -1 &&
          parse("HTTP/1.1 206 X\r\nContent-Range: bytes 0-9/5\r\n\r\n", &h) == 0 && h.range_start == -1 &&
          parse("HTTP/1.1 206 X\r\nContent-Range: items 1-2/3\r\n\r\n", &h) == 0 && h.range_start == -1 &&
          parse("HTTP/1.1 206 X\r\nContent-Range: bytes */*\r\n\r\n", &h) == 0 && h.range_total == -1 &&
          parse("HTTP/1.1 206 X\r\nContent-Range: bytes 1-2\r\n\r\n", &h) == 0 && h.range_start == -1 &&
          parse("HTTP/1.1 206 X\r\nContent-Range: bytes 0-9/4294967306\r\n\r\n", &h) == 0 && h.range_start == -1 &&
          parse("HTTP/1.1 206 X\r\nContent-Range: bytes 4294967296-4294967297/*\r\n\r\n", &h) == 0 &&
          h.range_start == -1, "Content-Range: broken ones are none, and so are numbers of 2 GB or more");
    check(parse("HTTP/1.1 200 OK\r\nX: 1\r\n folded\r\n\r\n", &h) == -1 && strstr(why, "folding") &&
          parse("HTTP/1.1 200 OK\r\nX: 1\r\n\tfolded\r\n\r\n", &h) == -1, "obsolete line folding refused");
    check(parse("HTTP/1.1 200 OK\r\nno colon\r\n\r\n", &h) == -1 && parse("HTTP/1.1 200 OK\r\n: x\r\n\r\n", &h) == -1 &&
          strstr(why, "malformed"), "a line without a field name");
    check(parse("HTTP/1.1 200 OK\r\nContent-Length : 3\r\n\r\n", &h) == -1 &&
          parse("HTTP/1.1 200 OK\r\nX@Y: 3\r\n\r\n", &h) == -1 && parse("HTTP/1.1 200 OK\r\nX(Y): 3\r\n\r\n", &h) == -1,
          "a field name with a blank or another character that is no token");
    check(parse("HTTP/1.1 200 OK\r\nX: a\rb\r\n\r\n", &h) == -1 && PARSE_LIT("HTTP/1.1 200 OK\r\nX: a\0b\r\n\r\n", &h) == -1,
          "a CR or a NUL inside a line");
    check(parse("HTTP/1.1 200 OK\r\nX-Any: anything at all\r\nServer: s\r\nContent-Type: text/plain\r\n\r\n", &h) == 0,
          "other fields ignored");
    check(parse("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nX@Y: refused if read\r\n", &h) == 0 && h.length == 3,
          "the head ends at its empty line");
}

/* the chunked body s fed in pieces of step bytes: what came out, and whether it ended where it should */
static int unchunk(const char *s, int n, int step, char *out, int *outlen, int *taken) {
    struct fx_chunks c;
    int off = 0, done = 0, got;
    memset(&c, 0, sizeof(c));
    *outlen = 0;
    while(off < n && !done) {
        int k = n - off < step ? n - off : step, used = fx_chunks_feed(&c, s + off, k, out + *outlen, &got, &done);
        if(used < 0) return -1;
        *outlen += got;
        off += used;
        if(used < k && !done) return -2;                /* stopped before the end of its input: only at the end */
    }
    *taken = off;
    return done;
}

#define UNCHUNK(lit, step, out, len, taken) unchunk(lit, (int)sizeof(lit) - 1, step, out, len, taken)

static void test_chunks(void) {
    static char body[200000], wire[260000], out[200000], line[FX_CHUNK_LINE + 64], trailer[FX_HEAD_MAX + 64];
    char o[256];
    int len, taken, i, step, ok, w;
    check(UNCHUNK("5\r\nhello\r\n0\r\n\r\n", 100, o, &len, &taken) == 1 && len == 5 && !memcmp(o, "hello", 5) && taken == 15,
          "chunks: one, then the end");
    check(UNCHUNK("3\r\nabc\r\n2\r\nde\r\n0\r\n\r\nNEXT", 100, o, &len, &taken) == 1 && len == 5 && taken == 20,
          "chunks: two, the end, and what comes behind it left");
    check(UNCHUNK("A;ext=1\r\n0123456789\r\n0;x\r\nTrailer: v\r\n\r\n", 1, o, &len, &taken) == 1 && len == 10,
          "chunks: an extension and a trailer, a byte at a time");
    check(UNCHUNK("5 ;x=\"y\"\r\nhello\r\n0\r\n\r\n", 2, o, &len, &taken) == 1 && len == 5,
          "chunks: a blank before an extension");
    check(UNCHUNK("a\n0123456789\n0\n\n", 3, o, &len, &taken) == 1 && len == 10, "chunks: LF alone, lower-case hex");
    check(UNCHUNK("5\r\nhel", 100, o, &len, &taken) == 0 && len == 3, "chunks: not at its end yet");
    check(UNCHUNK("x\r\n", 100, o, &len, &taken) == -1 && UNCHUNK("\r\n", 100, o, &len, &taken) == -1 &&
          UNCHUNK("3\r\nabcX\r\n", 100, o, &len, &taken) == -1 && UNCHUNK("3\rX", 100, o, &len, &taken) == -1 &&
          UNCHUNK("0x5\r\n", 100, o, &len, &taken) == -1 && UNCHUNK(" 5\r\n", 100, o, &len, &taken) == -1,
          "chunks: broken framing");
    check(UNCHUNK("80000000\r\n", 100, o, &len, &taken) == -1 && UNCHUNK("7fffffff\r\n", 100, o, &len, &taken) == 0 &&
          UNCHUNK("ffffffffffffffff\r\n", 100, o, &len, &taken) == -1, "chunks: a size of 2 GB is refused, one below taken");
    memset(line, 'e', sizeof(line));
    memcpy(line, "5;", 2);
    memcpy(line + FX_CHUNK_LINE - 2, "\r\n", 2);
    check(unchunk(line, FX_CHUNK_LINE, 999, o, &len, &taken) == 0, "chunks: the longest size line");
    memcpy(line + FX_CHUNK_LINE - 2, "ee\r\n", 4);
    check(unchunk(line, FX_CHUNK_LINE + 2, 999, o, &len, &taken) == -1, "chunks: a size line too long");
    memset(trailer, 't', sizeof(trailer));
    memcpy(trailer, "0\r\n", 3);
    memcpy(trailer + FX_HEAD_MAX, "\r\n\r\n", 4);
    check(unchunk(trailer, FX_HEAD_MAX + 4, 5000, o, &len, &taken) == -1, "chunks: a trailer too long");
    memcpy(trailer + FX_HEAD_MAX - 4, "\r\n\r\n", 4);
    check(unchunk(trailer, FX_HEAD_MAX, 5000, o, &len, &taken) == 1, "chunks: the longest trailer");
    for(i = 0; i < (int)sizeof(body); i++) body[i] = (char)(rand() & 0xff);
    w = 0;
    for(i = 0; i < (int)sizeof(body);) {        /* random chunk sizes, some extensions */
        int k = 1 + rand() % 9000;
        if(i + k > (int)sizeof(body)) k = (int)sizeof(body) - i;
        w += snprintf(wire + w, sizeof(wire) - (size_t)w, rand() % 4 ? "%x\r\n" : "%X;e=%d\r\n", k, k);
        memcpy(wire + w, body + i, (size_t)k);
        w += k;
        w += snprintf(wire + w, sizeof(wire) - (size_t)w, "\r\n");
        i += k;
    }
    w += snprintf(wire + w, sizeof(wire) - (size_t)w, "0\r\nT: 1\r\n\r\n");
    ok = 1;
    for(step = 1; step < 70000 && ok; step = step * 5 + 3)
        if(unchunk(wire, w, step, out, &len, &taken) != 1 || len != (int)sizeof(body) || memcmp(out, body, sizeof(body)) ||
           taken != w) ok = 0;
    check(ok, "chunks: 200000 random bytes in random chunks, fed in pieces of any size");
}

static int name_is(const char *path, const char *want) {
    char out[FX_NAME_MAX];
    fx_name_of(path, out, sizeof(out));
    return !strcmp(out, want);
}

static void test_names(void) {
    static char longpath[600], want[FX_NAME_MAX];
    char out[64];
    check(name_is("/files/report.pdf", "report.pdf") && name_is("/a.txt", "a.txt"), "name: the last name");
    check(name_is("/", "index.html") && name_is("", "index.html") && name_is("/dir/", "index.html"), "name: a folder");
    check(name_is("/get?file=x.zip", "get") && name_is("/x.zip#part", "x.zip") && name_is("/d/?q=/x", "index.html"),
          "name: no query or fragment");
    check(name_is("/My%20File.txt", "My File.txt") && name_is("/caf%C3%A9.txt", "caf\xc3\xa9.txt"), "name: percent-decoded");
    check(name_is("/a%2Fb", "a_b") && name_is("/a%5Cb%3A%2A", "a_b__") && name_is("/x%00y", "x_y") &&
          name_is("/q\"<>|.txt", "q____.txt") && name_is("/t%09ab", "t_ab"), "name: what FAT cannot hold becomes _");
    check(name_is("/..", "index.html") && name_is("/.", "index.html") && name_is("/%2e%2e", "index.html"),
          "name: . and .. are no names");
    check(name_is("/bad%zz", "bad%zz") && name_is("/end%4", "end%4"), "name: a % that encodes nothing stays");
    memset(longpath, 'n', sizeof(longpath) - 1);
    longpath[0] = '/';
    memset(want, 'n', FX_NAME_MAX - 1);
    check(name_is(longpath, want), "name: cut at 255 bytes");
    fx_size(out, sizeof(out), 512);
    check(!strcmp(out, "512 bytes"), "size: bytes");
    fx_size(out, sizeof(out), 1536);
    check(!strcmp(out, "1.5 KB"), "size: KB");
    fx_size(out, sizeof(out), 12900000);
    check(!strcmp(out, "12.3 MB"), "size: MB");
    fx_size(out, sizeof(out), 3221225472UL);
    check(!strcmp(out, "3.0 GB"), "size: GB");
}

int main(void) {
    srand(2410);
    test_head_end();
    test_status();
    test_fields();
    test_chunks();
    test_names();
    printf("test_fetch: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
