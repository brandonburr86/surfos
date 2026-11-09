/*
SurfOS chat client: wchat
--------------------
File: chat.c    Date: 11/05/26
--------------------
The Chat's rooms, their lines and nick lists, the connection's life and what goes out (chat.h).
recv.c reads the server, input.c what is typed. No system calls: host-tested as it is.

A room's lines are a ring of CHAT_LINES; every room's together are held to CHAT_BYTES by letting
the oldest line of the room that holds the most go, so one busy channel cannot push the others'
lines out. A nick list is kept sorted by name under the server's case mapping, so that a QUIT
or a NICK finds a nick in each channel by halving rather than by reading every list; a new case
mapping (ISUPPORT, which comes after the first lists only on an odd server) sorts them again.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "chatint.h"

static char names_plain[IRC_TEXT_MAX + 512], say_text[CHAT_TEXT_MAX];

/* the static buffers of this file: test_wchat looks through them for a password (11/09/26, the review) */
const struct chat_buf chat_bufs[] = { { names_plain, sizeof(names_plain) }, { say_text, sizeof(say_text) }, { NULL, 0 } };

/**** small things ****/

void chat_wipe(void *p, size_t n) {
    volatile unsigned char *b = p;
    while(n--) *b++ = 0;
}

int chat_since(const struct chat *c, unsigned when) {
    unsigned d = c->now - when;
    return d > 0x7fffffffu ? 0 : (int)d;
}

void chat_forget_password(struct chat *c) {
    chat_wipe(c->p.pass, sizeof(c->p.pass));
}

/* the last check before PASS or AUTHENTICATE goes (11/09/26, the review): over TLS (the helper said which), or
   without it only as the person agreed when the window asked */
int chat_password_may_go(const struct chat *c) {
    return c->tls[0] || c->p.plain_ok;
}

void chat_lose(struct chat *c, const char *why) {
    if(c->ending) return;
    c->ending = 1;
    snprintf(c->why, sizeof(c->why), "%s", why);
    c->dirty |= CHAT_DIRTY_STATUS;
}

/* a string cut short loses the start of a character it cut */
void chat_utf8_trim(char *s) {
    size_t n = strlen(s), i = n;
    int need;
    while(i > 0 && ((unsigned char)s[i - 1] & 0xc0) == 0x80 && n - i < 3) i--;
    if(!i || (unsigned char)s[i - 1] < 0xc0) return;
    need = (unsigned char)s[i - 1] < 0xe0 ? 2 : (unsigned char)s[i - 1] < 0xf0 ? 3 : 4;
    if((int)(n - i + 1) < need) s[i - 1] = 0;
}

void chat_isupport_defaults(struct chat *c) {
    c->casemap = IRC_CM_RFC1459;
    strcpy(c->prefix_modes, "ov");
    strcpy(c->prefix_chars, "@+");
    strcpy(c->chantypes, "#&");
    c->statusmsg[0] = 0;
    strcpy(c->chanmodes[0], "beI");
    strcpy(c->chanmodes[1], "k");
    strcpy(c->chanmodes[2], "l");
    strcpy(c->chanmodes[3], "imnpst");
    c->nicklen = 0;
    c->network[0] = 0;
}

int chat_is_me(const struct chat *c, const char *nick) {
    return c->nick[0] && !irc_casecmp(c->casemap, nick, c->nick);
}

int chat_is_channel(const struct chat *c, const char *name) {
    return name[0] && strchr(c->chantypes, name[0]) != NULL;
}

/* a byte a nick may hold, for telling a word from part of one (RFC 2812's letters, digits and specials) */
static int nick_byte(int ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
           (ch && strchr("-[]\\`^_{|}", ch));
}

int chat_names_us(const struct chat *c, const char *text) {
    char *plain = names_plain;
    size_t n = strlen(c->nick), i;
    const char *p;
    if(!n) return 0;
    irc_strip(plain, sizeof(names_plain), text);
    for(p = plain; *p; p++) {
        if(p > plain && nick_byte((unsigned char)p[-1])) continue;
        for(i = 0; i < n && p[i] && irc_lower(c->casemap, (unsigned char)p[i]) ==
                                  irc_lower(c->casemap, (unsigned char)c->nick[i]); i++) ;
        if(i == n && !nick_byte((unsigned char)p[n])) return 1;
    }
    return 0;
}

