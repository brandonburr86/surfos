/*
SurfOS chat client: wchat
--------------------
File: window.c  Date: 11/05/26
--------------------
The Chat's window (window.h). Two pages over a status bar:

    Connect   the server, the port, TLS (on, port 6697), the nick, and a password: none, the
              server's (PASS) or SASL's (the nick's account). The password field shows dots, and
              is wiped once the connection has the password; nothing keeps it after that.
    Chat      the rooms as tabs (the server's first, then channels and private queries, a mark for
              lines not read yet and a ! for one that names you), the room's topic, its messages
              with their times, the nick list of a channel at the right, the input line below.

The messages are an area the program draws (gk_area()): a row of 16 pixels a line, longer lines
wrapped at spaces under the text after the time, the scroll bar in rows. While the view is at
the newest line it follows the new ones; scrolled back it stays on the lines it shows, also when
the oldest go. The rows are counted once a line, at the width of the area less the scroll bar's
room, so that a bar coming or going never wraps them anew. The formatting codes are drawn (bold
twice a pixel apart, underline and strikethrough as lines, colours, reverse) or left out (View >
Formatting); a colour that cannot be read on what is behind it is drawn in the theme's ink.

The connection is a helper's (link.h): the window watches its pipe (gk_watch()), gives its frames
to the model, and writes the model's lines to it while poll() says a PIPE_BUF fits, so no write
waits. A helper killed by a fault keeps its pipe until it is collected, so while one runs the
window looks every CW_REAP_MS whether it was. The model's PINGs, pacing and timeouts run from
chatwin_tick() after every event and when gk_next() waited chatwin_wait()'s time.

Keys: Enter sends the line; Up and Down bring back the lines sent (the field's history); Page Up
and Page Down scroll the messages; Ctrl+Page Up and Ctrl+Page Down go to the room before or after;
Ctrl+W closes a room (leaves a channel); Ctrl+N connects. A double-click or Enter on a nick opens
a query with it. The close button asks first while connected: Close sends QUIT, and the window
goes when the connection has ended.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <surf.h>
#include "window.h"
#include "chatint.h"

static char rbuf[LINK_DATA_MAX + 64];   /* a read from the helper */
static char piece[IRC_TEXT_MAX + 4];    /* a run of a message, to draw */

/**** small things ****/

static struct chat_room *active(const struct chatwin *w) {
    return w->c->room[w->c->active];
}

static void stamp(struct chatwin *w) {
    w->c->now = uptime();
    w->c->minute = chat_minute();
}

/* s cut to cells characters (UTF-8, one cell each), a longer one's last cell a dot, into out (size bytes, 8 or more) */
static void cut(char *out, int size, const char *s, int cells) {
    int n = 0, i = 0, len;
    if(cells < 1) cells = 1;
    while(s[i] && n < cells) {
        len = 1;
        while(((unsigned char)s[i + len] & 0xc0) == 0x80) len++;
        if((n == cells - 1 && s[i + len]) || i + len > size - 4) break;    /* the last cell is the dot's */
        i += len;
        n++;
    }
    memcpy(out, s, (size_t)i);
    if(s[i]) {
        memcpy(out + i, "\xe2\x88\x99", 3);
        i += 3;
    }
    out[i] = 0;
}

/**** the messages: rows ****/

/* The start of a line before its text, its colour, and the text's colour; every row of the text starts after it, so
   that no wrapped row can begin where a sender does and pass for a line of its own (11/09/26, the review). A sender
   is shown in printable ASCII only: a nick, or the server's name for what it says itself, between dashes; so the
   Chat's own lines' mark, a character no server can put there, says that the Chat wrote them. */
#define CW_OWN "\xe2\x89\x8b"                  /* the Chat's own lines: SurfOS's mark */
#define CW_EVENT "\xc2\xb7"                     /* what happened in a room, as the Chat tells it */
static void who_of(const struct chatwin *w, const struct chat_line *l, char *out, int size, unsigned *who,
                   unsigned *text) {
    const struct gk_theme *t = gk_theme(w->ui);
    char name[CW_WHO * 4 + 8], ascii[CW_WHO * 4 + 8];      /* more than the column shows: its dot */
    int i, n = 0;
    for(i = 0; l->from[i] && n < (int)sizeof(ascii) - 1; i++)
        if(((unsigned char)l->from[i] & 0xc0) != 0x80)  /* a character: printable ASCII, or ? */
            ascii[n++] = l->from[i] > ' ' && l->from[i] < 0x7f ? l->from[i] : '?';
    ascii[n] = 0;
    cut(name, sizeof(name), ascii, CW_WHO - 3);
    *who = l->flags & LF_HIGHLIGHT ? t->coral : l->flags & LF_SELF ? t->muted : t->teal;
    *text = t->ink;
    switch(l->kind) {
    case LN_MSG: snprintf(out, (size_t)size, "<%s> ", n ? name : "?"); break;
    case LN_ACTION: snprintf(out, (size_t)size, "* %s ", n ? name : "?"); break;
    case LN_NOTICE: snprintf(out, (size_t)size, "-%s- ", n ? name : "?"); break;
    case LN_ERROR:                                  /* the Chat's own, or a server's, in coral */
        if(n) snprintf(out, (size_t)size, "-%s- ", name);
        else snprintf(out, (size_t)size, "%s ", CW_OWN);
        *who = *text = t->coral;
        break;
    default:                                        /* events, the Chat's own lines and a server's words: muted */
        if(n) snprintf(out, (size_t)size, "-%s- ", name);
        else snprintf(out, (size_t)size, "%s ", l->kind == LN_EVENT ? CW_EVENT : CW_OWN);
        *who = *text = t->muted;
    }
}

