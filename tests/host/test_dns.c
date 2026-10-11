/*
SurfOS DNS packet and resolver tests
--------------------
File: test_dns.c Date: 10/10/26 (Undertow)
--------------------
Constructed packets only; never depends on a public DNS service.
*/
#include <stdio.h>
#include <string.h>
#include <net/dns_wire.h>
#include <net/ip.h>
#include <net/dhcp.h>
#include <surfos/sysimpl.h>
#include <fs/vfs.h>

static int checks, failures;
static void check(int ok, const char *name) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", name); }
}
static u8 packet[1600];
static u_int used;
static void word(u_int off, u16 v) { packet[off] = v >> 8; packet[off + 1] = v; }
static u_int name(const char *s) {
    u_int start = used;
    while(*s) {
        const char *dot = strchr(s, '.');
        u_int n = dot ? (u_int)(dot - s) : strlen(s);
        packet[used++] = n;
        memcpy(packet + used, s, n); used += n;
        s += n; if(*s) s++;
    }
    packet[used++] = 0;
    return start;
}
static void begin(const char *question, u16 count) {
    memset(packet, 0, sizeof(packet)); used = 12;
    word(0, 0x1234); word(2, 0x8180); word(4, 1); word(6, count);
    name(question); word(used, 1); word(used + 2, 1); used += 4;
}
static u_int rr(const char *owner, u16 type, u16 cls, u16 bytes) {
    if(owner) name(owner); else { packet[used++] = 0xc0; packet[used++] = 12; }
    word(used, type); word(used + 2, cls); word(used + 8, bytes); used += 10;
    return used;
}
static void address(const char *owner, u16 cls) {
    rr(owner, 1, cls, 4);
    packet[used++] = 192; packet[used++] = 0; packet[used++] = 2; packet[used++] = 7;
}
static int parse(ipaddr_t *ip) { return dns_parse_a(packet, used, 0x1234, "www.example.test", ip); }
static void packets(void) {
    ipaddr_t ip = 0, sentinel = 0x12345678;
    u_int off, end;
    begin("www.example.test", 1); address(NULL, 1);
    check(parse(&ip) == 0 && ip == IP4(192, 0, 2, 7), "compressed_a_resolves");
    begin("WWW.Example.Test", 1); address(NULL, 1);
    check(parse(&ip) == 0, "question_case_insensitive");
    begin("www.example.test", 2);
    off = rr(NULL, 5, 1, 0); name("alias.example.test"); word(off - 2, used - off);
    /* The A owner is a compressed pointer into the CNAME's RDATA. */
    packet[used++] = 0xc0 | (off >> 8); packet[used++] = off;
    word(used, 1); word(used + 2, 1); word(used + 8, 4); used += 10;
    packet[used++] = 192; packet[used++] = 0; packet[used++] = 2; packet[used++] = 7;
    check(parse(&ip) == 0 && ip == IP4(192, 0, 2, 7), "compressed_cname_resolves");
    begin("www.example.test", 2); address("alias.example.test", 1);
    off = rr(NULL, 5, 1, 0); name("alias.example.test"); word(off - 2, used - off);
    check(parse(&ip) == 0, "cname_answers_out_of_order");
    begin("other.example.test", 1); address(NULL, 1);
    check(parse(&ip) == 1, "wrong_question_ignored");
    begin("www.example.test", 1); address(NULL, 1); word(0, 9);
    check(parse(&ip) == 1, "wrong_id_ignored");
    begin("www.example.test", 0); packet[12] = 0xc0; packet[13] = 12;
    check(parse(&ip) == -EIO, "pointer_loop_rejected");
    begin("www.example.test", 1); packet[used++] = 0xc0;
    check(parse(&ip) == -EIO, "truncated_pointer_rejected");
    begin("www.example.test", 1); address("unrelated.example.test", 1);
    check(parse(&ip) == -ENOENT, "answer_owner_checked");
    begin("www.example.test", 1); address(NULL, 3);
    check(parse(&ip) == -ENOENT, "answer_class_checked");
    begin("www.example.test", 0); word(2, 0x8183);
    check(parse(&ip) == -ENOENT, "nxdomain_reported");
    begin("www.example.test", 1); address(NULL, 1); word(2, 0x8380);
    check(parse(&ip) == -EIO, "truncated_response_rejected");
    begin("www.example.test", 1); address(NULL, 1); used--;
    ip = sentinel;
    check(parse(&ip) == -EIO && ip == sentinel, "output_unchanged_on_failure");
    begin("www.example.test", 1); off = rr(NULL, 5, 1, 2);
    packet[used++] = 0xc0; packet[used++] = 12;
    check(parse(&ip) == -EIO, "cname_cycle_rejected");
    begin("www.example.test", 2); address(NULL, 1);
    packet[used++] = 0xc0;
    check(parse(&ip) == -EIO, "malformed_later_answer_rejected");
    {
        unsigned int links, j;
        char owner[40], target[40];
        for(links = 8; links <= 9; links++) {
            begin("www.example.test", links + 1);
            for(j = 0; j < links; j++) {
                sprintf(owner, "link%u.example.test", j);
                sprintf(target, "link%u.example.test", j + 1);
                off = rr(j ? owner : NULL, 5, 1, 0);
                name(target); word(off - 2, used - off);
            }
            address(target, 1);
            check(parse(&ip) == (links == 8 ? 0 : -EIO),
                  links == 8 ? "eight_cname_links_allowed" : "ninth_cname_link_rejected");
        }
    }
    begin("www.example.test", 1); address(NULL, 1);
    word(used - 6, 3);
    check(parse(&ip) == -EIO, "invalid_a_length_rejected");
    begin("www.example.test", 0); packet[12] = 0xff; packet[13] = 0xff;
    check(parse(&ip) == -EIO, "out_of_packet_pointer_rejected");
    begin("www.example.test", 0); packet[12] = 0x40;
    check(parse(&ip) == -EIO, "reserved_label_form_rejected");
    check(dns_name_valid("good-host.example.") && !dns_name_valid("bad-.example") &&
          !dns_name_valid("white space.example") && !dns_name_valid("a..b"), "hostname_syntax");
    begin("www.example.test", 1); address(NULL, 1); word(10, 1);
    rr("", 41, 4096, 0);
    check(parse(&ip) == 0, "complete_additional_section_accepted");
    used--;
    check(parse(&ip) == -EIO, "truncated_additional_section_rejected");
    begin("www.example.test", 1); address(NULL, 1); word(8, 1);
    check(parse(&ip) == -EIO, "missing_authority_section_rejected");
    begin("www.example.test", 1); address(NULL, 1); end = used;
    for(used = 0; used < end; used++) {
        ip = sentinel;
        check(parse(&ip) != 0 && ip == sentinel, "every_truncation_preserves_output");
    }
}
/* The real resolver and syscall, with only their kernel/UDP environment simulated. */
static struct netdev device;
static surf_task task;
surf_task *curTask = &task;
mutex_t net_lock;
struct udp_socket { int slot; u16 id; };
static struct udp_socket sockets[2];
static int live, closes, received, mode, nested, nesting, send_fail, open_fail;
static int stages[2], route_ok;
static u32 now_ms, budget, lookup_end, sends;
static u16 send_ids[16];
static char *mutate_name;
static u16 nested_id;
void mutex_lock(mutex_t *m) { (void)m; }
void mutex_unlock(mutex_t *m) { (void)m; }
struct netdev *netdev_default(void) { return &device; }
struct netdev *netdev_first(void) { return &device; }
struct netdev *ip_route(ipaddr_t src, ipaddr_t dst) { (void)src; (void)dst; return route_ok ? &device : NULL; }
u_long uptime_ms(void) { return now_ms; }
u_long getticks(void) { return now_ms / 10; }
bool ip_parse(const char *s, ipaddr_t *ip) {
    if(strcmp(s, "192.0.2.1")) return false;
    *ip = IP4(192, 0, 2, 1); return true;
}
bool user_buf(const void *p, u_long len, bool write) {
    (void)len; (void)write;
    return (u_long)p >= 0x1000 && p != (void *)0x100000;
}
struct udp_socket *udp_open(u16 port) {
    (void)port;
    if(open_fail || live == 2) return NULL;
    sockets[live].slot = live;
    return &sockets[live++];
}
void udp_close(struct udp_socket *s) { (void)s; live--; closes++; }
void udp_bind_dev(struct udp_socket *s, struct netdev *d) { (void)s; (void)d; }
int udp_sendto(struct udp_socket *s, ipaddr_t dst, u16 port, const void *data, u_int len) {
    const u8 *p = data;
    (void)dst; (void)port; (void)len;
    s->id = (p[0] << 8) | p[1];
    if(!nesting && sends < 16) send_ids[sends] = s->id;
    if(!nesting) sends++;
    if(mutate_name) { mutate_name[0] = 'X'; mutate_name = NULL; }
    if(nested && !nesting) {
        ipaddr_t ip;
        nesting = 1;
        check(dns_lookup("www.example.test", &ip, 5000) == 0, "nested_lookup_succeeds");
        nested_id = sockets[1].id;
        nesting = 0;
    }
    return send_fail ? -1 : 0;
}
int udp_recvfrom(struct udp_socket *s, void *buf, u_int size, ipaddr_t *src, u16 *port, u_long remaining) {
    int stage = stages[s->slot]++;
    (void)size;
    check(remaining > 0 && remaining <= (u32)(lookup_end - now_ms), "receive_waits_within_the_budget");
    received++;
    if(mode == 3) { task.intr = TI_INT; return -1; }
    if(mode == 4 || (mode == 6 && stage == 0)) { now_ms += remaining; return -1; }   /* silence; 6: the first answer is lost */
    now_ms += mode == 1 ? 250 : 10;
    begin(mode == 2 && stage == 2 ? "other.example.test" : "www.example.test", 1);
    address(NULL, 1); word(0, s->id);
    if(mode == 1) word(0, s->id + 1);
    if(mode == 5) word(2, 0x8183);
    if(src) *src = mode == 2 && stage == 0 ? IP4(192, 0, 2, 9) : device.dns;
    if(port) *port = mode == 2 && stage == 1 ? 54 : 53;
    memcpy(buf, packet, used); return used;
}
#include "../../net/dns.c"
#include "../../kernel/websvc.c"