/**** rooms ****/

struct chat_room *chat_room_new(struct chat *c, int kind, const char *name) {
    struct chat_room *r;
    if(c->rooms >= CHAT_ROOMS) return NULL;
    r = calloc(1, sizeof(*r));
    if(!r) return NULL;
    r->id = ++c->next_id;
    r->kind = kind;
    chat_room_name(r, name);
    c->room[c->rooms++] = r;
    c->dirty |= CHAT_DIRTY_ROOMS;
    return r;
}

int chat_find(const struct chat *c, int kind, const char *name) {
    int i;
    for(i = 1; i < c->rooms; i++)
        if(c->room[i]->kind == kind && !irc_casecmp(c->casemap, c->room[i]->name, name)) return i;
    return -1;
}

struct chat_room *chat_room_of(struct chat *c, int kind, const char *name) {
    int i = chat_find(c, kind, name);
    return i < 0 ? NULL : c->room[i];
}

static long line_size(const struct chat_line *l) {
    return (long)(sizeof(*l) + strlen(l->from) + strlen(l->text) + 2);
}

static void drop_oldest(struct chat *c, struct chat_room *r) {
    struct chat_line *l = r->ring[r->first];
    long n = line_size(l);
    r->bytes -= n;
    c->bytes -= n;
    free(l);
    r->ring[r->first] = NULL;
    r->first = (r->first + 1) % CHAT_LINES;
    r->count--;
    if(c->rooms && r == c->room[c->active]) c->dirty |= CHAT_DIRTY_LINES;
}

void chat_room_drop(struct chat *c, int i) {
    struct chat_room *r = c->room[i];
    while(r->count) drop_oldest(c, r);
    free(r->ring);
    chat_nicks_clear(c, r);
    free(r->nicks);
    free(r->topic);
    free(r);
    memmove(&c->room[i], &c->room[i + 1], (size_t)(c->rooms - i - 1) * sizeof(c->room[0]));
    c->room[--c->rooms] = NULL;
    if(c->active > i || (c->active == i && i > 0)) c->active--;
    if(c->active >= c->rooms) c->active = c->rooms ? c->rooms - 1 : 0;
    if(c->rooms) {
        c->room[c->active]->unread = 0;
        c->room[c->active]->highlight = 0;
    }
    c->dirty |= CHAT_DIRTY_ALL;
}

/* every room's lines within CHAT_BYTES: the oldest of the room that holds the most go, never the line just added */
static void trim(struct chat *c, struct chat_room *added) {
    while(c->bytes > CHAT_BYTES) {
        struct chat_room *big = NULL;
        int i;
        for(i = 0; i < c->rooms; i++)
            if(c->room[i]->count && (!big || c->room[i]->bytes > big->bytes)) big = c->room[i];
        if(!big || (big == added && big->count == 1)) break;
        drop_oldest(c, big);
    }
}

void chat_say(struct chat *c, struct chat_room *r, int kind, int flags, const char *from, const char *fmt, ...) {
    char *text = say_text;
    struct chat_line *l;
    size_t lf, lt;
    long size;
    va_list ap;
    if(!r) r = c->room[0];
    if(!from) from = "";
    va_start(ap, fmt);
    vsnprintf(text, sizeof(say_text), fmt, ap);
    va_end(ap);
    chat_utf8_trim(text);
    if(!r->ring && !(r->ring = calloc(CHAT_LINES, sizeof(*r->ring)))) return;  /* out of memory: not kept */
    lf = strlen(from);
    lt = strlen(text);
    l = malloc(sizeof(*l) + lf + lt + 2);
    if(!l) return;
    l->from = (char *)(l + 1);
    memcpy(l->from, from, lf + 1);
    l->text = l->from + lf + 1;
    memcpy(l->text, text, lt + 1);
    l->minute = c->minute >= 0 && c->minute < 1440 ? (unsigned short)c->minute : CHAT_NO_TIME;
    l->kind = (unsigned char)kind;
    l->flags = (unsigned char)flags;
    l->seq = r->seq++;
    if(r->count == CHAT_LINES) drop_oldest(c, r);
    r->ring[(r->first + r->count) % CHAT_LINES] = l;
    r->count++;
    size = line_size(l);
    r->bytes += size;
    c->bytes += size;
    trim(c, r);
    if(r == c->room[c->active]) c->dirty |= CHAT_DIRTY_LINES;
    else {
        r->unread++;
        if(flags & LF_HIGHLIGHT) r->highlight = 1;
        c->dirty |= CHAT_DIRTY_ROOMS;
    }
}