/* the cells of a row's text: after the time and the line's start, on every row */
static int row_cells(const struct chatwin *w, int k, int who) {
    int n = w->cols - CW_TIME - who;
    (void)k;
    return n < 1 ? 1 : n;
}

static int rows_of(const struct chatwin *w, const struct chat_line *l) {
    char who[CW_WHO * 4 + 8];
    unsigned a, b;
    int at = 0, next, n = 0, cells;
    who_of(w, l, who, sizeof(who), &a, &b);
    cells = irc_cells(who);
    while(l->text[at] && n < 4096) {
        irc_wrap(l->text, at, row_cells(w, n, cells), &next);
        at = next;
        n++;
    }
    return n ? n : 1;
}

/* the area's characters across: its width less the scroll bar's room and the margins */
static int area_cols(const struct chatwin *w) {
    int x, y, width, height;
    gk_bounds(w->area, &x, &y, &width, &height);
    width = (width - GK_SCROLL_W - 2 * CW_MARGIN) / 8;
    return width < CW_TIME + CW_WHO + 8 ? CW_TIME + CW_WHO + 8 : width;
}

/* the rows of the active room's lines counted (all of them for another room or width, else the new ones), the
   extent set, and the view kept: at the newest line if it was there (another room's starts there), else on the line
   it showed at its top, also when lines before it went or the width wrapped them anew */
static void sync_lines(struct chatwin *w) {
    struct chat_room *r = active(w);
    struct gk_view v;
    unsigned long first = r->seq - (unsigned long)r->count, s, anchor = 0;
    int cols = area_cols(w), room = r->id != w->shown, wrap = cols != w->cols;
    int page, bottom, row = 0, at = 0, kept = 0;
    if(!room && !wrap && r->seq == w->synced && first == w->first) return;
    gk_area_view(w->area, &v);
    page = v.h / CW_ROW_H > 0 ? v.h / CW_ROW_H : 1;
    bottom = room || v.sy + page >= w->total;
    if(!bottom) {                                   /* the line at the top of the view, by the rows as they were */
        for(s = w->first; s < w->synced; s++) {
            int n = w->rows[s % CHAT_LINES];
            if(row + n > v.sy) {
                anchor = s;
                at = v.sy - row;
                kept = 1;
                break;
            }
            row += n;
        }
    }
    w->shown = r->id;
    w->cols = cols;
    for(s = room || wrap ? first : (w->synced > first ? w->synced : first); s < r->seq; s++)
        w->rows[s % CHAT_LINES] = (unsigned short)rows_of(w, chat_line_at(r, (int)(s - first)));
    w->total = 0;
    for(s = first; s < r->seq; s++) w->total += w->rows[s % CHAT_LINES];
    w->synced = r->seq;
    w->first = first;
    gk_area_extent(w->area, 0, 1, w->total, CW_ROW_H);
    if(bottom) row = w->total;
    else if(!kept || anchor < first) row = 0;       /* its line has gone: the oldest kept is at the top */
    else {
        for(row = 0, s = first; s < anchor; s++) row += w->rows[s % CHAT_LINES];
        row += at < w->rows[anchor % CHAT_LINES] ? at : w->rows[anchor % CHAT_LINES] - 1;
    }
    /* in view now: the newest lines, or lines that moved; a view scrolled back that keeps its lines needs nothing */
    if(bottom || room || wrap || row != v.sy) {
        gk_scroll_to(w->area, 0, row);
        gk_redraw(w->area);
    }
}

/**** the messages: drawing ****/

/* text's bytes from a to b drawn from x, y in their styles (runs counted from the text's start, so that a style set
   on an earlier row holds); fg the line's colour, bg what is behind it */
static void draw_text(struct chatwin *w, struct gw *win, const char *text, int a, int b, int x, int y, unsigned fg,
                      unsigned bg) {
    const struct gk_theme *t = gk_theme(w->ui);
    struct irc_style st;
    int pos = 0, start, len, codes = w->c->show_codes;
    irc_style_reset(&st);
    while(irc_run(text, &pos, &st, &start, &len) && start < b) {
        unsigned ink = fg, back = bg;
        int from = start > a ? start : a, to = start + len < b ? start + len : b, cells, n;
        if(to <= from) continue;
        n = to - from;
        memcpy(piece, text + from, (size_t)n);
        piece[n] = 0;
        cells = irc_cells(piece);
        if(codes && (st.fg != IRC_DEFAULT || st.bg != IRC_DEFAULT || st.reverse)) {     /* a sender's colours */
            if(st.fg != IRC_DEFAULT) ink = irc_rgb(st.fg);
            if(st.bg != IRC_DEFAULT) back = irc_rgb(st.bg);
            if(st.reverse) {
                unsigned swap = ink;
                ink = back;
                back = swap;
            }
            if(back != bg) gw_rect(win, x, y, cells * 8, CW_ROW_H, back);
            if(!irc_readable(ink, back)) ink = irc_readable(t->ink, back) ? t->ink : t->paper;
        }
        gw_text(win, x, y, piece, ink);
        if(codes && st.bold) gw_text(win, x + 1, y, piece, ink);
        if(codes && st.underline) gw_rect(win, x, y + 14, cells * 8, 1, ink);
        if(codes && st.strike) gw_rect(win, x, y + 8, cells * 8, 1, ink);
        x += cells * 8;
    }
}

