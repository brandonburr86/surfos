/*
SurfOS Undertow's window: loading in helpers
----------------------
File: loader.h  Date: 11/1/26
----------------------
The window never waits for the network, and never parses what TLS, HTTP or a picture's file hold: a helper does. A
helper is wundertow itself, started with spawnx() for one load: wundertow --helper KIND, its address in the
environment (WUNDERTOW_URL: an argument holds 255 bytes, an address 2048), its output a pipe to the window, no input,
no error stream. It answers once and ends:

    struct ld_head, then head.url_len bytes of the address it ended at, then head.len bytes:
    LD_BODY     a page's or a text's body, as ut_fetch() gave it
    LD_PIXELS   a picture's pixels, ph rows of pw
    LD_FAILED   why not, as a sentence

The window's side (loader.c) reads what is there when poll() says so, a part at a time, so it goes on answering
meanwhile. A helper's bytes are read as if they came from the network: its process is the one that parsed a server's,
so it may be anything by then. Every length is checked against what its kind may have before it is used, nothing
but the sizes the head gives is allocated, strings end where they must, and bytes that are no answer, a helper
that ends before its last byte, one that does not end, are a failed load (ld_read()'s result), never a dead window.

LD_HELPERS at once: one for a page, the rest for what a page brings with it. A helper that is no longer wanted is
killed (ld_stop()) and collected later (ld_reap()). loadproc.c holds the calls that are SurfOS's own, the host tests
their stand-ins. helper.c is the helper's side.
*/

#ifndef WUNDERTOW_LOADER_H
#define WUNDERTOW_LOADER_H

#include <stddef.h>
#include <poll.h>
#include "undertow.h"
#include "url.h"
#include "http.h"
#include "browser.h"

#define LD_HELPERS    4                 /* at once */
#define LD_RESOURCES  (LD_HELPERS - 1)  /* of them, for a page's pictures and sheets */
#define LD_PAGE_MS    (UT_LOAD_MS + 10000u)    /* a helper that takes longer is killed: Undertow's minute, and some */
#define LD_RES_MS     (UT_LOAD_MS + 30000u)    /* the same with a picture's decoding */
#define LD_MAGIC      0x31444c57u       /* "WLD1" */
#define LD_URL_ENV    "WUNDERTOW_URL"
#define LD_SELF       "/initrd/bin/wundertow"

enum { LD_BODY = 1, LD_PIXELS, LD_FAILED };

struct ld_head {                        /* what the pipe starts with: same program, same layout */
    unsigned int magic;                 /* LD_MAGIC */
    unsigned int what;                  /* LD_BODY, LD_PIXELS, LD_FAILED */
    int status, verified, redirects;    /* as in ut_response */
    unsigned int w, h;                  /* LD_PIXELS, and LD_FAILED when it knows: the picture's own size */
    unsigned int pw, ph;                /* LD_PIXELS: the pixels' size */
    unsigned int url_len, len;
    char tls[16], reason[64], type[64], charset[32];
};

struct ld_result {                      /* a load that ended */
    int kind;                           /* as it was asked: WB_LOAD_* */
    unsigned int page;
    int item, reload;
    char asked[WB_ADDR_MAX];
    int failed;
    char why[UT_ERR_MAX];               /* failed: a sentence, safe text */
    char url[UT_URL_MAX + 1];           /* where it ended up */
    int status, verified, redirects;
    int w, h, pw, ph;
    char tls[16], reason[64], type[64], charset[32];
    unsigned char *body;                /* malloc'ed, the caller's now: len bytes and a NUL; or pw x ph pixels */
    size_t len;
};

enum { LJ_FREE, LJ_HEAD, LJ_URL, LJ_BODY };

struct ld_job {
    int state;                          /* LJ_* */
    int pid, fd;
    int kind, item, reload;
    unsigned int page;
    size_t budget;                      /* the bytes of pixels the page has room for */
    unsigned int deadline;
    char asked[WB_ADDR_MAX];
    struct ld_head head;
    char url[UT_URL_MAX + 1];
    unsigned char *body;
    size_t have;                        /* bytes of this part so far */
};

struct loader {
    struct ld_job job[LD_HELPERS];
    int zombies;                        /* helpers killed that nobody has collected yet */
};

void ld_init(struct loader *l);

/* A helper for this request: 0, or -1 with why (the slot is free). A page's job is job[0]; the rest take the others,
   and the browser asks for no more of those than there are (wb_take()). */
int ld_start(struct loader *l, const struct wb_request *q, char *err, size_t cap);

/* Kill a job's helper, drop what it sent. */
void ld_stop(struct loader *l, int slot);

/* wb_cancel()'s answer carried out: WB_CANCEL_PAGE the page's job, WB_CANCEL_RESOURCES the others */
void ld_cancel(struct loader *l, int what);

/* Every job that runs into fds[] (from index 0, room for LD_HELPERS), and which job each is in slots[]; the number */
int ld_fds(const struct loader *l, struct pollfd *fds, int *slots);

/* What poll() said is there: reads some of it. 1 when the load has ended, with *out (the slot is free again; failed,
   or whatever it brought), 0 when more is to come. */
int ld_read(struct loader *l, int slot, struct ld_result *out);

/* A job past its deadline, as a failed load: 1 and *out, for one such job at a time; 0 when there is none. *wait gets
   the milliseconds until the next look that is needed (it is not raised, only lowered). */
int ld_overdue(struct loader *l, unsigned int now, struct ld_result *out, int *wait);

/* Is a helper at work? */
int ld_busy(const struct loader *l);

/* Collect what has ended; true while some are still to come */
int ld_reap(struct loader *l);

/* All of them, as the window goes */
void ld_stop_all(struct loader *l);

/* A page's answer as the response it is: 0 and *out (out->body is r->body, still r's), or -1 with why */
int ld_response(const struct ld_result *r, struct ut_response *out, char *err, size_t cap);

/* loadproc.c: SurfOS's own calls. The host tests have their own. */
int ld_spawn(const char *kind, const char *url, int out_fd);        /* the helper's pid, or -1 */
void ld_kill(int pid);
int ld_collect(void);                                               /* > 0: one more collected; 0: none has ended; < 0: none left */

/* helper.c: the helper's side, and what --layout and --render do by themselves */
int ld_helper(const char *kind, const struct ut_config *cfg);       /* exit status */
int ld_fetch_page(const char *url, const struct ut_config *cfg, struct ut_response *r, int *file, char *err, size_t cap);
void ld_fetch_free(struct ut_response *r, int file);

#endif