const struct chat_line *chat_line_at(const struct chat_room *r, int i) {
    if(i < 0 || i >= r->count) return NULL;
    return r->ring[(r->first + i) % CHAT_LINES];
}

void chat_select(struct chat *c, int room) {
    if(room < 0 || room >= c->rooms) return;
    c->active = room;
    c->room[room]->unread = 0;
    c->room[room]->highlight = 0;
    c->dirty |= CHAT_DIRTY_ALL;
}

int chat_close_room(struct chat *c, int room) {
    struct chat_room *r;
    const char *mid[1];
    if(room <= 0 || room >= c->rooms) return -1;
    r = c->room[room];
    if(r->kind == ROOM_CHANNEL && r->joined && c->state >= CHAT_REGISTERING && c->state != CHAT_QUITTING) {
        mid[0] = r->name;
        if(chat_queue(c, 0, "PART", mid, 1, NULL)) {
            chat_say(c, r, LN_ERROR, 0, "", "Not left: too much is waiting to go");
            return 0;
        }
        r->parting = 1;
        return 0;
    }
    chat_room_drop(c, room);
    return 0;
}

/* a room's name as the server has it, and as it is shown: a channel's name holds any byte but a few (RFC 2812
   section 1.3), so what is shown of it is cleaned as text is (11/09/26, the review) */
void chat_room_name(struct chat_room *r, const char *name) {
    char text[IRC_TEXT_MAX];
    snprintf(r->name, sizeof(r->name), "%s", name);
    irc_text(text, sizeof(text), r->name, (int)strlen(r->name));
    irc_strip(r->label, sizeof(r->label), text);
}

const char *chat_room_label(const struct chat *c, int room) {
    if(room < 0 || room >= c->rooms) return "";
    if(room) return c->room[room]->label;
    if(c->network[0]) return c->network;
    return c->p.host[0] ? c->p.host : "Server";
}

/**** nick lists ****/

int chat_nick_find(const struct chat *c, const struct chat_room *r, const char *nick, int *at) {
    int lo = 0, hi = r->nnicks;
    while(lo < hi) {
        int mid = lo + (hi - lo) / 2, d = irc_casecmp(c->casemap, r->nicks[mid].name, nick);
        if(!d) {
            if(at) *at = mid;
            return mid;
        }
        if(d < 0) lo = mid + 1;
        else hi = mid;
    }
    if(at) *at = lo;
    return -1;
}

/* a nick that did not fit: the list is not all of them (2 once that has been said) */
static int over(struct chat_room *r) {
    if(!r->nicks_over) r->nicks_over = 1;
    return -1;
}

int chat_nick_add(struct chat *c, struct chat_room *r, const char *nick, unsigned modes) {
    int at, i = chat_nick_find(c, r, nick, &at);
    size_t len;
    char *name;
    if(i >= 0) {
        r->nicks[i].modes = modes;
        if(r == c->room[c->active]) c->dirty |= CHAT_DIRTY_NICKS;
        return i;
    }
    if(r->nnicks >= CHAT_NICKS || c->nicks_all >= CHAT_NICKS_ALL) return over(r);
    if(r->nnicks == r->nicks_room) {
        int more = r->nicks_room ? r->nicks_room * 2 : 16;
        struct chat_nick *n;
        if(more > CHAT_NICKS) more = CHAT_NICKS;
        n = realloc(r->nicks, (size_t)more * sizeof(*n));
        if(!n) return over(r);
        r->nicks = n;
        r->nicks_room = more;
    }
    len = strlen(nick);
    name = malloc(len + 1);
    if(!name) return over(r);
    memcpy(name, nick, len + 1);
    memmove(&r->nicks[at + 1], &r->nicks[at], (size_t)(r->nnicks - at) * sizeof(*r->nicks));
    r->nicks[at].name = name;
    r->nicks[at].modes = modes;
    r->nnicks++;
    c->nicks_all++;
    if(r == c->room[c->active]) c->dirty |= CHAT_DIRTY_NICKS;
    return at;
}

