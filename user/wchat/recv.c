/*
SurfOS chat client: wchat
--------------------
File: recv.c    Date: 11/05/26
--------------------
What an IRC server sends (chat.h's chat_recv()), read as hostile: every parameter is counted
before it is used, every text is cleaned (irc_text()) before it is shown, a nick or a channel
from the server changes a list or a room only when irc_valid_nick() or irc_valid_channel() takes
it, and a reply the client did not ask for (a CAP ACK, an AUTHENTICATE +) changes nothing. The
commands are RFC 1459's and RFC 2812's that a client sees (PING, PRIVMSG, NOTICE, JOIN, PART,
QUIT, NICK, KICK, TOPIC, MODE, INVITE, KILL, ERROR, WALLOPS), IRCv3's CAP (version 302) and
AUTHENTICATE for SASL PLAIN (RFC 4616), and the numerics of RFC 1459 section 6, RFC 2812 section
5, the ISUPPORT draft (005) and IRCv3's SASL (900-908). Anything else is shown as it came, in
the server's room.

Where a line goes: a channel's message to its room, a private message to the query of its
sender (made for it, room allowing), a notice to that query or else the room in front, and what
the server says itself to the server's room. Errors, which answer what was just typed, go to
the room in front.

CTCP (the \x01 messages): ACTION is shown as an action; any other request is shown and never
answered, so that nobody can make this client say anything (its version, its time) or flood a
channel for them; a DCC offer is shown and nothing more, as Chat makes no DCC connections.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chatint.h"

#define DROPPED_MS 10000                /* lines dropped are said at most this often */
#define PONGS 2                         /* PONGs waiting at most: a flood of PINGs is answered once or twice */

static char text[IRC_TEXT_MAX], text2[IRC_TEXT_MAX], who[IRC_TEXT_MAX];

static const char *clean(char *out, const char *s) {
    irc_text(out, IRC_TEXT_MAX, s, (int)strnlen(s, IRC_IN_MAX));
    return out;
}

/* the parameters from the from-th on, one space between, cleaned into out */
static const char *rest(const struct irc_msg *m, int from, char *out) {
    static char raw[IRC_IN_MAX + 1];
    int i, o = 0;
    for(i = from; i < m->argc; i++) {
        int n = (int)strnlen(m->argv[i], IRC_IN_MAX);
        if(o && o < IRC_IN_MAX) raw[o++] = ' ';
        if(n > IRC_IN_MAX - o) n = IRC_IN_MAX - o;
        memcpy(raw + o, m->argv[i], (size_t)n);
        o += n;
    }
    irc_text(out, IRC_TEXT_MAX, raw, o);
    return out;
}

/* a nick the lists may hold: ASCII, no byte a message or a list would break at */
static int user_nick(const struct chat *c, const struct irc_msg *m) {
    return !m->server && irc_valid_nick(m->nick, CHAT_NICK_MAX, c->chantypes);
}

/* who sent m, to be shown: the nick, or the server's name cleaned */
static const char *sender(const struct irc_msg *m) {
    return clean(who, m->nick);
}

static struct chat_room *active(struct chat *c) {
    return c->room[c->active];
}

static void set_topic(struct chat *c, struct chat_room *r, const char *t) {
    size_t n = strlen(t);
    free(r->topic);
    r->topic = NULL;
    if(n && (r->topic = malloc(n + 1))) memcpy(r->topic, t, n + 1);
    if(r == active(c)) c->dirty |= CHAT_DIRTY_TOPIC;
}

/* each name of a comma list (a JOIN's or PART's channels) in turn: the next into out, 0 at the end */
static int next_name(const char **list, char *out, int size) {
    const char *p = *list, *end;
    int n;
    while(*p == ',') p++;
    if(!*p) return 0;
    end = strchr(p, ',');
    if(!end) end = p + strlen(p);
    n = (int)(end - p);
    if(n > size - 1) n = size - 1;
    memcpy(out, p, (size_t)n);
    out[n] = 0;
    *list = end;
    return 1;
}

/**** messages ****/

static void ctcp(struct chat *c, const struct irc_msg *m, int notice, const char *cmd, const char *arg, int arglen) {
    irc_text(text2, IRC_TEXT_MAX, arg, arglen);
    if(!strcmp(cmd, "DCC"))
        chat_say(c, NULL, LN_INFO, 0, "", "%s offered a DCC connection (%s), which Chat does not make", sender(m), text2);
    else if(notice) chat_say(c, NULL, LN_NOTICE, 0, sender(m), "CTCP %s reply: %s", cmd, text2);
    else chat_say(c, NULL, LN_INFO, 0, "", "%s sent a CTCP %s request, which Chat does not answer", sender(m), cmd);
}

