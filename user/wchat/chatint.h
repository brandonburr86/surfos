/*
SurfOS chat client: wchat
--------------------
File: chatint.h Date: 11/05/26
--------------------
What chat.c, recv.c and input.c share and the window does not see: the negotiation's states, the
rooms' and the nick lists' changes, the lines shown and the lines going out.
*/

#ifndef _WCHAT_CHATINT_H
#define _WCHAT_CHATINT_H

#include <stddef.h>
#include "chat.h"

#define CHAT_TEXT_MAX (IRC_TEXT_MAX + 512)  /* a line shown: a server's text and our words around it */

enum { CAP_NONE, CAP_LS, CAP_REQ, CAP_DONE };
enum { SASL_NONE, SASL_ASKED, SASL_SENT, SASL_DONE };

/* rooms */
struct chat_room *chat_room_new(struct chat *c, int kind, const char *name);  /* NULL: none left */
void chat_room_drop(struct chat *c, int i);
struct chat_room *chat_room_of(struct chat *c, int kind, const char *name);    /* NULL when there is none */

/* A line in room r (the server's room when r is NULL), its text made by printf from fmt: what
   comes from a server goes in through "%s" after irc_text() cleaned it. */
void chat_say(struct chat *c, struct chat_room *r, int kind, int flags, const char *from, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));

int chat_is_me(const struct chat *c, const char *nick);
int chat_is_channel(const struct chat *c, const char *name);
int chat_names_us(const struct chat *c, const char *text);     /* text names our nick, as a word */

/* the nick lists */
int chat_nick_find(const struct chat *c, const struct chat_room *r, const char *nick, int *at);
int chat_nick_add(struct chat *c, struct chat_room *r, const char *nick, unsigned modes);
void chat_nick_drop(struct chat *c, struct chat_room *r, int i);
int chat_nick_rename(struct chat *c, struct chat_room *r, int i, const char *to);
void chat_nicks_clear(struct chat *c, struct chat_room *r);
void chat_nicks_resort(struct chat *c);

/* A line to send (irc_compose()'s parts), queued: 0, or -1 when it cannot be put together or the queue is
   full. urgent: before everything else, unpaced. */
int chat_queue(struct chat *c, int urgent, const char *cmd, const char *const *mid, int nmid, const char *trail);
/* PRIVMSG to target, or a CTCP ACTION, in as many lines as it takes, each shown in room r as ours */
int chat_send_text(struct chat *c, struct chat_room *r, const char *target, const char *text, int action);

void chat_wipe(void *p, size_t n);          /* bytes overwritten in a way the compiler keeps */
void chat_utf8_trim(char *s);               /* a character cut at s's end goes */
void chat_forget_password(struct chat *c);
void chat_isupport_defaults(struct chat *c);
void chat_cap_end(struct chat *c);
void chat_lose(struct chat *c, const char *why);    /* the connection must end: chat.ending */
int chat_since(const struct chat *c, unsigned when);   /* milliseconds from when to now, at least 0 */

/* 11/09/26 (the review) */
void chat_room_name(struct chat_room *r, const char *name);   /* its name, and the label shown for it */
int chat_password_may_go(const struct chat *c);     /* over TLS, or without it as the person agreed */
int chat_secret_to(const struct chat *c, const char *target);  /* NickServ: what goes to it carries passwords */
/* each file's static buffers, NULL-ended, for the tests to look through for a password (tests/host/test_wchat.c) */
struct chat_buf { const void *at; size_t size; };
extern const struct chat_buf chat_bufs[], chat_recv_bufs[], chat_input_bufs[];

#endif
