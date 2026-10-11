/*
SurfOS Undertow: HTML (and plain text) as readable text with numbered links
----------------------
File: html.c  Date: 10/11/26
----------------------
A stand-in that renders nothing, until track R's renderer lands
(docs/superpowers/plans/2026-10-11-undertow-tracks.md). The interface is html.h's.
*/

#include <stdlib.h>
#include <string.h>
#include "undertow.h"
#include "html.h"

int ut_render(const struct ut_response *r, struct ut_document *out, char *err, size_t cap) {
    (void)r; (void)out;
    return ut_error(err, cap, "this build of Undertow has no HTML renderer");
}

void ut_document_free(struct ut_document *d) {
    size_t i;
    if(!d) return;
    for(i = 0; i < d->link_count; i++) {
        free(d->links[i].label);
        free(d->links[i].url);
    }
    for(i = 0; i < d->anchor_count; i++) free(d->anchors[i].name);
    free(d->text);
    free(d->title);
    free(d->links);
    free(d->anchors);
    free(d->spans);
    memset(d, 0, sizeof(*d));
}

int ut_anchor_offset(const struct ut_document *d, const char *fragment, size_t *offset) {
    (void)d; (void)fragment; (void)offset;
    return -1;
}