static void on_message(struct chat *c, const struct irc_msg *m, int notice) {
    const char *target, *arg;
    char cmd[16];
    struct chat_room *r;
    int kind = notice ? LN_NOTICE : LN_MSG, flags = 0, arglen, op = 0;
    if(m->argc < 2) return;
    target = m->argv[0];
    if(target[0] && strchr(c->statusmsg, target[0]) && chat_is_channel(c, target + 1)) op = *target++;  /* @#chan */
    if(irc_ctcp(m->argv[1], cmd, sizeof(cmd), &arg, &arglen)) {
        if(strcmp(cmd, "ACTION")) {
            ctcp(c, m, notice, cmd, arg, arglen);
            return;
        }
        kind = LN_ACTION;
        irc_text(text, IRC_TEXT_MAX, arg, arglen);
    } else clean(text, m->argv[1]);
    if(!user_nick(c, m)) {                          /* the server's own, or a sender with no nick to answer */
        chat_say(c, NULL, kind, 0, sender(m), "%s", text);
        return;
    }
    if(chat_is_channel(c, target)) {
        r = chat_room_of(c, ROOM_CHANNEL, target);
        if(!r) {
            chat_say(c, NULL, kind, 0, m->nick, "[%s] %s", clean(text2, target), text);
            return;
        }
        if(!notice && chat_names_us(c, text)) flags = LF_HIGHLIGHT;
        if(op) chat_say(c, r, kind, flags, m->nick, "[%c] %s", op, text);
        else chat_say(c, r, kind, flags, m->nick, "%s", text);
        return;
    }
    if(!chat_is_me(c, target)) {                    /* to a mask, a channel type we do not know, anyone else */
        chat_say(c, NULL, kind, 0, m->nick, "[%s] %s", clean(text2, target), text);
        return;
    }
    r = chat_room_of(c, ROOM_QUERY, m->nick);
    if(notice) {
        chat_say(c, r ? r : active(c), kind, 0, m->nick, "%s", text);
        return;
    }
    if(!r) r = chat_room_new(c, ROOM_QUERY, m->nick);
    if(!r) chat_say(c, NULL, kind, LF_HIGHLIGHT, m->nick, "(private) %s", text);
    else chat_say(c, r, kind, LF_HIGHLIGHT, m->nick, "%s", text);
}

/**** channels and nicks ****/

static int asked(struct chat *c, const char *chan) {
    int i;
    for(i = 0; i < CHAT_JOINING; i++)
        if(c->joining[i][0] && !irc_casecmp(c->casemap, c->joining[i], chan)) {
            c->joining[i][0] = 0;
            return 1;
        }
    return 0;
}

static void on_join(struct chat *c, const struct irc_msg *m) {
    char chan[CHAT_CHAN_MAX + 2];
    const char *list;
    struct chat_room *r;
    int me;
    if(m->argc < 1 || !user_nick(c, m)) return;
    me = chat_is_me(c, m->nick);
    if(me && m->user[0] && m->host[0] && strlen(m->user) + strlen(m->host) + 1 < sizeof(c->user))
        snprintf(c->user, sizeof(c->user), "%s@%s", m->user, m->host);
    for(list = m->argv[0]; next_name(&list, chan, sizeof(chan));) {
        if(!irc_valid_channel(chan, CHAT_CHAN_MAX, c->chantypes)) continue;
        r = chat_room_of(c, ROOM_CHANNEL, chan);
        if(!me) {
            if(!r || !r->joined) continue;
            chat_nick_add(c, r, m->nick, 0);
            chat_say(c, r, LN_EVENT, 0, "", "%s (%s@%s) joined", m->nick, clean(text, m->user), clean(text2, m->host));
            continue;
        }
        if(!r && !(r = chat_room_new(c, ROOM_CHANNEL, chan))) {
            chat_say(c, NULL, LN_ERROR, 0, "", "You joined %s, but Chat has no room left to show it in: close a query "
                     "or leave a channel", clean(text, chan));
            continue;
        }
        memcpy(r->name, chan, strlen(chan) + 1);        /* as the server writes it (CHAT_CHAN_MAX at most) */
        r->joined = 1;
        r->parting = r->names_done = 0;
        chat_nicks_clear(c, r);
        set_topic(c, r, "");
        chat_say(c, r, LN_EVENT, 0, "", "You joined %s", clean(text, chan));
        if(asked(c, chan)) chat_select(c, chat_find(c, ROOM_CHANNEL, chan));
    }
}

static void on_part(struct chat *c, const struct irc_msg *m) {
    char chan[CHAT_CHAN_MAX + 2];
    const char *list;
    struct chat_room *r;
    int i;
    if(m->argc < 1 || !user_nick(c, m)) return;
    if(m->argc > 1 && m->argv[1][0]) snprintf(text2, sizeof(text2), " (%s)", clean(text, m->argv[1]));
    else text2[0] = 0;
    for(list = m->argv[0]; next_name(&list, chan, sizeof(chan));) {
        if(!(r = chat_room_of(c, ROOM_CHANNEL, chan)) || !r->joined) continue;
        if(chat_is_me(c, m->nick)) {
            if(r->parting) {                        /* our /part: its room goes */
                chat_room_drop(c, chat_find(c, ROOM_CHANNEL, chan));
                continue;
            }
            r->joined = 0;
            chat_nicks_clear(c, r);
            chat_say(c, r, LN_EVENT, 0, "", "You left %s%s", clean(text, chan), text2);
            continue;
        }
        if((i = chat_nick_find(c, r, m->nick, NULL)) >= 0) chat_nick_drop(c, r, i);
        chat_say(c, r, LN_EVENT, 0, "", "%s left%s", m->nick, text2);
    }
}