/* rows from to to of line l, the first of them at y */
static void draw_line(struct chatwin *w, struct gw *win, const struct chat_line *l, int from, int to, int y, int width) {
    const struct gk_theme *t = gk_theme(w->ui);
    char who[CW_WHO * 4 + 8], time[8];
    unsigned who_ink, ink, bg = l->flags & LF_HIGHLIGHT ? t->band : t->sheet;
    int at = 0, next, end, k, cells;
    who_of(w, l, who, sizeof(who), &who_ink, &ink);
    cells = irc_cells(who);
    for(k = 0; k <= to; k++) {
        end = l->text[at] ? irc_wrap(l->text, at, row_cells(w, k, cells), &next) : at;
        if(!l->text[at]) next = at;
        if(k >= from) {
            int ry = y + (k - from) * CW_ROW_H;
            if(bg != t->sheet) gw_rect(win, 0, ry, width, CW_ROW_H, bg);
            if(!k) {
                if(l->minute == CHAT_NO_TIME) snprintf(time, sizeof(time), "--:--");
                else snprintf(time, sizeof(time), "%02u:%02u", l->minute / 60u % 24u, l->minute % 60u);
                gw_text(win, CW_MARGIN, ry, time, t->muted);
                gw_text(win, CW_MARGIN + CW_TIME * 8, ry, who, who_ink);
            }
            draw_text(w, win, l->text, at, end, CW_MARGIN + (CW_TIME + cells) * 8, ry, ink, bg);
        }
        at = next;
    }
}

static void draw_area(struct gk_widget *area, struct gw *win, const struct gk_view *v, void *data) {
    struct chatwin *w = data;
    const struct gk_theme *t = gk_theme(w->ui);
    struct chat_room *r = active(w);
    unsigned long first = r->seq - (unsigned long)r->count, s;
    int row = 0, top = v->sy + v->cy / CW_ROW_H, last = v->sy + (v->cy + v->ch - 1) / CW_ROW_H, n, from, to;
    (void)area;
    gw_rect(win, v->cx, v->cy, v->cw, v->ch, t->sheet);
    if(r->id != w->shown || first != w->first || r->seq != w->synced) return;    /* counted at the next tick */
    for(s = first; s < r->seq && row <= last; s++, row += n) {
        n = w->rows[s % CHAT_LINES];
        if(row + n <= top) continue;
        from = top > row ? top - row : 0;
        to = last - row < n - 1 ? last - row : n - 1;
        draw_line(w, win, chat_line_at(r, (int)(s - first)), from, to, (row + from - v->sy) * CW_ROW_H, v->w);
    }
}

/**** the rest of what is shown ****/

/* a room's tab: its name, ! for a line that names you, and how many came since it was shown */
static void tab_label(const struct chatwin *w, int i, char *out, int size) {
    const struct chat_room *r = w->c->room[i];
    char name[CHAT_CHAN_MAX + 8];
    irc_text(name, sizeof(name), chat_room_label(w->c, i), (int)strlen(chat_room_label(w->c, i)));
    if(r->unread > 999) snprintf(out, (size_t)size, "%s%s 999+", name, r->highlight ? " !" : "");
    else if(r->unread) snprintf(out, (size_t)size, "%s%s %d", name, r->highlight ? " !" : "", r->unread);
    else snprintf(out, (size_t)size, "%s", name);
}

/* the tabs as the rooms are: rooms are only added at the end or closed, so a tab whose room has gone is the first
   that differs */
static void sync_tabs(struct chatwin *w) {
    struct chat *c = w->c;
    char label[CHAT_CHAN_MAX + 32];
    int i = 0;
    while(i < w->ntabs) {
        if(i < c->rooms && w->tab_room[i] == c->room[i]->id) {
            i++;
            continue;
        }
        gk_tab_remove(w->tabs, i);
        memmove(&w->tab_room[i], &w->tab_room[i + 1], (size_t)(w->ntabs - i - 1) * sizeof(w->tab_room[0]));
        w->ntabs--;
    }
    for(i = w->ntabs; i < c->rooms; i++) {
        tab_label(w, i, label, sizeof(label));
        if(!gk_tab(w->tabs, label)) break;
        w->tab_room[w->ntabs++] = c->room[i]->id;
    }
    for(i = 0; i < w->ntabs; i++) {
        tab_label(w, i, label, sizeof(label));
        gk_tab_label(w->tabs, i, label);
    }
    gk_set_value(w->tabs, c->active);
}

/* a nick list's line: its mark and its name, as the list shows them */
static int by_rank(const struct chatwin *w, const char *a, const char *b) {
    const char *pa = strchr(w->c->prefix_chars, a[0]), *pb = strchr(w->c->prefix_chars, b[0]);
    int ra = a[0] != ' ' && pa ? (int)(pa - w->c->prefix_chars) : CHAT_PREFIXES;
    int rb = b[0] != ' ' && pb ? (int)(pb - w->c->prefix_chars) : CHAT_PREFIXES;
    return ra != rb ? ra - rb : irc_casecmp(w->c->casemap, a + 1, b + 1);
}