void chat_nick_drop(struct chat *c, struct chat_room *r, int i) {
    free(r->nicks[i].name);
    memmove(&r->nicks[i], &r->nicks[i + 1], (size_t)(r->nnicks - i - 1) * sizeof(*r->nicks));
    r->nnicks--;
    c->nicks_all--;
    if(r == c->room[c->active]) c->dirty |= CHAT_DIRTY_NICKS;
}

int chat_nick_rename(struct chat *c, struct chat_room *r, int i, const char *to) {
    unsigned modes = r->nicks[i].modes;
    chat_nick_drop(c, r, i);
    return chat_nick_add(c, r, to, modes);
}

void chat_nicks_clear(struct chat *c, struct chat_room *r) {
    int i;
    for(i = 0; i < r->nnicks; i++) free(r->nicks[i].name);
    c->nicks_all -= r->nnicks;
    r->nnicks = 0;
    r->nicks_over = 0;
    if(c->rooms && r == c->room[c->active]) c->dirty |= CHAT_DIRTY_NICKS;
}

static const struct chat *sorting;          /* qsort() has no argument for the comparison's case mapping */
static const struct chat_room *ranking;

static int by_name(const void *a, const void *b) {
    return irc_casecmp(sorting->casemap, ((const struct chat_nick *)a)->name, ((const struct chat_nick *)b)->name);
}

void chat_nicks_resort(struct chat *c) {
    int i, j, k;
    sorting = c;
    for(i = 1; i < c->rooms; i++) {
        struct chat_room *r = c->room[i];
        if(r->nnicks < 2) continue;
        qsort(r->nicks, (size_t)r->nnicks, sizeof(*r->nicks), by_name);
        for(j = 1, k = 0; j < r->nnicks; j++) {     /* names the new mapping makes one are one */
            if(!irc_casecmp(c->casemap, r->nicks[k].name, r->nicks[j].name)) {
                r->nicks[k].modes |= r->nicks[j].modes;
                free(r->nicks[j].name);
                c->nicks_all--;
            } else r->nicks[++k] = r->nicks[j];
        }
        r->nnicks = k + 1;
    }
    c->dirty |= CHAT_DIRTY_NICKS;
}

static int rank(unsigned modes) {
    int i;
    for(i = 0; i < CHAT_PREFIXES; i++) if(modes & (1u << i)) return i;
    return CHAT_PREFIXES;
}

char chat_nick_mark(const struct chat *c, const struct chat_nick *n) {
    int i = rank(n->modes);
    return i < (int)strlen(c->prefix_chars) ? c->prefix_chars[i] : ' ';
}

static int by_rank(const void *a, const void *b) {
    const struct chat_nick *x = &ranking->nicks[*(const int *)a], *y = &ranking->nicks[*(const int *)b];
    int d = rank(x->modes) - rank(y->modes);
    return d ? d : irc_casecmp(sorting->casemap, x->name, y->name);
}

void chat_nick_order(const struct chat *c, const struct chat_room *r, int *order) {
    int i;
    for(i = 0; i < r->nnicks; i++) order[i] = i;
    sorting = c;
    ranking = r;
    qsort(order, (size_t)r->nnicks, sizeof(*order), by_rank);
}

/**** going out ****/

int chat_queue(struct chat *c, int urgent, const char *cmd, const char *const *mid, int nmid, const char *trail) {
    char line[IRC_LINE_MAX + 1];
    int n = irc_compose(line, sizeof(line), cmd, mid, nmid, trail);
    if(n < 0) return -1;
    if(urgent ? c->ucount == CHAT_URGENT : c->qcount == CHAT_QUEUE) {
        chat_wipe(line, sizeof(line));
        return -1;
    }
    if(urgent) memcpy(c->urgent[(c->uhead + c->ucount++) % CHAT_URGENT], line, (size_t)n + 1);
    else memcpy(c->queue[(c->qhead + c->qcount++) % CHAT_QUEUE], line, (size_t)n + 1);
    chat_wipe(line, sizeof(line));
    return 0;
}

static void empty_queues(struct chat *c) {
    chat_wipe(c->urgent, sizeof(c->urgent));
    chat_wipe(c->queue, sizeof(c->queue));
    c->uhead = c->ucount = c->qhead = c->qcount = 0;
}