static void on_quit(struct chat *c, const struct irc_msg *m) {
    int i, k;
    if(!user_nick(c, m) || chat_is_me(c, m->nick)) return;
    if(m->argc > 0 && m->argv[0][0]) snprintf(text2, sizeof(text2), " (%s)", clean(text, m->argv[0]));
    else text2[0] = 0;
    for(i = 1; i < c->rooms; i++) {
        struct chat_room *r = c->room[i];
        if(r->kind == ROOM_CHANNEL && r->joined && (k = chat_nick_find(c, r, m->nick, NULL)) >= 0) {
            chat_nick_drop(c, r, k);
            chat_say(c, r, LN_EVENT, 0, "", "%s quit%s", m->nick, text2);
        } else if(r->kind == ROOM_QUERY && !irc_casecmp(c->casemap, r->name, m->nick))
            chat_say(c, r, LN_EVENT, 0, "", "%s quit%s", m->nick, text2);
    }
}

static void on_nick(struct chat *c, const struct irc_msg *m) {
    const char *to;
    int i, k, me;
    if(m->argc < 1 || !user_nick(c, m)) return;
    to = m->argv[0];
    if(!irc_valid_nick(to, CHAT_NICK_MAX, c->chantypes)) return;
    me = chat_is_me(c, m->nick);
    if(me) {
        snprintf(c->nick, sizeof(c->nick), "%s", to);
        chat_say(c, NULL, LN_EVENT, 0, "", "You are now known as %s", to);
        c->dirty |= CHAT_DIRTY_STATUS;
    }
    for(i = 1; i < c->rooms; i++) {
        struct chat_room *r = c->room[i];
        if(r->kind == ROOM_CHANNEL && r->joined && (k = chat_nick_find(c, r, m->nick, NULL)) >= 0) {
            chat_nick_rename(c, r, k, to);
            if(me) chat_say(c, r, LN_EVENT, 0, "", "You are now known as %s", to);
            else chat_say(c, r, LN_EVENT, 0, "", "%s is now known as %s", m->nick, to);
        } else if(r->kind == ROOM_QUERY && !irc_casecmp(c->casemap, r->name, m->nick)) {
            chat_say(c, r, LN_EVENT, 0, "", "%s is now known as %s", m->nick, to);
            if(chat_find(c, ROOM_QUERY, to) < 0) {  /* the query follows the nick, unless another holds it */
                snprintf(r->name, sizeof(r->name), "%s", to);
                c->dirty |= CHAT_DIRTY_ROOMS;
            }
        }
    }
}

static void on_kick(struct chat *c, const struct irc_msg *m) {
    struct chat_room *r;
    int i;
    if(m->argc < 2 || !(r = chat_room_of(c, ROOM_CHANNEL, m->argv[0])) || !r->joined) return;
    if(m->argc > 2 && m->argv[2][0]) snprintf(text2, sizeof(text2), " (%s)", clean(text, m->argv[2]));
    else text2[0] = 0;
    if(chat_is_me(c, m->argv[1])) {
        r->joined = r->parting = 0;
        chat_nicks_clear(c, r);
        chat_say(c, r, LN_EVENT, LF_HIGHLIGHT, "", "You were kicked from %s by %s%s", r->name, sender(m), text2);
        return;
    }
    if((i = chat_nick_find(c, r, m->argv[1], NULL)) >= 0) chat_nick_drop(c, r, i);
    chat_say(c, r, LN_EVENT, 0, "", "%s kicked %s%s", sender(m), clean(text, m->argv[1]), text2);
}

static void on_topic(struct chat *c, const struct irc_msg *m) {
    struct chat_room *r;
    if(m->argc < 1 || !(r = chat_room_of(c, ROOM_CHANNEL, m->argv[0]))) return;
    clean(text, m->argc > 1 ? m->argv[1] : "");
    set_topic(c, r, text);
    if(text[0]) chat_say(c, r, LN_EVENT, 0, "", "%s changed the topic to: %s", sender(m), text);
    else chat_say(c, r, LN_EVENT, 0, "", "%s removed the topic", sender(m));
}

/* a channel's MODE: the nicks' ranks (PREFIX's modes) changed; the other modes' parameters passed over by
   CHANMODES' groups: A and B always have one, C only when set, D none (an unknown mode as D) */
