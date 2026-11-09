/*
SurfOS chat client: wchat
--------------------
File: input.c   Date: 11/05/26
--------------------
What is typed into the Chat's input line (chat.h's chat_input()). A line is a
message for the room in front, or one of the commands: /join /part /msg /me /nick /topic /query
/quit. "//" at the start sends a message that starts with one "/". A line is made fit to send
before anything else: its line ends and tabs become spaces (a paste of several lines is one
message, never a second command), the controls go but the formatting codes (\x01 too: CTCP is
/me's alone), and it is cut at CHAT_INPUT_MAX bytes. A message too long for one PRIVMSG goes in
several (chat_send_text()). A name typed (a channel, a nick) is checked as one from a server is
before it goes into a line, so it cannot carry a second parameter or command; what went wrong
is a line in the room, and nothing is sent. The lines typed before are the input field's to bring
back (gk_field_history()).
*/

#include <stdio.h>
#include <string.h>
#include "chatint.h"

#define COMMANDS "/join /part /msg /me /nick /topic /query /quit"

static char t[CHAT_INPUT_MAX + 1], shown[IRC_TEXT_MAX];
static char join_names[CHAT_INPUT_MAX + 1], join_list[IRC_LINE_MAX], join_keys[IRC_LINE_MAX];
static char part_chan[CHAT_CHAN_MAX + 2], msg_to[CHAT_CHAN_MAX + 2], nick_name[CHAT_NICK_MAX + 2],
            query_who[CHAT_NICK_MAX + 2];

/* the static buffers of this file: test_wchat looks through them for a password (11/09/26, the review) */
const struct chat_buf chat_input_bufs[] = {
    { t, sizeof(t) }, { shown, sizeof(shown) }, { join_names, sizeof(join_names) }, { join_list, sizeof(join_list) },
    { join_keys, sizeof(join_keys) }, { part_chan, sizeof(part_chan) }, { msg_to, sizeof(msg_to) },
    { nick_name, sizeof(nick_name) }, { query_who, sizeof(query_who) }, { NULL, 0 }
};

static const char *show(const char *s) {
    irc_text(shown, sizeof(shown), s, (int)strnlen(s, CHAT_INPUT_MAX));
    return shown;
}