int chat_send(struct chat *c, char *out, int size) {
    int o = 0;
    for(;;) {
        int urgent = c->ucount > 0, n;
        char *line;
        if((int)(c->send_at - c->now) < 0) c->send_at = c->now;
        if(urgent) line = c->urgent[c->uhead];
        else if(c->qcount && (int)(c->send_at - c->now) < CHAT_BURST_MS) line = c->queue[c->qhead];
        else break;
        n = (int)strlen(line);
        if(o + n > size) break;
        memcpy(out + o, line, (size_t)n);
        o += n;
        chat_wipe(line, IRC_LINE_MAX + 1);          /* a PASS or an AUTHENTICATE stays nowhere */
        if(urgent) {
            c->uhead = (c->uhead + 1) % CHAT_URGENT;
            c->ucount--;
        } else {
            c->qhead = (c->qhead + 1) % CHAT_QUEUE;
            c->qcount--;
        }
        /* registration's lines count for nothing: servers count a client's lines from its welcome, and the first
           /join should not wait for the CAP, NICK and USER before it */
        if(c->state >= CHAT_ONLINE) c->send_at += CHAT_LINE_MS;
    }
    return o;
}

/* the bytes of text that go in one PRIVMSG of at most budget: whole characters, up to the last space when that
   leaves at least half of the room used */
static int piece(const char *text, int budget) {
    int n = (int)strlen(text), i;
    if(n <= budget) return n;
    n = budget;
    while(n > 0 && ((unsigned char)text[n] & 0xc0) == 0x80) n--;
    for(i = n; i > budget / 2; i--) if(text[i] == ' ') return i;
    return n ? n : 1;
}

/* NickServ's commands carry passwords (IDENTIFY, REGISTER, GHOST, SET PASSWORD ...): what goes to it is shown with
   its first word only, and every copy here is wiped (11/09/26, the review) */
int chat_secret_to(const struct chat *c, const char *target) {
    return !irc_casecmp(c->casemap, target, "NickServ");
}

static void masked(char *shown, int first) {
    size_t n = first ? strcspn(shown, " ") : 0;
    if(first && !shown[n]) return;                  /* one word: nothing after it to hide */
    snprintf(shown + n, IRC_TEXT_MAX - n, "%s****", n ? " " : "");
}

int chat_send_text(struct chat *c, struct chat_room *r, const char *target, const char *text, int action) {
    char part[IRC_LINE_MAX], body[IRC_LINE_MAX + 16], shown[IRC_TEXT_MAX], to[IRC_TEXT_MAX];
    const char *mid[1];
    int at = 0, len = (int)strlen(text), n, secret = chat_secret_to(c, target), ret = 0;
    int elsewhere = r->kind == ROOM_SERVER || irc_casecmp(c->casemap, r->name, target);    /* /msg from another room */
    /* what the server puts in front when it passes it on: ":nick!user@host PRIVMSG target :", the user and host
       guessed (10 and 63) until our JOIN showed them */
    int budget = IRC_LINE_MAX - 2 - (1 + (int)strlen(c->nick) + 1 + (c->user[0] ? (int)strlen(c->user) : 74) + 9 +
                                     (int)strlen(target) + 2) - (action ? 9 : 0);
    if(budget < 32) {
        chat_say(c, r, LN_ERROR, 0, "", "Not sent: the name is too long to send to");
        return -1;
    }
    while(at < len) {
        n = piece(text + at, budget);
        memcpy(part, text + at, (size_t)n);
        part[n] = 0;
        if(action) snprintf(body, sizeof(body), "\001ACTION %s\001", part);
        else snprintf(body, sizeof(body), "%s", part);
        mid[0] = target;
        if(chat_queue(c, 0, "PRIVMSG", mid, 1, body)) {
            chat_say(c, r, LN_ERROR, 0, "", "Not sent: too much is waiting to go");
            ret = -1;
            break;
        }
        irc_text(shown, sizeof(shown), part, n);
        if(secret) masked(shown, !at);
        if(elsewhere) {
            irc_text(to, sizeof(to), target, (int)strlen(target));
            chat_say(c, r, action ? LN_ACTION : LN_MSG, LF_SELF, c->nick, "\xe2\x86\x92 %s: %s", to, shown);
        } else chat_say(c, r, action ? LN_ACTION : LN_MSG, LF_SELF, c->nick, "%s", shown);
        at += n;
        while(text[at] == ' ') at++;
    }
    if(secret) {
        chat_wipe(part, sizeof(part));
        chat_wipe(body, sizeof(body));
        chat_wipe(shown, sizeof(shown));
    }
    return ret;
}