static void apply_modes(struct chat *c, struct chat_room *r, const struct irc_msg *m) {
    const char *s, *k;
    int set = 1, p = 2, i;
    for(s = m->argv[1]; *s; s++) {
        if(*s == '+' || *s == '-') {
            set = *s == '+';
            continue;
        }
        if((k = strchr(c->prefix_modes, *s)) != NULL) {
            if(p >= m->argc) break;
            if((i = chat_nick_find(c, r, m->argv[p++], NULL)) >= 0) {
                unsigned bit = 1u << (k - c->prefix_modes);
                if(set) r->nicks[i].modes |= bit;
                else r->nicks[i].modes &= ~bit;
                if(r == active(c)) c->dirty |= CHAT_DIRTY_NICKS;
            }
        } else if(strchr(c->chanmodes[0], *s) || strchr(c->chanmodes[1], *s) || (set && strchr(c->chanmodes[2], *s)))
            p++;
    }
}

static void on_mode(struct chat *c, const struct irc_msg *m) {
    struct chat_room *r;
    if(m->argc < 2) return;
    rest(m, 1, text);
    if(!chat_is_channel(c, m->argv[0]) || !(r = chat_room_of(c, ROOM_CHANNEL, m->argv[0]))) {
        chat_say(c, NULL, LN_EVENT, 0, "", "%s sets mode %s on %s", sender(m), text, clean(text2, m->argv[0]));
        return;
    }
    chat_say(c, r, LN_EVENT, 0, "", "%s sets mode %s", sender(m), text);
    if(r->joined) apply_modes(c, r, m);
}

/* RPL_NAMREPLY: RFC 2812's "<client> <symbol> <channel> :names", or RFC 1459's without the symbol; a name's leading
   PREFIX characters are its ranks (more than one with multi-prefix), anything from a '!' (userhost-in-names) not */
static void on_names(struct chat *c, const struct irc_msg *m) {
    const char *chan, *p;
    struct chat_room *r;
    char nick[CHAT_NICK_MAX + 2];
    int sym = m->argc >= 4 && m->argv[1][0] && strchr("=*@", m->argv[1][0]) && !m->argv[1][1];
    if(m->argc < 3) return;
    chan = m->argv[sym ? 2 : 1];
    p = m->argv[sym ? 3 : 2];
    if(!(r = chat_room_of(c, ROOM_CHANNEL, chan)) || !r->joined) return;
    if(r->names_done) {                             /* a new list: the old one goes */
        chat_nicks_clear(c, r);
        r->names_done = 0;
    }
    while(*p) {
        unsigned modes = 0;
        const char *k;
        int n = 0;
        while(*p == ' ') p++;
        while(*p && (k = strchr(c->prefix_chars, *p)) != NULL) {
            modes |= 1u << (k - c->prefix_chars);
            p++;
        }
        while(*p && *p != ' ' && *p != '!') {
            if(n < (int)sizeof(nick) - 1) nick[n++] = *p;
            p++;
        }
        while(*p && *p != ' ') p++;
        nick[n] = 0;
        if(n && irc_valid_nick(nick, CHAT_NICK_MAX, c->chantypes) && chat_nick_add(c, r, nick, modes) < 0 &&
           r->nicks_over == 1) {
            r->nicks_over = 2;                      /* said once */
            chat_say(c, r, LN_INFO, 0, "", "The list of %s is longer than Chat keeps: it shows %d nicks", r->name,
                     r->nnicks);
        }
    }
}

/**** registration ****/

/* the next nick to try at registration, from the one asked for: _ and __, then 2 to 8, in the length allowed */
static int next_nick(struct chat *c) {
    static const char *const ends[] = { "_", "__", "2", "3", "4", "5", "6", "7", "8" };
    int limit = (int)strlen(c->p.nick), keep = limit, e;
    if(c->tries >= (int)(sizeof(ends) / sizeof(ends[0]))) return 0;
    e = (int)strlen(ends[c->tries]);
    if(limit < 9) limit = 9;                        /* RFC 2812's nine at least */
    if(c->nicklen > 0 && c->nicklen < limit) limit = c->nicklen;
    if(keep + e > limit) keep = limit - e;
    if(keep < 1) return 0;
    snprintf(c->nick, sizeof(c->nick), "%.*s%s", keep, c->p.nick, ends[c->tries++]);
    return 1;
}

static void nick_refused(struct chat *c, const struct irc_msg *m) {
    const char *mid[1];
    const char *tried = m->argc >= 2 ? m->argv[1] : "";
    rest(m, 2, text2);
    if(c->state != CHAT_REGISTERING) {
        chat_say(c, active(c), LN_ERROR, 0, "", "%s: %s", clean(text, tried), text2);
        return;
    }
    if(irc_casecmp(c->casemap, tried, c->nick)) return;    /* about a nick tried before: the answer to ours comes */
    if(m->numeric == 432) {
        chat_say(c, NULL, LN_ERROR, 0, "", "The server does not take the nick %s (%s): choose another with /nick NAME",
                 clean(text, tried), text2);
        return;
    }
    if(c->tries < 0 || !next_nick(c)) {
        chat_say(c, NULL, LN_ERROR, 0, "", "%s is taken too: choose another nick with /nick NAME", clean(text, tried));
        return;
    }
    chat_say(c, NULL, LN_INFO, 0, "", "%s is taken: trying %s", clean(text, tried), c->nick);
    mid[0] = c->nick;
    chat_queue(c, 1, "NICK", mid, 1, NULL);
}