/* the list as the room's nicks are, by rank and name: the lines that differ removed and inserted, so that what is
   selected and scrolled stays where it was; a list of another room made anew */
static void sync_nicks(struct chatwin *w) {
    static int order[CHAT_NICKS];
    static char want[CHAT_NICK_MAX + 4];
    struct chat_room *r = active(w);
    int n = r->kind == ROOM_CHANNEL ? r->nnicks : 0, have, i = 0, j = 0, d;
    if(r->id != w->nicks_room) {
        gk_list_clear(w->nicks);
        w->nicks_room = r->id;
    }
    have = gk_list_count(w->nicks);
    if(n) chat_nick_order(w->c, r, order);
    while(i < have || j < n) {
        if(j < n) snprintf(want, sizeof(want), "%c%s", chat_nick_mark(w->c, &r->nicks[order[j]]), r->nicks[order[j]].name);
        d = j == n ? -1 : i == have ? 1 : by_rank(w, gk_list_text(w->nicks, i), want);
        if(!d) {
            i++;
            j++;
        } else if(d < 0) {
            gk_list_remove(w->nicks, i);
            have--;
        } else {
            if(gk_list_insert(w->nicks, i, want) < 0) {
                w->nicks_room = 0;                  /* out of memory: anew next time */
                return;
            }
            i++;
            have++;
            j++;
        }
    }
}

static void sync_topic(struct chatwin *w) {
    static char text[CHAT_TEXT_MAX], plain[CHAT_TEXT_MAX], shown[CHAT_TEXT_MAX];
    const struct gk_theme *t = gk_theme(w->ui);
    struct chat_room *r = active(w);
    int x, y, width, height;
    unsigned color = t->ink;
    if(r->kind == ROOM_CHANNEL && r->topic) snprintf(text, sizeof(text), "%s", r->topic);
    else if(r->kind == ROOM_CHANNEL) {
        snprintf(text, sizeof(text), r->joined ? "%s: no topic" : "%s: not joined", r->name);
        color = t->muted;
    } else if(r->kind == ROOM_QUERY) {
        snprintf(text, sizeof(text), "Talking with %s", r->name);
        color = t->muted;
    } else {
        chat_status(w->c, text, sizeof(text));
        color = t->muted;
    }
    irc_strip(plain, sizeof(plain), text);
    irc_text(text, sizeof(text), plain, (int)strlen(plain));
    gk_bounds(w->topic, &x, &y, &width, &height);
    cut(shown, sizeof(shown), text, width > 16 ? width / 8 - 1 : 80);
    gk_set_text(w->topic, shown);
    if(color != w->topic_color) gk_label_color(w->topic, color);
    w->topic_color = color;
}

static void sync_status(struct chatwin *w) {
    char s[200];
    struct chat_room *r = active(w);
    chat_status(w->c, s, sizeof(s));
    gk_set_text(w->status, s);
    if(r->kind == ROOM_CHANNEL && r->joined) snprintf(s, sizeof(s), "%d nick%s%s", r->nnicks, r->nnicks == 1 ? "" : "s",
                                                      r->nicks_over ? " (not all)" : "");
    else s[0] = 0;
    gk_status_right(w->status, s);
    gk_menu_enable(w->ui, MENU_CONNECT, w->c->state == CHAT_OFF && !w->closing);
    gk_menu_enable(w->ui, MENU_DISCONNECT, w->c->state != CHAT_OFF);
    gk_menu_enable(w->ui, MENU_CLOSE_ROOM, w->c->active > 0);
    chat_title(w->c, s, sizeof(s));
    if(strcmp(s, w->title)) {
        gw_title(gk_gw(w->ui), s);
        snprintf(w->title, sizeof(w->title), "%s", s);
    }
}

/* what the model changed, shown */
static void show_all(struct chatwin *w) {
    unsigned d = w->c->dirty;
    w->c->dirty = 0;
    if(d & CHAT_DIRTY_ROOMS) sync_tabs(w);
    sync_lines(w);
    if((d & CHAT_DIRTY_NICKS) || active(w)->id != w->nicks_room) sync_nicks(w);
    if(d & (CHAT_DIRTY_TOPIC | CHAT_DIRTY_ROOMS | CHAT_DIRTY_STATUS | CHAT_DIRTY_NICKS)) {
        sync_topic(w);
        sync_status(w);
    }
}

/**** the helper ****/

/* the connection is over: the pipes closed (the helper sees their end), the helper killed and collected later, and the
   model told why unless it knows */
static void end_link(struct chatwin *w, const char *why) {
    if(w->from_fd >= 0) {
        gk_unwatch(w->ui, w->from_fd);
        close(w->from_fd);
        w->from_fd = -1;
    }
    if(w->to_fd >= 0) {
        gk_unwatch(w->ui, w->to_fd);
        close(w->to_fd);
        w->to_fd = -1;
    }
    w->out_watched = 0;
    chat_wipe(w->outbox, sizeof(w->outbox));        /* a PASS or AUTHENTICATE line may be in it */
    w->outlen = 0;
    if(w->pid > 0) {
        if(!chat_reaped(w->pid)) {
            chat_kill(w->pid);
            if(!chat_reaped(w->pid)) w->zombie = w->pid;
        }
        w->pid = 0;
    }
    if(why && w->c->state != CHAT_OFF) chat_down(w->c, why);
}

