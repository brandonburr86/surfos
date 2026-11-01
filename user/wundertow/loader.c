/*
SurfOS Undertow's window: loading in helpers
----------------------
File: loader.c  Date: 11/1/26
----------------------
The window's side of loader.h: helpers started and stopped, their answers read a part at a time. Everything a helper
sends is checked here before anything is done with it; see loader.h.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include "undertow.h"
#include "url.h"
#include "http.h"
#include "image.h"
#include "browser.h"
#include "loader.h"

#define CHUNK (64u * 1024)              /* read at a time: the window looks at its events between */

typedef char enough_helpers[WB_RESOURCES <= LD_RESOURCES ? 1 : -1];     /* what the browser asks for has a slot */

void ld_init(struct loader *l) {
    memset(l, 0, sizeof(*l));
}

static void job_clear(struct ld_job *j) {
    if(j->state != LJ_FREE) close(j->fd);
    free(j->body);
    memset(j, 0, sizeof(*j));
}

int ld_start(struct loader *l, const struct wb_request *q, char *err, size_t cap) {
    static const char *const kinds[] = { "page", "image", "text" };
    struct ld_job *j = NULL;
    int p[2], i;
    if(q->kind == WB_LOAD_PAGE) j = &l->job[0];
    else for(i = 1; i < LD_HELPERS && !j; i++) if(l->job[i].state == LJ_FREE) j = &l->job[i];
    if(!j || j->state != LJ_FREE) return ut_error(err, cap, "this window is already loading as much as it can");
    if(pipe(p)) return ut_error(err, cap, "no pipe to load through: %s", strerror(errno));
    j->pid = ld_spawn(kinds[q->kind], q->url, p[1]);
    close(p[1]);
    if(j->pid <= 0) {
        close(p[0]);
        memset(j, 0, sizeof(*j));
        return ut_error(err, cap, "no helper to load with: %s", strerror(errno));
    }
    j->fd = p[0];
    j->state = LJ_HEAD;
    j->kind = q->kind;
    j->item = q->item;
    j->reload = q->reload;
    j->page = q->page;
    j->budget = q->budget;
    j->deadline = ut_now() + (q->kind == WB_LOAD_PAGE ? LD_PAGE_MS : LD_RES_MS);
    snprintf(j->asked, sizeof(j->asked), "%s", q->url);
    return 0;
}

void ld_stop(struct loader *l, int slot) {
    struct ld_job *j = &l->job[slot];
    if(j->state == LJ_FREE) return;
    ld_kill(j->pid);
    l->zombies++;
    job_clear(j);
}

void ld_cancel(struct loader *l, int what) {
    int i;
    if(what & WB_CANCEL_PAGE) ld_stop(l, 0);
    if(what & WB_CANCEL_RESOURCES) for(i = 1; i < LD_HELPERS; i++) ld_stop(l, i);
}

void ld_stop_all(struct loader *l) {
    ld_cancel(l, WB_CANCEL_PAGE | WB_CANCEL_RESOURCES);
}

int ld_fds(const struct loader *l, struct pollfd *fds, int *slots) {
    int i, n = 0;
    for(i = 0; i < LD_HELPERS; i++) {
        if(l->job[i].state == LJ_FREE) continue;
        fds[n].fd = l->job[i].fd;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        slots[n++] = i;
    }
    return n;
}

int ld_busy(const struct loader *l) {
    int i;
    for(i = 0; i < LD_HELPERS; i++) if(l->job[i].state != LJ_FREE) return 1;
    return 0;
}

/* Every look collects: a helper that was killed by a fault keeps its descriptors until it is collected (a kill's are
   closed at the reap, a task's own exit closes them; docs/ARCHITECTURE.md), so its pipe says nothing until then. */
int ld_reap(struct loader *l) {
    int r;
    while((r = ld_collect()) != 0) {
        if(r < 0) {
            l->zombies = 0;
            break;
        }
        if(l->zombies) l->zombies--;
    }
    return l->zombies > 0 || ld_busy(l);
}

/* The job ended: *out says how, why being why it was given up on. Whatever its helper does next, nobody looks. */
static void finish(struct loader *l, int slot, struct ld_result *out, const char *why) {
    struct ld_job *j = &l->job[slot];
    const struct ld_head *h = &j->head;
    memset(out, 0, sizeof(*out));
    out->kind = j->kind;
    out->page = j->page;
    out->item = j->item;
    out->reload = j->reload;
    snprintf(out->asked, sizeof(out->asked), "%s", j->asked);
    if(why) {
        out->failed = 1;
        ut_safe_text(out->why, sizeof(out->why), why, strlen(why), 1);
        if(h->magic == LD_MAGIC && h->what == LD_PIXELS && h->w <= WIMG_SIDE_MAX && h->h <= WIMG_SIDE_MAX) {
            out->w = (int)h->w;                 /* too much for the page: its frame still takes its shape */
            out->h = (int)h->h;
        }
    } else if(h->what == LD_FAILED) {
        out->failed = 1;
        ut_safe_text(out->why, sizeof(out->why), (const char *)j->body, h->len, 1);
        if(!out->why[0]) snprintf(out->why, sizeof(out->why), "the load failed");
        out->w = (int)h->w;
        out->h = (int)h->h;
    } else {
        snprintf(out->url, sizeof(out->url), "%s", j->url);
        out->status = h->status;
        out->verified = h->verified != 0;
        out->redirects = h->redirects;
        out->w = (int)h->w;
        out->h = (int)h->h;
        out->pw = (int)h->pw;
        out->ph = (int)h->ph;
        ut_safe_text(out->tls, sizeof(out->tls), h->tls, strlen(h->tls), 0);
        ut_safe_text(out->reason, sizeof(out->reason), h->reason, strlen(h->reason), 1);
        ut_safe_text(out->type, sizeof(out->type), h->type, strlen(h->type), 0);
        ut_safe_text(out->charset, sizeof(out->charset), h->charset, strlen(h->charset), 0);
        out->body = j->body;
        out->len = h->len;
        j->body = NULL;
    }
    ld_kill(j->pid);
    l->zombies++;
    job_clear(j);
}