static void tidy(char *out, const char *in) {
    int o = 0;
    for(; *in && o < CHAT_INPUT_MAX; in++) {
        unsigned char ch = (unsigned char)*in;
        if(ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
        else if((ch < 0x20 && !irc_is_code(ch)) || ch == 0x7f) continue;
        out[o++] = (char)ch;
    }
    out[o] = 0;
    if(*in) chat_utf8_trim(out);                    /* cut: not inside a character */
}

/* the next word of *p into out (size bytes): its length, 0 when there is none, -1 when it does not fit */
static int word(const char **p, char *out, int size) {
    const char *s = *p;
    int n = 0, over = 0;
    while(*s == ' ') s++;
    while(*s && *s != ' ') {
        if(n < size - 1) out[n++] = *s;
        else over = 1;
        s++;
    }
    out[n] = 0;
    while(*s == ' ') s++;
    *p = s;
    return over ? -1 : n;
}

/**** the commands ****/

static int error(struct chat *c, const char *fmt, const char *arg) {
    chat_say(c, c->room[c->active], LN_ERROR, 0, "", fmt, arg);
    return -1;
}

/* nothing but registration's own lines goes before the server's welcome */
static int online(struct chat *c) {
    if(c->state == CHAT_ONLINE) return 1;
    if(c->state == CHAT_OFF) error(c, "%s", "Not connected: nothing was sent");
    else if(c->state == CHAT_QUITTING) error(c, "%s", "Leaving the server: nothing was sent");
    else error(c, "%s", "Not registered yet: wait for the server's welcome");
    return 0;
}

/* a channel /join asked for, whose room is shown when it comes; the oldest of CHAT_JOINING goes */
static void expect(struct chat *c, const char *chan) {
    int i;
    for(i = 0; i < CHAT_JOINING && c->joining[i][0]; i++) ;
    if(i == CHAT_JOINING) {
        memmove(c->joining[0], c->joining[1], sizeof(c->joining) - sizeof(c->joining[0]));
        i = CHAT_JOINING - 1;
    }
    memcpy(c->joining[i], chan, strlen(chan) + 1);  /* a channel's name, CHAT_CHAN_MAX at most */
}

static void say(struct chat *c, const char *text, int action) {
    struct chat_room *r = c->room[c->active];
    if(!online(c)) return;
    if(r->kind == ROOM_SERVER) {
        error(c, "%s", "This is the server's room: join a channel (/join #name) or talk to someone (/query nick)");
        return;
    }
    if(r->kind == ROOM_CHANNEL && !r->joined) {
        error(c, "You are not in %s: /join it first", show(r->name));
        return;
    }
    chat_send_text(c, r, r->name, text, action);
}

/* /join #a,#b [keys]: a name without a channel type gets the server's first one; the first room to come is shown */
static void join(struct chat *c, const char *p) {
    char *names = join_names, *list = join_list, *keys = join_keys;
    char one[CHAT_CHAN_MAX + 2], type[2] = { c->chantypes[0], 0 };
    const char *mid[2], *q;
    int o = 0, k, i;
    if(!online(c)) return;
    if(word(&p, names, sizeof(join_names)) <= 0) {
        error(c, "%s", "/join #channel [key]: which channel?");
        return;
    }
    k = word(&p, keys, sizeof(join_keys));
    if(k < 0) {
        error(c, "%s", "That key is too long");
        return;
    }
    for(q = names; *q;) {
        int n = (int)strcspn(q, ",");
        int typed = chat_is_channel(c, q) || !type[0];  /* "surf" is #surf */
        if(n + !typed > CHAT_CHAN_MAX) {
            error(c, "%s", "A channel's name has 200 characters at most");
            return;
        }
        snprintf(one, sizeof(one), "%s%.*s", typed ? "" : type, n, q);
        if(n && !irc_valid_channel(one, CHAT_CHAN_MAX, c->chantypes)) {
            error(c, "%s is not a channel's name", show(one));
            return;
        }
        if(n) {
            if(o + (int)strlen(one) + 2 > (int)sizeof(join_list)) {
                error(c, "%s", "Too many channels at once");
                return;
            }
            o += snprintf(list + o, sizeof(join_list) - (size_t)o, "%s%s", o ? "," : "", one);
        }
        q += n;
        if(*q == ',') q++;
    }
    if(!o) {
        error(c, "%s", "/join #channel [key]: which channel?");
        return;
    }
    if(!strchr(list, ',') && (i = chat_find(c, ROOM_CHANNEL, list)) >= 0 && c->room[i]->joined) {
        chat_select(c, i);                          /* in it already: its room comes in front */
        return;
    }
    mid[0] = list;
    mid[1] = keys;
    if(chat_queue(c, 0, "JOIN", mid, k > 0 ? 2 : 1, NULL)) {
        error(c, "%s", "Not sent: the key cannot be sent, or too much is waiting to go");
        return;
    }
    for(q = list; *q;) {                            /* each room is shown when it comes */
        int n = (int)strcspn(q, ",");
        snprintf(one, sizeof(one), "%.*s", n, q);
        expect(c, one);
        q += n;
        if(*q == ',') q++;
    }
}

/* /part [#channel] [reason]: the room's channel unless one is named */
static void part(struct chat *c, const char *p) {
    char *chan = part_chan;
    struct chat_room *r = c->room[c->active];
    const char *mid[1], *was = p;
    if(!online(c)) return;
    if(chat_is_channel(c, p)) {
        if(word(&p, chan, sizeof(part_chan)) < 0 || !irc_valid_channel(chan, CHAT_CHAN_MAX, c->chantypes)) {
            error(c, "%s is not a channel's name", show(was));
            return;
        }
        r = chat_room_of(c, ROOM_CHANNEL, chan);
    } else if(r->kind == ROOM_CHANNEL) snprintf(chan, sizeof(part_chan), "%s", r->name);
    else {
        error(c, "%s", "/part #channel [reason]: which channel?");
        return;
    }
    mid[0] = chan;
    if(chat_queue(c, 0, "PART", mid, 1, *p ? p : NULL)) {
        error(c, "%s", "Not sent: the reason is too long, or too much is waiting to go");
        return;
    }
    if(r) r->parting = 1;
}

/* /msg target text: shown in the target's room when it has one, else in this one as sent to it */
static void msg(struct chat *c, const char *p) {
    char *to = msg_to;
    struct chat_room *r;
    int channel;
    if(!online(c)) return;
    if(word(&p, to, sizeof(msg_to)) <= 0 || !*p) {
        error(c, "%s", "/msg nick text: to whom, and what?");
        return;
    }
    channel = chat_is_channel(c, to);
    if(channel ? !irc_valid_channel(to, CHAT_CHAN_MAX, c->chantypes) : !irc_valid_nick(to, CHAT_NICK_MAX, c->chantypes)) {
        error(c, "%s is not a nick or a channel", show(to));
        return;
    }
    r = chat_room_of(c, channel ? ROOM_CHANNEL : ROOM_QUERY, to);
    chat_send_text(c, r ? r : c->room[c->active], to, p, 0);
}

static void nick(struct chat *c, const char *p) {
    char *name = nick_name;
    const char *mid[1];
    if(word(&p, name, sizeof(nick_name)) <= 0 || strlen(name) > 32 || !irc_valid_nick(name, CHAT_NICK_MAX, c->chantypes)) {
        error(c, "%s", "/nick NAME: a nick of at most 32 characters, with no space, comma or ! @ * ?");
        return;
    }
    mid[0] = name;
    if(c->state == CHAT_REGISTERING) {              /* the server has none of ours yet: this one, and no more guesses */
        if(chat_queue(c, 1, "NICK", mid, 1, NULL)) return;
        memcpy(c->nick, name, strlen(name) + 1);    /* 32 bytes at most, checked above */
        c->tries = -1;
        return;
    }
    if(!online(c)) return;
    if(chat_queue(c, 0, "NICK", mid, 1, NULL)) error(c, "%s", "Not sent: too much is waiting to go");
}

/* /topic [text]: without text the server is asked, and its answer is shown */
static void topic(struct chat *c, const char *p) {
    struct chat_room *r = c->room[c->active];
    const char *mid[1];
    if(!online(c)) return;
    if(r->kind != ROOM_CHANNEL || !r->joined) {
        error(c, "%s", "A topic is a channel's: /topic in the room of a channel you are in");
        return;
    }
    mid[0] = r->name;
    if(chat_queue(c, 0, "TOPIC", mid, 1, *p ? p : NULL)) error(c, "%s", "Not sent: too much is waiting to go");
}

/* /query nick [text]: its room, made if need be, in front */
static void query(struct chat *c, const char *p) {
    char *who = query_who;
    struct chat_room *r;
    if(word(&p, who, sizeof(query_who)) <= 0 || !irc_valid_nick(who, CHAT_NICK_MAX, c->chantypes)) {
        error(c, "%s", "/query nick [text]: with whom?");
        return;
    }
    if(!(r = chat_room_of(c, ROOM_QUERY, who)) && !(r = chat_room_new(c, ROOM_QUERY, who))) {
        error(c, "%s", "No room is left: close a query or leave a channel");
        return;
    }
    chat_select(c, chat_find(c, ROOM_QUERY, who));
    if(*p && online(c)) chat_send_text(c, r, r->name, p, 0);
}

static void command(struct chat *c, const char *s) {
    char name[16];
    const char *p = s;
    int i, n = word(&p, name, sizeof(name));
    for(i = 0; name[i]; i++) if(name[i] >= 'A' && name[i] <= 'Z') name[i] = (char)(name[i] + 32);
    if(!n) error(c, "%s", "A command after the /: " COMMANDS);
    else if(n < 0) error(c, "There is no command /%s... The commands: " COMMANDS, show(name));
    else if(!strcmp(name, "join")) join(c, p);
    else if(!strcmp(name, "part")) part(c, p);
    else if(!strcmp(name, "msg")) msg(c, p);
    else if(!strcmp(name, "me")) {
        if(*p) say(c, p, 1);
        else error(c, "%s", "/me action: doing what?");
    } else if(!strcmp(name, "nick")) nick(c, p);
    else if(!strcmp(name, "topic")) topic(c, p);
    else if(!strcmp(name, "query")) query(c, p);
    else if(!strcmp(name, "quit")) {
        if(c->state == CHAT_OFF) error(c, "%s", "Not connected");
        else chat_quit(c, p);
    } else error(c, "There is no command /%s. The commands: " COMMANDS, show(name));
}

void chat_input(struct chat *c, const char *line) {
    tidy(t, line);
    if(t[strspn(t, " ")]) {                         /* not nothing but spaces */
        if(t[0] == '/' && t[1] != '/') command(c, t + 1);
        else say(c, t[0] == '/' ? t + 1 : t, 0);
    }
    chat_wipe(t, sizeof(t));                        /* what was typed may have held a password (NickServ's) */
}

/* 11/09/26 (the review): /msg NickServ ..., /query NickServ ..., and a line or /me typed in NickServ's query carry
   NickServ's passwords (IDENTIFY, REGISTER, GHOST ...): the window keeps them out of its history */
int chat_secret(const struct chat *c, const char *line) {
    char typed[CHAT_INPUT_MAX + 1], cmd[16], to[CHAT_NICK_MAX + 2];
    const char *p = typed;
    const struct chat_room *r = c->room[c->active];
    int i, in_query = r->kind == ROOM_QUERY && chat_secret_to(c, r->name), secret = 0;
    tidy(typed, line);
    if(typed[0] != '/' || typed[1] == '/') secret = in_query;
    else {
        p++;
        if(word(&p, cmd, sizeof(cmd)) > 0) {
            for(i = 0; cmd[i]; i++) if(cmd[i] >= 'A' && cmd[i] <= 'Z') cmd[i] = (char)(cmd[i] + 32);
            if(!strcmp(cmd, "me")) secret = in_query;
            else if((!strcmp(cmd, "msg") || !strcmp(cmd, "query")) && word(&p, to, sizeof(to)) > 0)
                secret = chat_secret_to(c, to);
        }
    }
    chat_wipe(typed, sizeof(typed));
    return secret;
}