static void reset(int test_mode) {
    memset(&device, 0, sizeof(device)); device.ip = IP4(10, 0, 2, 15);
    device.dns = IP4(10, 0, 2, 3); device.netmask = IP4(255, 255, 255, 0); device.link_up = true;
    task.intr = 0; now_ms = 0; mode = test_mode; route_ok = 1; sends = 0;
    live = closes = received = nested = nesting = send_fail = open_fail = 0;
    memset(stages, 0, sizeof(stages)); budget = 5000; lookup_end = budget;
    mutate_name = NULL;
}
static void runtime(void) {
    ipaddr_t ip, sentinel = 0x12345678;
    char host[] = "www.example.test", longhost[255];
    reset(0);
    check(dns_lookup(host, &ip, budget) == 0 && ip == IP4(192, 0, 2, 7), "lookup_success");
    check(live == 0 && closes == 1, "success_closes_socket");
    reset(1); now_ms = 0xfffffff0; lookup_end = now_ms + budget;
    check(dns_lookup(host, &ip, budget) == -ETIMEDOUT && now_ms == (u32)(0xfffffff0 + budget),
          "unrelated_flood_deadline_across_clock_wrap");
    check(received == 20 && live == 0, "flood_closes_socket");
    check(sends == 3 && send_ids[0] == send_ids[1] && send_ids[1] == send_ids[2], "flood_query_resent_after_1_and_3_seconds");
    reset(2);
    check(dns_lookup(host, &ip, budget) == 0 && received == 4, "source_port_and_question_filtered");
    reset(3); ip = sentinel;
    check(dns_lookup(host, &ip, budget) == -EINTR && ip == sentinel && live == 0, "interruption_preserves_output_and_closes");
    reset(4);
    check(dns_lookup(host, &ip, budget) == -ETIMEDOUT && live == 0 && now_ms == budget, "silence_times_out");
    check(sends == 3, "silence_query_sent_three_times");
    reset(6); ip = sentinel;
    check(dns_lookup(host, &ip, budget) == 0 && ip == IP4(192, 0, 2, 7) && now_ms == 1000 + 10 && live == 0,
          "lost_answer_retried_after_one_second");
    check(sends == 2 && send_ids[0] == send_ids[1], "retry_keeps_the_query_id");
    reset(4); budget = 20000; lookup_end = budget;
    check(dns_lookup(host, &ip, budget) == -ETIMEDOUT && now_ms == budget && sends == 7,
          "retry_gaps_double_to_four_seconds");
    reset(4); budget = 1000; lookup_end = budget;
    check(dns_lookup(host, &ip, budget) == -ETIMEDOUT && now_ms == budget && sends == 1,
          "short_budget_sends_once");
    reset(5); ip = sentinel;
    check(dns_lookup(host, &ip, budget) == -ENOENT && ip == sentinel && live == 0, "nxdomain_runtime");
    reset(0); device.dns = 0;
    check(dns_lookup(host, &ip, budget) == -ENETUNREACH, "missing_dns_route");
    reset(0); route_ok = 0;
    check(dns_lookup(host, &ip, budget) == -ENETUNREACH, "unusable_dns_route");
    reset(0); device.dns = IP4(8, 8, 8, 8);
    check(dns_lookup(host, &ip, budget) == -ENETUNREACH && received == 0, "off_subnet_dns_without_gateway");
    reset(0); device.dns = IP4(8, 8, 8, 8); device.gateway = IP4(10, 0, 2, 2);
    check(dns_lookup(host, &ip, budget) == 0, "off_subnet_dns_with_gateway");
    reset(0); device.link_up = false;
    check(dns_lookup(host, &ip, budget) == -ENETUNREACH, "down_dns_route");
    reset(0); task.intr = TI_INT;
    check(dns_lookup(host, &ip, budget) == -EINTR && live == 0, "already_interrupted");
    reset(0);
    check(dns_lookup(host, &ip, 0) == -ETIMEDOUT && live == 0, "zero_deadline");
    reset(0); nested = 1;
    check(dns_lookup(host, &ip, budget) == 0 && sockets[0].id != nested_id && live == 0,
          "concurrent_query_ids_differ");
    reset(5);
    check(dns_resolve(host, &ip, budget) == -1, "legacy_failure_convention");
    reset(0); send_fail = 1;
    check(dns_lookup(host, &ip, budget) < 0 && live == 0, "send_failure_closes_socket");
    reset(0); open_fail = 1;
    check(dns_lookup(host, &ip, budget) < 0, "socket_exhaustion_reported");
    reset(0);
    check(sys_resolve("192.0.2.1", &ip) == 0 && ip == IP4(192, 0, 2, 1) && received == 0,
          "syscall_dotted_quad_no_network");
    check(sys_resolve((void *)0x100000, &ip) == -EFAULT, "syscall_bad_hostname_pointer");
    check(sys_resolve(host, (void *)0x100000) == -EFAULT, "syscall_bad_output_pointer");
    check(sys_resolve("", &ip) == -EINVAL && sys_resolve("-bad.test", &ip) == -EINVAL &&
          sys_resolve("a..b", &ip) == -EINVAL, "syscall_invalid_hostnames");
    memset(longhost, 'a', sizeof(longhost)); longhost[254] = 0;
    check(sys_resolve(longhost, &ip) == -EINVAL, "syscall_overlong_hostname");
    reset(0); mutate_name = host;
    check(sys_resolve(host, &ip) == 0 && host[0] == 'X', "syscall_copies_before_waiting");
    host[0] = 'w'; reset(4); ip = sentinel;
    check(sys_resolve(host, &ip) == -ETIMEDOUT && ip == sentinel, "syscall_failure_preserves_output");
}
int main(void) {
    packets();
    runtime();
    printf("DNS: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