static void start_link(struct chatwin *w) {
    struct chat *c = w->c;
    char why[96];
    int pid = chat_spawn(c->p.host, c->p.port, c->p.tls, &w->to_fd, &w->from_fd);
    if(pid < 0) {
        w->to_fd = w->from_fd = -1;
        snprintf(why, sizeof(why), "the connection's helper could not be started: %s", strerror(errno));
        chat_down(c, why);
        return;
    }
    w->pid = pid;
    memset(&w->link, 0, sizeof(w->link));
    if(gk_watch(w->ui, w->from_fd, POLLIN, WATCH_FROM) < 0) end_link(w, "the window watches too much already");
}

/* a frame from the helper */
static void frame(struct chatwin *w, int kind, int len) {
    char why[LINK_TEXT_MAX + 64];
    if(kind == LINK_UP) chat_up(w->c, w->link.body);
    else if(kind == LINK_DATA) chat_recv(w->c, w->link.body, len);
    else {
        link_words(why, sizeof(why), w->link.body);
        if(w->c->state != CHAT_OFF) chat_down(w->c, why[0] ? why : "the connection ended");
        end_link(w, NULL);
    }
}

/* what the helper sent: its frames, read as hostile; its end is the connection's (why: what to say then) */
static void link_in(struct chatwin *w, const char *why) {
    int n = (int)read(w->from_fd, rbuf, sizeof(rbuf)), at = 0, used, kind, len, r;
    if(n < 0 && errno == EINTR) return;
    if(n <= 0) {
        end_link(w, why);
        return;
    }
    while(at < n && w->from_fd >= 0) {
        r = link_read(&w->link, rbuf + at, n - at, &used, &kind, &len);
        if(r < 0) {
            end_link(w, "the connection's helper sent what no helper sends");
            return;
        }
        at += used;
        if(r) frame(w, kind, len);
    }
}

/* what waits for the helper, a PIPE_BUF at most: poll() said that much fits, so the write does not wait. The watch
   is level-triggered: it goes as soon as nothing waits, or it would say POLLOUT for ever */
static void link_out(struct chatwin *w, int revents) {
    int n = w->outlen < 4096 ? w->outlen : 4096, k;
    if(revents & (POLLERR | POLLHUP | POLLNVAL)) {
        end_link(w, "the connection's helper has gone");
        return;
    }
    if((revents & POLLOUT) && n) {
        k = (int)write(w->to_fd, w->outbox, (size_t)n);
        if(k < 0 && errno == EINTR) return;
        if(k <= 0) {
            end_link(w, "the connection's helper has gone");
            return;
        }
        memmove(w->outbox, w->outbox + k, (size_t)(w->outlen - k));
        chat_wipe(w->outbox + w->outlen - k, (size_t)k);
        w->outlen -= k;
    }
    if(!w->outlen && w->out_watched) {
        gk_unwatch(w->ui, w->to_fd);
        w->out_watched = 0;
    }
}

/* the model's lines for the helper, while there is room; the pipe watched while any wait */
static void push(struct chatwin *w) {
    if(w->to_fd < 0) return;
    if(w->c->state >= CHAT_REGISTERING && w->outlen < CW_OUTBOX)
        w->outlen += chat_send(w->c, w->outbox + w->outlen, CW_OUTBOX - w->outlen);
    if(w->outlen && !w->out_watched) w->out_watched = gk_watch(w->ui, w->to_fd, POLLOUT, WATCH_TO) == 0;
    else if(!w->outlen && w->out_watched) {
        gk_unwatch(w->ui, w->to_fd);
        w->out_watched = 0;
    }
}

/**** the connect form ****/

/* the form's note, in coral while the password would go without encryption (11/09/26, the review) */
static void form_note(struct chatwin *w) {
    const struct gk_theme *t = gk_theme(w->ui);
    if(!gk_value(w->tls) && gk_value(w->passkind) != CHAT_PASS_NONE) {
        gk_set_text(w->formnote, "No encryption: anyone on the network can read the password.");
        gk_label_color(w->formnote, t->coral);
    } else {
        gk_set_text(w->formnote, "Nothing is kept: the password is asked for at each connection.");
        gk_label_color(w->formnote, t->muted);
    }
}

static void show_form(struct chatwin *w) {
    struct chat *c = w->c;
    char port[12];
    if(c->p.host[0]) gk_set_text(w->host, c->p.host);
    if(c->p.port) {
        snprintf(port, sizeof(port), "%u", c->p.port);
        gk_set_text(w->port, port);
        gk_set_value(w->tls, c->p.tls);
    }
    if(c->p.nick[0]) gk_set_text(w->nick, c->p.nick);
    gk_set_value(w->passkind, c->p.pass_kind);
    gk_enable(w->pass, c->p.pass_kind != CHAT_PASS_NONE);
    gk_field_wipe(w->pass);
    gk_set_text(w->formerr, "");
    form_note(w);
    gk_set_value(w->pages, PAGE_CONNECT);
    gk_focus(c->p.host[0] ? w->pass : w->host);
    if(!c->p.host[0] || c->p.pass_kind == CHAT_PASS_NONE) gk_focus(c->p.host[0] ? w->nick : w->host);
}

static void form_error(struct chatwin *w, const char *why) {
    gk_set_text(w->formerr, why);
    gk_label_color(w->formerr, gk_theme(w->ui)->coral);
}