/* does a CAP list hold name, or name=value (*value then points at the value)? */
static int has_cap(const char *list, const char *name, const char **value) {
    size_t n = strlen(name);
    const char *p = list;
    while(*p) {
        const char *end;
        while(*p == ' ') p++;
        end = p;
        while(*end && *end != ' ') end++;
        if((size_t)(end - p) >= n && !strncmp(p, name, n) && (p + n == end || p[n] == '=')) {
            if(value) *value = p[n] == '=' ? p + n + 1 : NULL;
            return 1;
        }
        p = end;
    }
    return 0;
}

static void cap_choose(struct chat *c) {
    const char *mid[1] = { "REQ" };
    if(c->p.pass_kind == CHAT_PASS_SASL) {
        if(c->sasl_offered && c->sasl_plain && !chat_queue(c, 1, "CAP", mid, 1, "sasl")) {
            c->cap = CAP_REQ;
            return;
        }
        chat_say(c, NULL, LN_ERROR, 0, "", c->sasl_offered ? "The server's SASL has no PLAIN login: you are not logged in"
                                                           : "The server offers no SASL login: you are not logged in");
    }
    chat_cap_end(c);
}

static void on_cap(struct chat *c, const struct irc_msg *m) {
    const char *sub, *value, *list;
    if(m->argc < 3) return;
    sub = m->argv[1];
    list = m->argv[m->argc - 1];
    if(!irc_casecmp(IRC_CM_ASCII, sub, "LS")) {
        if(c->cap != CAP_LS) return;
        if(has_cap(list, "sasl", &value)) {
            c->sasl_offered = 1;
            if(!value) c->sasl_plain = 1;           /* no list of mechanisms (CAP 301): PLAIN is tried */
            else {
                const char *p = value;              /* sasl=PLAIN,EXTERNAL ... up to the token's end */
                while(*p && *p != ' ') {
                    size_t n = strcspn(p, ", ");
                    if(n == 5 && !strncasecmp(p, "PLAIN", 5)) c->sasl_plain = 1;
                    p += n;
                    if(*p == ',') p++;
                }
            }
        }
        if(!(m->argc >= 4 && !strcmp(m->argv[2], "*"))) cap_choose(c);  /* "*": more LS lines come */
    } else if(!irc_casecmp(IRC_CM_ASCII, sub, "ACK")) {
        const char *mid[1] = { "PLAIN" };
        if(c->cap != CAP_REQ) return;
        if(has_cap(list, "sasl", NULL) && !chat_queue(c, 1, "AUTHENTICATE", mid, 1, NULL)) c->sasl = SASL_ASKED;
        else chat_cap_end(c);
    } else if(!irc_casecmp(IRC_CM_ASCII, sub, "NAK")) {
        if(c->cap != CAP_REQ) return;
        chat_say(c, NULL, LN_ERROR, 0, "", "The server refused SASL: you are not logged in");
        chat_cap_end(c);
    }
}

/* AUTHENTICATE + after AUTHENTICATE PLAIN: RFC 4616's message, the authorization identity empty (the account's
   own), the account the nick asked for, then the password, in base64 lines of 400 (IRCv3 SASL 3.1), a last one
   of exactly 400 followed by "+". Every copy of the password is wiped. */
static void on_authenticate(struct chat *c, const struct irc_msg *m) {
    static unsigned char raw[CHAT_NICK_MAX + CHAT_PASS_MAX + 3];
    static char b64[(sizeof(raw) + 2) / 3 * 4 + 1], chunk[401];
    const char *mid[1];
    int n = 0, len, at, k;
    size_t a = strlen(c->p.nick), p = strnlen(c->p.pass, CHAT_PASS_MAX);
    if(c->sasl != SASL_ASKED) return;
    if(m->argc < 1 || strcmp(m->argv[0], "+")) {
        mid[0] = "*";
        chat_queue(c, 1, "AUTHENTICATE", mid, 1, NULL);
        chat_say(c, NULL, LN_ERROR, 0, "", "The server's SASL asked for something else: you are not logged in");
        chat_cap_end(c);
        return;
    }
    raw[n++] = 0;
    memcpy(raw + n, c->p.nick, a);
    n += (int)a;
    raw[n++] = 0;
    memcpy(raw + n, c->p.pass, p);
    n += (int)p;
    len = irc_base64(raw, n, b64, sizeof(b64));
    chat_wipe(raw, sizeof(raw));
    chat_forget_password(c);
    for(at = 0; at < len; at += 400) {
        k = len - at < 400 ? len - at : 400;
        memcpy(chunk, b64 + at, (size_t)k);
        chunk[k] = 0;
        mid[0] = chunk;
        chat_queue(c, 1, "AUTHENTICATE", mid, 1, NULL);
    }
    if(len % 400 == 0) {
        mid[0] = "+";
        chat_queue(c, 1, "AUTHENTICATE", mid, 1, NULL);
    }
    chat_wipe(b64, sizeof(b64));
    chat_wipe(chunk, sizeof(chunk));
    c->sasl = SASL_SENT;
}