int chat_wait(const struct chat *c) {
    int w = -1;
#define SOON(ms) do { int v_ = (ms); if(v_ < 0) v_ = 0; if(w < 0 || v_ < w) w = v_; } while(0)
    if(c->ending || c->ucount) SOON(0);
    if(c->qcount) SOON((int)(c->send_at - c->now) - CHAT_BURST_MS + 1);
    if(c->state >= CHAT_REGISTERING) {
        if(c->cap == CAP_LS || c->cap == CAP_REQ) SOON(CHAT_CAP_MS - chat_since(c, c->cap_at));
        if(c->state == CHAT_QUITTING) SOON(CHAT_QUIT_MS - chat_since(c, c->quit_at));
        if(c->ping_at) SOON(CHAT_PING_MS - chat_since(c, c->ping_at));
        else SOON(CHAT_IDLE_MS - chat_since(c, c->last_rx));
    }
#undef SOON
    return w;
}

/**** the connection ****/

int chat_init(struct chat *c) {
    memset(c, 0, sizeof(*c));
    c->minute = -1;
    c->show_codes = 1;
    chat_isupport_defaults(c);
    if(!chat_room_new(c, ROOM_SERVER, "")) return -1;
    c->dirty = CHAT_DIRTY_ALL;
    return 0;
}

void chat_free(struct chat *c) {
    while(c->rooms) chat_room_drop(c, c->rooms - 1);
    chat_forget_password(c);
    empty_queues(c);
}

static int fail(char *err, int size, const char *why) {
    snprintf(err, (size_t)size, "%s", why);
    return -1;
}

int chat_check(const struct chat_params *p, char *err, int size) {
    const char *s;
    size_t n = strlen(p->host);
    if(!n) return fail(err, size, "Type the server's name");
    if(n > CHAT_HOST_MAX) return fail(err, size, "A server's name has 253 characters at most");
    for(s = p->host; *s; s++)
        if(!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '.' || *s == '-'))
            return fail(err, size, "A server's name has only letters, digits, dots and dashes");
    if(p->port < 1 || p->port > 65535) return fail(err, size, "The port is a number from 1 to 65535");
    n = strlen(p->nick);
    if(!n) return fail(err, size, "Type a nick");
    if(n > 32) return fail(err, size, "A nick has 32 characters at most");
    for(s = p->nick; *s; s++)
        if(!nick_byte((unsigned char)*s) || (s == p->nick && ((*s >= '0' && *s <= '9') || *s == '-')))
            return fail(err, size, "A nick starts with a letter or one of [ ] \\ ` _ ^ { | }, and then has letters, "
                        "digits, those and dashes");
    if(p->pass_kind == CHAT_PASS_NONE) return 0;
    if(p->pass_kind != CHAT_PASS_SERVER && p->pass_kind != CHAT_PASS_SASL) return fail(err, size, "No such password");
    if(!p->pass[0]) return fail(err, size, "Type the password, or choose none");
    if(strnlen(p->pass, sizeof(p->pass)) > CHAT_PASS_MAX) return fail(err, size, "A password has 256 bytes at most");
    for(s = p->pass; *s; s++)
        if(*s == '\r' || *s == '\n') return fail(err, size, "A password cannot hold a line end");
    return 0;
}

int chat_connect(struct chat *c, const struct chat_params *p, char *err, int size) {
    if(c->state != CHAT_OFF) return fail(err, size, "Chat is connected already");
    if(chat_check(p, err, size)) return -1;
    if(irc_casecmp(IRC_CM_ASCII, p->host, c->p.host) || p->port != c->p.port) {   /* another server's rooms go */
        while(c->rooms > 1) chat_room_drop(c, c->rooms - 1);
        free(c->room[0]->topic);
        c->room[0]->topic = NULL;
    }
    c->p = *p;
    c->state = CHAT_CONNECTING;
    c->ending = 0;
    c->why[0] = 0;
    chat_say(c, NULL, LN_INFO, 0, "", "Connecting to %s, port %u, %s ...", p->host, p->port,
             p->tls ? "with TLS" : "without encryption");
    if(p->pass_kind != CHAT_PASS_NONE && !p->tls)
        chat_say(c, NULL, LN_ERROR, 0, "", p->plain_ok ? "The password goes unencrypted, as you agreed: this connection "
                 "has no TLS" : "The password will not be sent: this connection has no TLS");
    c->dirty |= CHAT_DIRTY_ALL;
    return 0;
}