/* Connect, or Enter in the form. A password without TLS is asked about first (11/09/26, the review): Cancel is the
   box's first answer, which Enter gives, and Esc gives Cancel too; only Send anyway, chosen, sends it (plain_ok) */
static void connect_now(struct chatwin *w, int plain_ok) {
    struct chat_params p;
    char err[160];
    const char *s = gk_text(w->port);
    unsigned long port = 0;
    if(w->c->state != CHAT_OFF) return;
    memset(&p, 0, sizeof(p));
    if(strlen(gk_text(w->host)) > CHAT_HOST_MAX) {
        form_error(w, "A server's name has 253 characters at most");
        return;
    }
    snprintf(p.host, sizeof(p.host), "%s", gk_text(w->host));
    for(; *s >= '0' && *s <= '9' && port < 100000; s++) port = port * 10 + (unsigned long)(*s - '0');
    p.port = *s || port > 65535 ? 0 : (unsigned)port;
    p.tls = gk_value(w->tls);
    if(strlen(gk_text(w->nick)) > CHAT_NICK_MAX) {
        form_error(w, "A nick has 32 characters at most");
        return;
    }
    snprintf(p.nick, sizeof(p.nick), "%s", gk_text(w->nick));
    p.pass_kind = gk_value(w->passkind);
    if(p.pass_kind != CHAT_PASS_NONE) {
        if(strlen(gk_text(w->pass)) > CHAT_PASS_MAX) {
            form_error(w, "A password has 256 bytes at most");
            return;
        }
        memcpy(p.pass, gk_text(w->pass), strlen(gk_text(w->pass)) + 1);
    }
    if(!p.tls && p.pass_kind != CHAT_PASS_NONE && !plain_ok && p.pass[0]) {
        chat_wipe(&p, sizeof(p));
        gk_confirm(w->ui, BOX_PLAIN, "Send your password unencrypted?", "No encryption: anyone on the network can read it.",
                   "Cancel, and turn TLS on, to send it encrypted.", "Cancel", "Send anyway", NULL);
        return;
    }
    p.plain_ok = plain_ok;
    stamp(w);
    if(chat_connect(w->c, &p, err, sizeof(err))) {
        chat_wipe(&p, sizeof(p));
        form_error(w, err);
        return;
    }
    chat_wipe(&p, sizeof(p));
    gk_field_wipe(w->pass);                         /* the model has it now, until it goes to the server */
    gk_set_text(w->formerr, "");
    gk_set_value(w->pages, PAGE_CHAT);
    chat_select(w->c, 0);
    gk_focus(w->input);
    start_link(w);
}

/**** events ****/

static void scroll_page(struct chatwin *w, int down) {
    struct gk_view v;
    int page;
    gk_area_view(w->area, &v);
    page = v.h / CW_ROW_H > 1 ? v.h / CW_ROW_H - 1 : 1;
    gk_scroll_to(w->area, 0, v.sy + (down ? page : -page));
}

/* the close button, Ctrl+Q: at once when there is no connection, else after QUIT and the connection's end */
static int closing(struct chatwin *w) {
    if(w->c->state == CHAT_OFF && !w->pid) return 0;
    w->closing = 1;
    chat_quit(w->c, NULL);
    return 1;
}

static int menu(struct chatwin *w, int id) {
    struct chat *c = w->c;
    switch(id) {
    case MENU_CONNECT:
        if(c->state == CHAT_OFF && !w->closing) show_form(w);
        break;
    case MENU_DISCONNECT:
        chat_quit(c, NULL);
        break;
    case MENU_CLOSE_ROOM:
        chat_close_room(c, c->active);
        break;
    case MENU_QUIT:
        if(c->state == CHAT_OFF) return closing(w);
        gk_confirm(w->ui, BOX_CLOSE, "Leave the server and close Chat?", "Chat says goodbye to the server first.", "",
                   "Close", NULL, "Cancel");
        break;
    case MENU_PREV:
    case MENU_NEXT:
        if(c->rooms > 1) chat_select(c, (c->active + (id == MENU_NEXT ? 1 : c->rooms - 1)) % c->rooms);
        gk_set_value(w->pages, PAGE_CHAT);
        break;
    case MENU_CODES:                                /* the same rows either way: drawn again */
        c->show_codes = !c->show_codes;
        gk_menu_check(w->ui, MENU_CODES, c->show_codes);
        gk_redraw(w->area);
        break;
    case MENU_ABOUT:
        gk_message(w->ui, BOX_ABOUT, "About Chat", "IRC in a window: /join /part /msg /me /nick",     /* 54 cells a line */
                   "/topic /query /quit. It keeps no logs, no passwords.");
        break;
    }
    return 1;
}