/* The head is whole: is it an answer this job can have? NULL, with the body's room made; or why not */
static const char *check_head(struct ld_job *j) {
    struct ld_head *h = &j->head;
    unsigned long long bytes = 0;
    if(h->magic != LD_MAGIC) return "the helper's answer is no answer";
    h->tls[sizeof(h->tls) - 1] = 0;
    h->reason[sizeof(h->reason) - 1] = 0;
    h->type[sizeof(h->type) - 1] = 0;
    h->charset[sizeof(h->charset) - 1] = 0;
    if(h->url_len > UT_URL_MAX) return "the helper's answer is no answer";
    if(h->redirects < 0 || h->redirects > UT_REDIRECTS_MAX) h->redirects = 0;
    switch(h->what) {
    case LD_FAILED:
        if(h->len > UT_ERR_MAX || h->url_len) return "the helper's answer is no answer";
        if(h->w > WIMG_SIDE_MAX || h->h > WIMG_SIDE_MAX) h->w = h->h = 0;
        break;
    case LD_BODY:
        if(j->kind == WB_LOAD_IMAGE || h->len > UT_BODY_MAX || !h->url_len) return "the helper's answer is no answer";
        if(h->status < 100 || h->status > 599) return "the helper's answer is no answer";
        h->w = h->h = h->pw = h->ph = 0;
        break;
    case LD_PIXELS:
        if(j->kind != WB_LOAD_IMAGE || !h->url_len || h->status < 100 || h->status > 599)
            return "the helper's answer is no answer";
        if(!h->pw || !h->ph || h->w > WIMG_SIDE_MAX || h->h > WIMG_SIDE_MAX || h->pw > h->w || h->ph > h->h)
            return "the helper's answer is no answer";
        bytes = (unsigned long long)h->pw * h->ph * sizeof(unsigned int);
        if(bytes != h->len) return "the helper's answer is no answer";
        if(bytes > j->budget) return "the page holds as many pictures as it can";
        break;
    default:
        return "the helper's answer is no answer";
    }
    j->body = malloc((size_t)h->len + 1);
    if(!j->body) return "out of memory for what was loaded";
    return NULL;
}

int ld_read(struct loader *l, int slot, struct ld_result *out) {
    struct ld_job *j = &l->job[slot];
    char *dst;
    size_t want;
    ssize_t n;
    const char *why;
    switch(j->state) {
    case LJ_HEAD:
        dst = (char *)&j->head + j->have;
        want = sizeof(j->head) - j->have;
        break;
    case LJ_URL:
        dst = j->url + j->have;
        want = j->head.url_len - j->have;
        break;
    case LJ_BODY:
        dst = (char *)j->body + j->have;
        want = j->head.len - j->have;
        break;
    default:
        return 0;
    }
    if(want > CHUNK) want = CHUNK;
    n = read(j->fd, dst, want);
    if(n < 0) {
        if(errno == EINTR || errno == EAGAIN) return 0;
        finish(l, slot, out, "the load ended: its pipe failed");
        return 1;
    }
    if(!n) {                            /* its end, before the end of the answer */
        finish(l, slot, out, "the load ended before its end: its helper stopped");
        return 1;
    }
    j->have += (size_t)n;
    while(j->state != LJ_FREE) {        /* as many parts as are whole: the next may be of no bytes */
        if(j->state == LJ_HEAD) {
            if(j->have < sizeof(j->head)) return 0;
            why = check_head(j);
            if(why) {
                finish(l, slot, out, why);
                return 1;
            }
            j->state = LJ_URL;
            j->have = 0;
        } else if(j->state == LJ_URL) {
            if(j->have < j->head.url_len) return 0;
            j->url[j->head.url_len] = 0;
            if(strlen(j->url) != j->head.url_len) {
                finish(l, slot, out, "the helper's answer is no answer");
                return 1;
            }
            j->state = LJ_BODY;
            j->have = 0;
        } else {
            if(j->have < j->head.len) return 0;
            j->body[j->head.len] = 0;
            finish(l, slot, out, NULL);
            return 1;
        }
    }
    return 0;
}

int ld_overdue(struct loader *l, unsigned int now, struct ld_result *out, int *wait) {
    int i;
    for(i = 0; i < LD_HELPERS; i++) {
        struct ld_job *j = &l->job[i];
        unsigned int left;
        if(j->state == LJ_FREE) continue;
        if(ut_expired(j->deadline, now)) {
            finish(l, i, out, "the load took too long, and was stopped");
            return 1;
        }
        left = ut_remaining(j->deadline, now);
        if(*wait < 0 || left < (unsigned int)*wait) *wait = (int)(left > 0x7fffffffu ? 0x7fffffffu : left);
    }
    return 0;
}

int ld_response(const struct ld_result *r, struct ut_response *out, char *err, size_t cap) {
    memset(out, 0, sizeof(*out));
    if(ut_url_parse(r->url, &out->url, err, cap)) return -1;
    out->body = r->body;
    out->len = r->len;
    out->status = r->status;
    out->verified = r->verified;
    out->redirects = r->redirects;
    memcpy(out->tls, r->tls, sizeof(out->tls));
    memcpy(out->reason, r->reason, sizeof(out->reason));
    memcpy(out->content_type, r->type, sizeof(out->content_type));
    memcpy(out->charset, r->charset, sizeof(out->charset));
    return 0;
}
