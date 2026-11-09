/*
SurfOS chat client: tests of ut_transport_read_now(), the read that never waits
--------------------
File: test_wchat_transport.c  Date: 11/09/26
--------------------
The one function the chat track added to Undertow's transport (user/undertow/transport.c, by
insertion, 11/09/26, the review): wchat's helper waits in poll() on its two descriptors and reads the
server with ut_transport_read_now(), which gives what has come, the end, a failure, or
UT_TRANSPORT_AGAIN when a record held no data (a TLS 1.3 session ticket) or only part of one has come,
and never waits. The world is Undertow's own transport test, tests/host/test_undertow_transport.c,
taken in whole with its main() renamed: its simulated network moves the clock only when something
waits, and counts every poll() that would (poll_waits), and its TLS 1.3 server sends two session
tickets after its handshake, as OpenSSL's do, then nothing until the client speaks: the server that
would have held ut_transport_read() for UT_IDLE_MS. Its first argument is the certificates' directory.
*/

#define main undertow_transport_main
#include "test_undertow_transport.c"
#undef main

/* reads with ut_transport_read_now() until the end or a failure, as the helper does after each poll(): what came */
static int read_all_now(struct ut_transport *t, char *got, size_t cap, int *agains) {
    size_t n = 0;
    int r = UT_TRANSPORT_AGAIN, tries;
    for(tries = 0; tries < 1000 && n < cap - 1; tries++) {
        r = ut_transport_read_now(t, got + n, cap - 1 - n);
        if(r == UT_TRANSPORT_AGAIN) {
            (*agains)++;
            continue;
        }
        if(r <= 0) break;
        n += (size_t)r;
    }
    got[n] = 0;
    return r;
}

static void read_now(int tls) {
    static char got[4096];
    struct ut_transport *t = NULL;
    const char *what = tls ? "read_now, TLS 1.3" : "read_now, plain TCP";
    char msg[200];
    unsigned int waits, at;
    int r, i, agains = 0, failed = 0;
    reset_world();
    if(tls) plan_tls(0, 0, 0);
    else plan_plain();
    snprintf(msg, sizeof(msg), "%s: connected", what);
    check(open_at(tls ? "https://fixture.test/" : "http://fixture.test/", &t) == 0, msg);
    if(!t) return;
    waits = poll_waits;
    at = sim_ms;
    for(i = 0; i < 5; i++)                      /* the tickets, or nothing: no data, no failure, no wait */
        if(ut_transport_read_now(t, got, sizeof(got)) != UT_TRANSPORT_AGAIN) failed++;
    snprintf(msg, sizeof(msg), "%s: before the request, nothing to read: UT_TRANSPORT_AGAIN five times", what);
    check(!failed, msg);
    snprintf(msg, sizeof(msg), "%s: and no wait, the clock where it was", what);
    check(poll_waits == waits && sim_ms == at, msg);
    if(tls) {
        check(net.tickets == 2 && net.down_off == net.down_len, "read_now, TLS 1.3: the server's two tickets read, and dropped");
    }
    check(ut_transport_write(t, REQUEST, strlen(REQUEST)) == (int)strlen(REQUEST), "read_now: the request written");
    r = read_all_now(t, got, sizeof(got), &agains);
    snprintf(msg, sizeof(msg), "%s: the response whole, then the end (%d, \"%.60s\")", what, r, got);
    check(r == 0 && !strcmp(got, RESPONSE), msg);
    snprintf(msg, sizeof(msg), "%s: an end as it should be (close_notify over TLS)", what);
    check(ut_transport_eof_clean(t), msg);
    snprintf(msg, sizeof(msg), "%s: still no wait, and no read() that poll() did not allow", what);
    check(poll_waits == waits && bad_calls == 0, msg);
    check(ut_transport_read_now(t, got, sizeof(got)) == 0, "read_now: after the end, the end again");
    ut_transport_close(t);
    check_clean(what);
}

/* the load's deadline holds as for ut_transport_read(): past it, a failure that says so, and nothing more */
static void read_now_late(void) {
    static char got[256];
    struct ut_transport *t = NULL;
    reset_world();
    plan_plain();
    check(open_at("http://fixture.test/", &t) == 0, "read_now late: connected");
    if(!t) return;
    sim_ms += UT_LOAD_MS + 1000;
    check(ut_transport_read_now(t, got, sizeof(got)) == -1 && strlen(ut_transport_error(t)) > 0 && safe_line(ut_transport_error(t)),
          "read_now late: past the deadline, -1 and a message");
    check(ut_transport_read_now(t, got, sizeof(got)) == -1, "read_now late: and -1 after that");
    check(ut_transport_read_now(NULL, got, sizeof(got)) == -1 && ut_transport_read_now(t, got, 0) == -1,
          "read_now: no transport, or no room, -1");
    ut_transport_close(t);
    check_clean("read_now late");
}

int main(int argc, char **argv) {
    const char *tmp = getenv("TMPDIR");
    static char ca[1024];
    if(argc > 1) certdir = argv[1];
    snprintf(tmpdir, sizeof(tmpdir), "%s", tmp && *tmp ? tmp : "/tmp");
    if(tmpdir[strlen(tmpdir) - 1] == '/') tmpdir[strlen(tmpdir) - 1] = 0;
    snprintf(ca, sizeof(ca), "%s/ca.pem", certdir);
    ut_tls_roots_path = ca;
    sim_ms = 100000;
    ut_config_init(&cfg);
    {
        static const char hosts[] = "host=fixture.test 10.0.2.2\n";
        check(ut_config_parse(&cfg, hosts, strlen(hosts), err, sizeof(err)) == 0, "the host table");
    }
    read_now(1);
    read_now(0);
    read_now_late();
    ut_transport_cleanup();
    check(ut_alloc_live == 0, "everything freed at the end");
    printf("test_wchat_transport: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
