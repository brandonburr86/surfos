/*
SurfOS Undertow's window: the loads of a browser
----------------------
File: pump.c  Date: 11/1/26
----------------------
The middle of the window's loop, and of --load's, which has no window: what the browser wants started and stopped, the
wait for a helper's bytes and the window's events together, and each load that ended given to the browser. Apart from
main.c so that the host tests run it: a window that stops answering is what all of this is for.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include "undertow.h"
#include "url.h"
#include "http.h"
#include "browser.h"
#include "loader.h"

#define WAKE_MS 250                     /* with a helper at work: how often to look for one that was killed (ld_reap()) */

static struct ld_result R;              /* 6 KB */

/* A load that ended, to the browser. *r's body is let go of. */
static void ended(struct wb *b, struct ld_result *r) {
    static struct ut_response resp;
    struct wb_page *p;
    char path[UT_URL_MAX + 1], err[UT_ERR_MAX];
    if(r->kind == WB_LOAD_PAGE) {
        err[0] = 0;
        if(r->failed) wb_failed(b, r->asked, r->why);
        else if(ld_response(r, &resp, err, sizeof(err)) ||
                !(p = wb_page_new(&resp, wb_file_path(r->asked, path, sizeof(path)), err, sizeof(err))))
            wb_failed(b, r->asked, err);
        else wb_loaded(b, p, r->reload);
    } else if(r->kind == WB_LOAD_TEXT)
        wb_text(b, r->page, r->item, r->failed ? NULL : (const char *)r->body, r->len, r->type, r->why);
    free(r->body);
    r->body = NULL;
}

int ld_start_loads(struct loader *l, struct wb *b) {
    static struct wb_request q;
    char err[UT_ERR_MAX];
    int c = wb_cancel(b), changed = 0;
    if(c) ld_cancel(l, c);
    while(wb_take(b, &q)) {
        changed = 1;                    /* a load under way: Stop in the toolbar */
        if(!ld_start(l, &q, err, sizeof(err))) continue;
        memset(&R, 0, sizeof(R));       /* no helper: a failed load like another */
        R.kind = q.kind;
        R.page = q.page;
        R.item = q.item;
        R.reload = q.reload;
        R.failed = 1;
        snprintf(R.asked, sizeof(R.asked), "%s", q.url);
        snprintf(R.why, sizeof(R.why), "%s", err);
        ended(b, &R);
    }
    return changed;
}

int ld_wait(struct loader *l, struct wb *b, int winfd, int *win) {
    struct pollfd fds[LD_HELPERS + 1];
    int slots[LD_HELPERS], base = 0, n, i, wait = -1, changed = 0;
    *win = 0;
    while(ld_overdue(l, ut_now(), &R, &wait)) {
        ended(b, &R);
        changed = 1;
    }
    if(changed) return 1;
    if(ld_reap(l) && (wait < 0 || wait > WAKE_MS)) wait = WAKE_MS;
    if(winfd >= 0) {
        fds[0].fd = winfd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        base = 1;
    }
    n = ld_fds(l, fds + base, slots);
    if(poll(fds, (nfds_t)(base + n), wait) < 0) return 0;
    if(base && fds[0].revents) *win = 1;
    for(i = 0; i < n; i++)
        if(fds[base + i].revents && ld_read(l, slots[i], &R)) {
            ended(b, &R);
            changed = 1;
        }
    return changed;
}