/**** ISUPPORT (RPL_ISUPPORT, 005) ****/

static int hexval(char h) {
    if(h >= '0' && h <= '9') return h - '0';
    if(h >= 'a' && h <= 'f') return h - 'a' + 10;
    if(h >= 'A' && h <= 'F') return h - 'A' + 10;
    return -1;
}

/* a value with the draft's \xHH escapes undone, cleaned, into out */
static void unescape(const char *v, char *out, int size) {
    static char raw[IRC_LINE_MAX];
    int o = 0;
    while(*v && o < (int)sizeof(raw) - 1) {
        if(v[0] == '\\' && v[1] == 'x' && hexval(v[2]) >= 0 && hexval(v[3]) >= 0) {
            raw[o++] = (char)(hexval(v[2]) * 16 + hexval(v[3]));
            v += 4;
            continue;
        }
        raw[o++] = *v++;
    }
    irc_text(out, size, raw, o);
}

/* characters a list of channel types, status prefixes or modes may hold */
static int symbols_ok(const char *s, int max) {
    int n = 0;
    for(; *s; s++, n++) if(*s <= ' ' || *s > '~' || *s == ',' || n >= max) return 0;
    return 1;
}

static void prefix(struct chat *c, const char *v) {
    char modes[CHAT_PREFIXES + 1];
    const char *close;
    size_t n;
    if(!v) v = "(ov)@+";
    if(!*v) {                                       /* PREFIX= : no ranks */
        c->prefix_modes[0] = c->prefix_chars[0] = 0;
        return;
    }
    if(v[0] != '(' || !(close = strchr(v, ')'))) return;
    n = (size_t)(close - v - 1);
    if(n > CHAT_PREFIXES || strlen(close + 1) != n) return;
    memcpy(modes, v + 1, n);
    modes[n] = 0;
    if(!symbols_ok(modes, CHAT_PREFIXES) || !symbols_ok(close + 1, CHAT_PREFIXES)) return;
    strcpy(c->prefix_modes, modes);
    strcpy(c->prefix_chars, close + 1);
    c->dirty |= CHAT_DIRTY_NICKS;
}

static void isupport_token(struct chat *c, const char *t) {
    char key[24];
    const char *v;
    int off = t[0] == '-', i;
    size_t n;
    t += off;
    v = strchr(t, '=');
    n = v ? (size_t)(v - t) : strlen(t);
    if(!n || n >= sizeof(key)) return;
    memcpy(key, t, n);
    key[n] = 0;
    v = off ? NULL : v ? v + 1 : "";
    if(!strcmp(key, "PREFIX")) prefix(c, v);
    else if(!strcmp(key, "CHANTYPES")) {
        if(!v) strcpy(c->chantypes, "#&");
        else if(symbols_ok(v, (int)sizeof(c->chantypes) - 1)) strcpy(c->chantypes, v);
    } else if(!strcmp(key, "STATUSMSG")) {
        if(!v) c->statusmsg[0] = 0;
        else if(symbols_ok(v, (int)sizeof(c->statusmsg) - 1)) strcpy(c->statusmsg, v);
    } else if(!strcmp(key, "CHANMODES") && v) {
        char groups[4][33];
        const char *p = v;
        for(i = 0; i < 4; i++) {
            size_t k = strcspn(p, ",");
            if(k >= sizeof(groups[i])) return;
            memcpy(groups[i], p, k);
            groups[i][k] = 0;
            if(!symbols_ok(groups[i], 32)) return;
            p += k;
            if(*p == ',') p++;
            else if(i < 3) {                        /* fewer groups than four: the rest have none */
                for(i++; i < 4; i++) groups[i][0] = 0;
                break;
            }
        }
        memcpy(c->chanmodes, groups, sizeof(groups));
    } else if(!strcmp(key, "NICKLEN")) {
        long len = v ? strtol(v, NULL, 10) : 0;
        c->nicklen = len > 0 && len <= CHAT_NICK_MAX ? (int)len : 0;
    } else if(!strcmp(key, "CASEMAPPING")) {
        int cm = irc_casemap_named(v ? v : "rfc1459");
        if(cm != c->casemap) {
            c->casemap = cm;
            chat_nicks_resort(c);
        }
    } else if(!strcmp(key, "NETWORK")) {
        if(v) unescape(v, c->network, (int)sizeof(c->network));
        else c->network[0] = 0;
        c->dirty |= CHAT_DIRTY_ROOMS | CHAT_DIRTY_STATUS;
    }
}

/**** numerics ****/