int chatwin_event(struct chatwin *w, const struct gk_event *ev) {
    struct chat *c = w->c;
    char line[CHAT_NICK_MAX + 16];
    stamp(w);
    switch(ev->type) {
    case GK_EV_CLOSE:
        if(c->state == CHAT_OFF) return closing(w);
        gk_confirm(w->ui, BOX_CLOSE, "Leave the server and close Chat?", "Chat says goodbye to the server first.", "",
                   "Close", NULL, "Cancel");
        break;
    case GK_EV_ANSWER:
        if(ev->id == BOX_CLOSE && ev->value == 0) return closing(w);
        if(ev->id == BOX_PLAIN && ev->value == 1) connect_now(w, 1);     /* Send anyway: chosen, never the default */
        break;
    case GK_EV_MENU:
        return menu(w, ev->id);
    case GK_EV_CLICK:
        if(ev->id == ID_CONNECT) connect_now(w, 0);
        else if(ev->id == ID_CANCEL) {
            gk_field_wipe(w->pass);
            gk_set_value(w->pages, PAGE_CHAT);
            gk_focus(w->input);
        }
        break;
    case GK_EV_ACTIVATE:
        if(ev->id == ID_INPUT) {                     /* a line for NickServ is kept out of the history, and wiped */
            int secret = chat_secret(c, gk_text(w->input));
            if(!secret) gk_field_remember(w->input);
            chat_input(c, gk_text(w->input));
            if(secret) gk_field_wipe(w->input);
            else gk_set_text(w->input, "");
        } else if(ev->id == ID_NICKS && ev->value >= 0 && gk_list_text(w->nicks, ev->value)) {
            snprintf(line, sizeof(line), "/query %s", gk_list_text(w->nicks, ev->value) + 1);
            chat_input(c, line);
            gk_focus(w->input);
        } else if(ev->id == ID_HOST || ev->id == ID_PORT || ev->id == ID_NICK || ev->id == ID_PASS) connect_now(w, 0);
        break;
    case GK_EV_CHANGE:
        if(ev->id == ID_TABS) chat_select(c, ev->value);
        else if(ev->id == ID_TLS) {                 /* the usual ports follow TLS */
            if(ev->value && !strcmp(gk_text(w->port), "6667")) gk_set_text(w->port, "6697");
            if(!ev->value && !strcmp(gk_text(w->port), "6697")) gk_set_text(w->port, "6667");
            form_note(w);
        } else if(ev->id == ID_PASSKIND) {
            gk_enable(w->pass, ev->value != CHAT_PASS_NONE);
            if(ev->value == CHAT_PASS_NONE) gk_field_wipe(w->pass);
            form_note(w);
        }
        break;
    case GK_EV_TAB_CLOSE:
        if(ev->value == 0) gk_set_text(w->status, "The server's room stays: Disconnect in the Chat menu ends it");
        else chat_close_room(c, ev->value);
        break;
    case GK_EV_FD:
        if(ev->id == WATCH_FROM && w->from_fd >= 0)
            link_in(w, c->state == CHAT_CONNECTING ? "the connection was not made" : "the connection's helper ended");
        else if(ev->id == WATCH_TO && w->to_fd >= 0) link_out(w, ev->value);
        break;
    case GK_EV_KEY:                                 /* a key no widget took: the input line's neighbours */
        if((ev->value == WIN_KEY_PGUP || ev->value == WIN_KEY_PGDN) && !(ev->mods & (WIN_MOD_CTRL | WIN_MOD_ALT)))
            scroll_page(w, ev->value == WIN_KEY_PGDN);
        else if((ev->mods & WIN_MOD_ALT) && !(ev->mods & WIN_MOD_CTRL) && ev->value >= '1' && ev->value <= '9' &&
                ev->value - '1' < c->rooms) {       /* Alt+1 to Alt+9: the rooms by their places */
            chat_select(c, ev->value - '1');
            gk_set_value(w->pages, PAGE_CHAT);
        }
        break;
    case GK_EV_POINTER:                             /* a click in the messages: the keys stay the input line's */
        if(ev->w == w->area && (ev->value & WIN_BUTTON_LEFT)) gk_focus(w->input);
        break;
    case GK_EV_RESIZE:
    case GK_EV_THEME:
        c->dirty |= CHAT_DIRTY_ALL;
        gk_redraw(w->area);
        form_note(w);
        break;
    }
    return 1;
}

int chatwin_tick(struct chatwin *w) {
    struct chat *c = w->c;
    stamp(w);
    if(w->zombie && chat_reaped(w->zombie)) w->zombie = 0;
    if(w->pid > 0 && chat_reaped(w->pid)) {         /* it ended: what it sent before is read first (collected, its */
        int reads;                                  /* end of the pipe is closed: the reads end) */
        w->pid = 0;
        for(reads = 0; w->from_fd >= 0 && reads < 64; reads++) link_in(w, "the connection's helper stopped");
        end_link(w, "the connection's helper stopped");
    }
    chat_tick(c);
    if(c->ending) end_link(w, c->why[0] ? c->why : "closed");
    push(w);
    show_all(w);
    return !(w->closing && c->state == CHAT_OFF && !w->pid);
}

int chatwin_wait(const struct chatwin *w) {
    int ms = chat_wait(w->c);
    /* lines that wait for room in the pipe are no reason to wake: its POLLOUT does that */
    if(!ms && !w->c->ending && (w->to_fd < 0 || w->outlen > CW_OUTBOX - IRC_LINE_MAX - 1)) ms = CW_REAP_MS;
    if((w->pid || w->zombie) && (ms < 0 || ms > CW_REAP_MS)) ms = CW_REAP_MS;
    return ms;
}

/**** the window ****/

static struct gk_widget *labelled(struct gk_widget *col, const char *text) {
    struct gk_widget *row = gk_hbox(col, 8), *label = gk_label(row, text);
    gk_size(label, 88, 0);
    return row;
}