void chat_up(struct chat *c, const char *tls) {
    const char *mid[3];
    char user[10];
    int i, n = 0;
    if(c->state != CHAT_CONNECTING) return;
    for(i = 0; tls && tls[i] && n < (int)sizeof(c->tls) - 1; i++)   /* the helper's word: printable ASCII kept */
        if(tls[i] > ' ' && tls[i] < 0x7f) c->tls[n++] = tls[i];
    c->tls[n] = 0;
    chat_isupport_defaults(c);
    c->casemap_set = 0;
    c->state = CHAT_REGISTERING;
    snprintf(c->nick, sizeof(c->nick), "%s", c->p.nick);
    c->user[0] = 0;
    c->tries = 0;
    c->cap = CAP_NONE;
    c->sasl = SASL_NONE;
    c->sasl_offered = c->sasl_plain = 0;
    c->last_rx = c->cap_at = c->send_at = c->now;
    c->ping_at = 0;
    memset(&c->rd, 0, sizeof(c->rd));
    if(c->tls[0]) chat_say(c, NULL, LN_INFO, 0, "", "Connected with %s: the server's certificate is verified", c->tls);
    else chat_say(c, NULL, LN_INFO, 0, "", "Connected, without encryption");
    mid[0] = "LS";
    mid[1] = "302";
    chat_queue(c, 1, "CAP", mid, 2, NULL);
    c->cap = CAP_LS;
    if(c->p.pass_kind == CHAT_PASS_SERVER && !chat_password_may_go(c)) {
        chat_say(c, NULL, LN_ERROR, 0, "", "The password was not sent: this connection has no encryption");
        chat_forget_password(c);
    } else if(c->p.pass_kind == CHAT_PASS_SERVER) {
        mid[0] = c->p.pass;
        if(strchr(c->p.pass, ' ') || c->p.pass[0] == ':') chat_queue(c, 1, "PASS", NULL, 0, c->p.pass);
        else chat_queue(c, 1, "PASS", mid, 1, NULL);
        chat_forget_password(c);
    }
    mid[0] = c->nick;
    chat_queue(c, 1, "NICK", mid, 1, NULL);
    for(i = 0, n = 0; c->p.nick[i] && n < (int)sizeof(user) - 1; i++) {    /* the user name: the nick's letters */
        char ch = c->p.nick[i];
        if(ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        if((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) user[n++] = ch;
    }
    user[n] = 0;
    mid[0] = n ? user : "surf";
    mid[1] = "0";
    mid[2] = "*";
    chat_queue(c, 1, "USER", mid, 3, c->p.nick);
    c->dirty |= CHAT_DIRTY_STATUS;
}

/* the negotiation is over, SASL with it: the password is not wanted any more (PASS went with registration) */
void chat_cap_end(struct chat *c) {
    const char *mid[1] = { "END" };
    chat_forget_password(c);
    if(c->sasl != SASL_NONE) c->sasl = SASL_DONE;
    if(c->cap != CAP_LS && c->cap != CAP_REQ) return;
    chat_queue(c, 1, "CAP", mid, 1, NULL);
    c->cap = CAP_DONE;
}

void chat_down(struct chat *c, const char *why) {
    char text[IRC_TEXT_MAX];
    int i, was = c->state;
    if(was == CHAT_OFF) return;
    irc_text(text, sizeof(text), why ? why : "", why ? (int)strnlen(why, 1024) : 0);
    for(i = 0; i < c->rooms; i++) {
        struct chat_room *r = c->room[i];
        if(!i || r->joined)
            chat_say(c, r, was == CHAT_QUITTING ? LN_INFO : LN_ERROR, 0, "", "%s%s%s",
                     was == CHAT_CONNECTING ? "Not connected" : "Disconnected", text[0] ? ": " : "", text);
        if(r->kind == ROOM_CHANNEL) {
            r->joined = r->parting = r->names_done = 0;
            chat_nicks_clear(c, r);
        }
    }
    c->state = CHAT_OFF;
    c->ending = 0;
    c->cap = CAP_NONE;
    c->sasl = SASL_NONE;
    c->tls[0] = 0;
    c->user[0] = 0;
    memset(c->joining, 0, sizeof(c->joining));
    empty_queues(c);
    memset(&c->rd, 0, sizeof(c->rd));
    chat_forget_password(c);
    c->dirty |= CHAT_DIRTY_ALL;
}

void chat_quit(struct chat *c, const char *reason) {
    if(c->state == CHAT_CONNECTING) {
        chat_lose(c, "cancelled");
        return;
    }
    if(c->state < CHAT_REGISTERING || c->state == CHAT_QUITTING) return;
    if(c->qcount) {
        chat_say(c, c->room[c->active], LN_INFO, 0, "", "%d line%s waiting to go %s not sent", c->qcount,
                 c->qcount == 1 ? "" : "s", c->qcount == 1 ? "was" : "were");
        chat_wipe(c->queue, sizeof(c->queue));
        c->qhead = c->qcount = 0;
    }
    if((!reason || !*reason || chat_queue(c, 1, "QUIT", NULL, 0, reason)) && chat_queue(c, 1, "QUIT", NULL, 0, NULL)) {
        chat_lose(c, "closed");             /* no room even for QUIT: the connection just ends */
        return;
    }
    c->state = CHAT_QUITTING;
    c->quit_at = c->now;
    c->dirty |= CHAT_DIRTY_STATUS;
}

void chat_tick(struct chat *c) {
    if(c->state < CHAT_REGISTERING || c->ending) return;
    if(c->cap == CAP_LS && chat_since(c, c->cap_at) >= CHAT_CAP_MS) {
        chat_say(c, NULL, LN_INFO, 0, "", "The server did not answer CAP: registering without it%s",
                 c->p.pass_kind == CHAT_PASS_SASL ? ", and without the SASL login" : "");
        chat_cap_end(c);
    }
    if(c->cap == CAP_REQ && chat_since(c, c->cap_at) >= CHAT_CAP_MS) {     /* the SASL login, a step unanswered */
        const char *mid[1] = { "*" };
        if(c->sasl == SASL_ASKED || c->sasl == SASL_SENT) chat_queue(c, 1, "AUTHENTICATE", mid, 1, NULL);
        chat_say(c, NULL, LN_ERROR, 0, "", "The server did not answer the SASL login: you are not logged in");
        chat_cap_end(c);                            /* CAP END, and what is left of the password goes */
    }
    if(c->state == CHAT_QUITTING && chat_since(c, c->quit_at) >= CHAT_QUIT_MS) {
        chat_lose(c, "closed after QUIT");
        return;
    }
    if(c->ping_at) {
        if(chat_since(c, c->ping_at) >= CHAT_PING_MS) {
            char why[96];
            snprintf(why, sizeof(why), "the server has not answered for %d seconds",
                     (CHAT_IDLE_MS + CHAT_PING_MS) / 1000);
            chat_lose(c, why);
        }
    } else if(chat_since(c, c->last_rx) >= CHAT_IDLE_MS && !chat_queue(c, 1, "PING", NULL, 0, "wchat"))
        c->ping_at = c->now;
}

/**** for the window ****/

void chat_status(const struct chat *c, char *out, int size) {
    const char *how = c->tls[0] ? c->tls : "not encrypted";
    switch(c->state) {
    case CHAT_CONNECTING:
        snprintf(out, (size_t)size, "Connecting to %s, port %u ...", c->p.host, c->p.port);
        break;
    case CHAT_REGISTERING:
        snprintf(out, (size_t)size, "Registering as %s with %s, port %u (%s) ...", c->nick, c->p.host, c->p.port, how);
        break;
    case CHAT_ONLINE:
        snprintf(out, (size_t)size, "%s on %s, port %u (%s)", c->nick, c->p.host, c->p.port, how);
        break;
    case CHAT_QUITTING:
        snprintf(out, (size_t)size, "Leaving %s ...", c->p.host);
        break;
    default:
        snprintf(out, (size_t)size, "Not connected");
    }
    chat_utf8_trim(out);
}

void chat_title(const struct chat *c, char *out, int size) {
    if(c->state == CHAT_OFF) snprintf(out, (size_t)size, "Chat");
    else snprintf(out, (size_t)size, "Chat - %s", chat_room_label(c, c->active));
    chat_utf8_trim(out);
}