static void numeric(struct chat *c, const struct irc_msg *m) {
    struct chat_room *r;
    int n = m->numeric, i;
    switch(n) {
    case 1:                                         /* RPL_WELCOME: registered, under the nick it names */
        if(m->argc >= 1 && irc_valid_nick(m->argv[0], CHAT_NICK_MAX, c->chantypes))
            snprintf(c->nick, sizeof(c->nick), "%s", m->argv[0]);
        if(c->state == CHAT_REGISTERING) c->state = CHAT_ONLINE;
        if(c->cap == CAP_LS || c->cap == CAP_REQ) c->cap = CAP_DONE;   /* it did not wait for CAP END */
        chat_forget_password(c);
        c->dirty |= CHAT_DIRTY_STATUS | CHAT_DIRTY_ROOMS;
        chat_say(c, NULL, LN_INFO, 0, "", "%s", rest(m, 1, text));
        return;
    case 5:                                         /* RPL_ISUPPORT: read, not shown (RFC 2812's RPL_BOUNCE is) */
        if(m->argc >= 2 && !strncmp(m->argv[m->argc - 1], "Try server", 10)) break;
        if(m->argc >= 3)
            for(i = 1; i < m->argc - 1; i++) isupport_token(c, m->argv[i]);
        return;
    case 301:                                       /* RPL_AWAY */
        if(m->argc < 3) break;
        r = chat_room_of(c, ROOM_QUERY, m->argv[1]);
        chat_say(c, r ? r : active(c), LN_INFO, 0, "", "%s is away: %s", clean(text, m->argv[1]), clean(text2, m->argv[2]));
        return;
    case 324:                                       /* RPL_CHANNELMODEIS */
        if(m->argc < 3 || !(r = chat_room_of(c, ROOM_CHANNEL, m->argv[1]))) break;
        chat_say(c, r, LN_INFO, 0, "", "Modes: %s", rest(m, 2, text));
        return;
    case 329:                                       /* RPL_CREATIONTIME: not shown */
    case 366:                                       /* RPL_ENDOFNAMES */
        if(n == 366 && m->argc >= 2 && (r = chat_room_of(c, ROOM_CHANNEL, m->argv[1]))) {
            r->names_done = 1;
            if(r == active(c)) c->dirty |= CHAT_DIRTY_NICKS;
        }
        return;
    case 331:                                       /* RPL_NOTOPIC */
    case 332:                                       /* RPL_TOPIC */
        if(m->argc < 2 || !(r = chat_room_of(c, ROOM_CHANNEL, m->argv[1]))) break;
        set_topic(c, r, n == 332 && m->argc >= 3 ? clean(text, m->argv[2]) : "");
        if(r->topic) chat_say(c, r, LN_EVENT, 0, "", "The topic: %s", r->topic);
        else chat_say(c, r, LN_EVENT, 0, "", "No topic is set");
        return;
    case 333:                                       /* RPL_TOPICWHOTIME */
        if(m->argc < 3 || !(r = chat_room_of(c, ROOM_CHANNEL, m->argv[1]))) break;
        clean(text, m->argv[2]);
        text[strcspn(text, "!")] = 0;
        chat_say(c, r, LN_EVENT, 0, "", "Set by %s", text);
        return;
    case 353:                                       /* RPL_NAMREPLY */
        on_names(c, m);
        return;
    case 396:                                       /* RPL_VISIBLEHOST: the host the server now shows */
        if(m->argc >= 2 && strchr(c->user, '@') && strchr(c->user, '@') - c->user + 1 + strlen(m->argv[1]) <
           sizeof(c->user))
            strcpy(strchr(c->user, '@') + 1, m->argv[1]);
        break;
    case 421:                                       /* ERR_UNKNOWNCOMMAND */
    case 451:                                       /* ERR_NOTREGISTERED */
        if(m->argc >= 2 && !irc_casecmp(IRC_CM_ASCII, m->argv[1], "CAP") && (c->cap == CAP_LS || c->cap == CAP_REQ)) {
            if(c->p.pass_kind == CHAT_PASS_SASL)
                chat_say(c, NULL, LN_ERROR, 0, "", "The server does not know CAP: you are not logged in by SASL");
            c->cap = CAP_DONE;
            chat_forget_password(c);
            return;
        }
        break;
    case 432:                                       /* ERR_ERRONEUSNICKNAME */
    case 433:                                       /* ERR_NICKNAMEINUSE */
    case 436:                                       /* ERR_NICKCOLLISION */
    case 437:                                       /* ERR_UNAVAILRESOURCE */
        nick_refused(c, m);
        return;
    case 464:                                       /* ERR_PASSWDMISMATCH */
        chat_say(c, NULL, LN_ERROR, 0, "", "The server refused the password: %s", rest(m, 1, text));
        return;
    case 900:                                       /* RPL_LOGGEDIN */
        if(m->argc < 3) break;
        chat_say(c, NULL, LN_INFO, 0, "", "You are logged in as %s", clean(text, m->argv[2]));
        return;
    case 903:                                       /* RPL_SASLSUCCESS */
        if(c->sasl != SASL_SENT) break;
        chat_say(c, NULL, LN_INFO, 0, "", "%s", rest(m, 1, text));
        chat_cap_end(c);
        return;
    case 902:                                       /* ERR_NICKLOCKED */
    case 904:                                       /* ERR_SASLFAIL */
    case 905:                                       /* ERR_SASLTOOLONG */
    case 906:                                       /* ERR_SASLABORTED */
    case 907:                                       /* ERR_SASLALREADY */
        if(c->sasl != SASL_ASKED && c->sasl != SASL_SENT) break;
        chat_say(c, NULL, LN_ERROR, 0, "", "The SASL login failed (%s): you are not logged in", rest(m, 1, text));
        chat_cap_end(c);                            /* registration goes on without the login */
        return;
    }
    if(n < 400 || n >= 600) chat_say(c, NULL, LN_INFO, 0, "", "%s", rest(m, 1, text));
    else if(m->argc >= 3)                           /* an error about a channel, a nick, a command: named first */
        chat_say(c, active(c), LN_ERROR, 0, "", "%s: %s", clean(text2, m->argv[1]), rest(m, 2, text));
    else chat_say(c, active(c), LN_ERROR, 0, "", "%s", rest(m, 1, text));
}