static void build_form(struct chatwin *w, struct gk_widget *page) {
    struct gk_widget *col = gk_vbox(page, 8), *row;
    gk_pad(col, 16);
    gk_label(col, "Connect to an IRC server");
    gk_separator(col);
    w->host = gk_field(labelled(col, "Server:"), ID_HOST, CHAT_HOST_MAX, 0);
    gk_field_hint(w->host, "irc.example.org");
    row = labelled(col, "Port:");
    w->port = gk_field(row, ID_PORT, 5, 0);
    gk_size(w->port, 64, 0);
    gk_grow(w->port, 0, 0);
    gk_set_text(w->port, "6697");
    w->tls = gk_check(row, "TLS: encrypted, the certificate checked", ID_TLS, 1);
    w->nick = gk_field(labelled(col, "Nick:"), ID_NICK, 32, 0);
    gk_set_text(w->nick, "surfer");
    w->passkind = gk_radio(labelled(col, "Password:"), ID_PASSKIND, 1);
    gk_radio_add(w->passkind, "None");
    gk_radio_add(w->passkind, "The server's");
    gk_radio_add(w->passkind, "SASL, the nick's account");
    gk_set_value(w->passkind, CHAT_PASS_NONE);
    w->pass = gk_field(labelled(col, ""), ID_PASS, CHAT_PASS_MAX, GK_PASSWORD);
    gk_enable(w->pass, 0);
    w->formnote = gk_label(col, "");
    w->formerr = gk_label(col, "");
    row = gk_hbox(col, 8);
    gk_button(row, "Connect", ID_CONNECT);
    gk_button(row, "Cancel", ID_CANCEL);
}

static void build_chat(struct chatwin *w, struct gk_widget *page) {
    struct gk_widget *row, *mid;
    w->tabs = gk_tabs(page, ID_TABS, GK_CLOSABLE);
    gk_grow(w->tabs, 1, 0);                         /* its pages are empty: the strip alone */
    row = gk_hbox(page, 0);
    gk_pad(row, 4);
    w->topic = gk_label(row, "");
    gk_grow(w->topic, 1, 0);
    gk_separator(page);
    mid = gk_hbox(page, 0);
    gk_grow(mid, 1, 1);
    w->area = gk_area(mid, ID_AREA, 0, draw_area, w);
    w->nicks = gk_list(mid, ID_NICKS);
    gk_size(w->nicks, CW_NICKS_W, 0);
    gk_grow(w->nicks, 0, 1);
    row = gk_hbox(page, 0);
    gk_pad(row, 4);
    w->input = gk_field(row, ID_INPUT, CHAT_INPUT_MAX, 0);
    gk_grow(w->input, 1, 0);
    gk_field_history(w->input, CHAT_HISTORY);
    gk_field_hint(w->input, "A message, or /join #channel, /msg nick text, /query nick, /nick name, /quit");
}

static void build_menus(struct chatwin *w) {
    int m = gk_menu(w->ui, "Chat");
    gk_menu_item(w->ui, m, MENU_CONNECT, "Connect\xe2\x88\x99", GK_CTRL | 'N', 0);
    gk_menu_item(w->ui, m, MENU_DISCONNECT, "Disconnect", 0, GK_ITEM_OFF);
    gk_menu_line(w->ui, m);
    gk_menu_item(w->ui, m, MENU_CLOSE_ROOM, "Close room", GK_CTRL | 'W', GK_ITEM_OFF);
    gk_menu_item(w->ui, m, MENU_QUIT, "Close Chat", GK_CTRL | 'Q', 0);
    m = gk_menu(w->ui, "View");
    gk_menu_item(w->ui, m, MENU_PREV, "Room before", GK_CTRL | WIN_KEY_PGUP, 0);
    gk_menu_item(w->ui, m, MENU_NEXT, "Room after", GK_CTRL | WIN_KEY_PGDN, 0);
    gk_menu_line(w->ui, m);
    gk_menu_item(w->ui, m, MENU_CODES, "Formatting", 0, GK_ITEM_CHECK | GK_ITEM_ON);
    m = gk_menu(w->ui, "Help");
    gk_menu_item(w->ui, m, MENU_ABOUT, "About Chat", 0, 0);
}

int chatwin_open(struct chatwin *w, struct chat *c) {
    struct gk_widget *root;
    memset(w, 0, sizeof(*w));
    w->c = c;
    w->to_fd = w->from_fd = -1;
    w->ui = gk_open_limits("Chat", CW_W, CW_H, CW_MIN_W, CW_MIN_H, 0, 0, WIN_RESIZABLE);
    if(!w->ui) return -1;
    root = gk_root(w->ui);
    w->pages = gk_pages(root);
    build_form(w, gk_vbox(w->pages, 0));
    build_chat(w, gk_vbox(w->pages, 0));
    build_menus(w);
    w->status = gk_status(root);
    gk_set_value(w->pages, PAGE_CONNECT);
    form_note(w);
    gk_focus(w->host);
    if(!gk_ok(w->ui)) {
        gk_close(w->ui);
        w->ui = NULL;
        errno = ENOMEM;
        return -1;
    }
    gk_flush(w->ui);                                /* laid out: the area's width is known */
    c->dirty = CHAT_DIRTY_ALL;
    stamp(w);
    show_all(w);
    return 0;
}

void chatwin_close(struct chatwin *w) {
    end_link(w, NULL);
    if(w->zombie && !chat_reaped(w->zombie)) chat_kill(w->zombie);
    w->zombie = 0;
    gk_close(w->ui);
    w->ui = NULL;
}