/**** the commands ****/

static void on_ping(struct chat *c, const struct irc_msg *m) {
    int i, pongs = 0;
    for(i = 0; i < c->ucount; i++) if(!strncmp(c->urgent[(c->uhead + i) % CHAT_URGENT], "PONG", 4)) pongs++;
    if(pongs >= PONGS) return;
    if(!m->argc || chat_queue(c, 1, "PONG", NULL, 0, m->argv[m->argc - 1])) chat_queue(c, 1, "PONG", NULL, 0, NULL);
}

static void dispatch(struct chat *c, const struct irc_msg *m) {
    const char *cmd = m->command;
    if(m->numeric >= 0) numeric(c, m);
    else if(!strcmp(cmd, "PING")) on_ping(c, m);
    else if(!strcmp(cmd, "PONG")) ;                 /* that the server spoke is enough */
    else if(!strcmp(cmd, "PRIVMSG")) on_message(c, m, 0);
    else if(!strcmp(cmd, "NOTICE")) on_message(c, m, 1);
    else if(!strcmp(cmd, "JOIN")) on_join(c, m);
    else if(!strcmp(cmd, "PART")) on_part(c, m);
    else if(!strcmp(cmd, "QUIT")) on_quit(c, m);
    else if(!strcmp(cmd, "NICK")) on_nick(c, m);
    else if(!strcmp(cmd, "KICK")) on_kick(c, m);
    else if(!strcmp(cmd, "TOPIC")) on_topic(c, m);
    else if(!strcmp(cmd, "MODE")) on_mode(c, m);
    else if(!strcmp(cmd, "CAP")) on_cap(c, m);
    else if(!strcmp(cmd, "AUTHENTICATE")) on_authenticate(c, m);
    else if(!strcmp(cmd, "INVITE") && m->argc >= 2)
        chat_say(c, active(c), LN_INFO, LF_HIGHLIGHT, "", "%s invites you to %s (to go: /join %s)", sender(m),
                 clean(text, m->argv[1]), text);
    else if(!strcmp(cmd, "KILL") && m->argc >= 1 && chat_is_me(c, m->argv[0]))
        chat_say(c, NULL, LN_ERROR, 0, "", "%s disconnected you: %s", sender(m), rest(m, 1, text));
    else if(!strcmp(cmd, "ERROR"))
        chat_say(c, NULL, c->state == CHAT_QUITTING ? LN_INFO : LN_ERROR, 0, "", "The server ends the connection: %s",
                 rest(m, 0, text));
    else if(!strcmp(cmd, "WALLOPS")) chat_say(c, NULL, LN_NOTICE, 0, sender(m), "%s", rest(m, 0, text));
    else chat_say(c, NULL, LN_INFO, 0, sender(m), "%s %s", cmd, rest(m, 0, text));
}

void chat_recv(struct chat *c, const char *in, int n) {
    static struct irc_msg m;
    int used, r;
    if(c->state < CHAT_REGISTERING || n <= 0) return;
    c->last_rx = c->now;
    c->ping_at = 0;
    while(n > 0) {
        r = irc_read(&c->rd, in, n, &used);
        in += used;
        n -= used;
        if(r == IRC_LONG) c->long_lines++;
        else if(r == IRC_BAD) c->bad_lines++;
        else if(r == IRC_LINE) {
            if(irc_parse(c->rd.line, &m)) c->bad_lines++;
            else dispatch(c, &m);
        }
    }
    if(c->long_lines + c->bad_lines > c->dropped_told &&
       (!c->dropped_told || chat_since(c, c->dropped_at) >= DROPPED_MS)) {
        chat_say(c, NULL, LN_ERROR, 0, "", "The server sent lines that are no IRC messages, or over %d bytes: "
                 "%lu left out so far", IRC_IN_MAX, c->long_lines + c->bad_lines);
        c->dropped_told = c->long_lines + c->bad_lines;
        c->dropped_at = c->now;
    }
}
