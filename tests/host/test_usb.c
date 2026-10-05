/* Host-side unit tests for driver/usb/usbdesc.c: finding the CDC Ethernet (ECM) or NCM
   function in a configuration descriptor, the MAC address string, and NCM's 16-bit
   transfer blocks in both directions. The descriptors are built by hand: QEMU's usb-net
   (copied from its source, hw/usb/dev-network.c), one shaped like the CDC Ethernet
   configuration of an RTL8153 adapter (SuperSpeed, with endpoint companions), and an
   NCM one. QEMU has no NCM device, so this is where NCM gets exercised before hardware.
   Built like the kernel objects and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <surfos/types.h>
#include <sys/usb.h>

void exit(int status);

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

/**** configuration descriptors ****/

static u8 cfg[1024];
static u_int used;

static void add(const u8 *d, u_int n) { memcpy(cfg + used, d, n); used += n; }
#define ADD(...) do { const u8 _d[] = { __VA_ARGS__ }; add(_d, sizeof(_d)); } while(0)

static void begin(u8 value, u8 ninterfaces) {
    memset(cfg, 0, sizeof(cfg));
    used = 0;
    ADD(9, USB_DT_CONFIG, 0, 0, ninterfaces, value, 0, 0xc0, 50);
}

static u_int end(void) {
    cfg[2] = (u8)used;
    cfg[3] = (u8)(used >> 8);
    return used;
}

/* QEMU usb-net, configuration 1 (CDC Ethernet) */
static u_int qemu_cdc(void) {
    begin(1, 2);
    ADD(9, USB_DT_INTERFACE, 0, 0, 1, 0x02, 0x06, 0x00, 5);          /* communications, ECM */
    ADD(5, 0x24, 0x00, 0x10, 0x01);                                 /* header, CDC 1.10 */
    ADD(5, 0x24, 0x06, 0x00, 0x01);                                 /* union: 0 controls 1 */
    ADD(13, 0x24, 0x0f, 3, 0, 0, 0, 0, 0xea, 0x05, 0, 0, 0);        /* Ethernet: MAC string 3, 1514 */
    ADD(7, USB_DT_ENDPOINT, 0x81, 0x03, 16, 0, 32);                 /* notifications */
    ADD(9, USB_DT_INTERFACE, 1, 0, 0, 0x0a, 0, 0, 0);               /* data, off */
    ADD(9, USB_DT_INTERFACE, 1, 1, 2, 0x0a, 0, 0, 4);               /* data, on */
    ADD(7, USB_DT_ENDPOINT, 0x82, 0x02, 64, 0, 0);
    ADD(7, USB_DT_ENDPOINT, 0x02, 0x02, 64, 0, 0);
    return end();
}

/* QEMU usb-net, configuration 2 (RNDIS: CDC ACM with a vendor protocol) */
static u_int qemu_rndis(void) {
    begin(2, 2);
    ADD(9, USB_DT_INTERFACE, 0, 0, 1, 0x02, 0x02, 0xff, 6);
    ADD(5, 0x24, 0x00, 0x10, 0x01);
    ADD(5, 0x24, 0x01, 0x00, 0x01);
    ADD(4, 0x24, 0x02, 0x00);
    ADD(5, 0x24, 0x06, 0x00, 0x01);
    ADD(7, USB_DT_ENDPOINT, 0x81, 0x03, 8, 0, 32);
    ADD(9, USB_DT_INTERFACE, 1, 0, 2, 0x0a, 0, 0, 4);
    ADD(7, USB_DT_ENDPOINT, 0x82, 0x02, 64, 0, 0);
    ADD(7, USB_DT_ENDPOINT, 0x02, 0x02, 64, 0, 0);
    return end();
}

/* shaped like an RTL8153's second configuration: CDC Ethernet at SuperSpeed */
static u_int rtl8153_ecm(void) {
    begin(2, 2);
    ADD(9, USB_DT_INTERFACE, 0, 0, 1, 0x02, 0x06, 0x00, 5);
    ADD(5, 0x24, 0x00, 0x10, 0x01);
    ADD(5, 0x24, 0x06, 0x00, 0x01);
    ADD(13, 0x24, 0x0f, 3, 0, 0, 0, 0, 0xea, 0x05, 0, 0, 0);
    ADD(7, USB_DT_ENDPOINT, 0x83, 0x03, 16, 0, 8);
    ADD(6, USB_DT_SS_EP_COMP, 0, 0, 16, 0);
    ADD(9, USB_DT_INTERFACE, 1, 0, 0, 0x0a, 0, 0, 0);
    ADD(9, USB_DT_INTERFACE, 1, 1, 2, 0x0a, 0, 0, 4);
    ADD(7, USB_DT_ENDPOINT, 0x81, 0x02, 0x00, 0x04, 0);             /* 1024-byte packets */
    ADD(6, USB_DT_SS_EP_COMP, 3, 0, 0, 0);                          /* bursts of 4 */
    ADD(7, USB_DT_ENDPOINT, 0x02, 0x02, 0x00, 0x04, 0);
    ADD(6, USB_DT_SS_EP_COMP, 3, 0, 0, 0);
    return end();
}

/* an NCM function: data interface protocol 1, an NCM functional descriptor */
static u_int ncm(void) {
    begin(2, 2);
    ADD(9, USB_DT_INTERFACE, 0, 0, 1, 0x02, 0x0d, 0x00, 5);
    ADD(5, 0x24, 0x00, 0x10, 0x01);
    ADD(5, 0x24, 0x06, 0x00, 0x01);
    ADD(13, 0x24, 0x0f, 3, 0, 0, 0, 0, 0xea, 0x05, 0, 0, 0);
    ADD(6, 0x24, 0x1a, 0x00, 0x01, 0x21);                           /* NCM 1.0, capabilities 0x21 */
    ADD(7, USB_DT_ENDPOINT, 0x83, 0x03, 16, 0, 8);
    ADD(9, USB_DT_INTERFACE, 1, 0, 0, 0x0a, 0, 1, 0);
    ADD(9, USB_DT_INTERFACE, 1, 1, 2, 0x0a, 0, 1, 4);
    ADD(7, USB_DT_ENDPOINT, 0x81, 0x02, 0x00, 0x02, 0);
    ADD(7, USB_DT_ENDPOINT, 0x02, 0x02, 0x00, 0x02, 0);
    return end();
}

static void test_functions(void) {
    struct cdc_net_function f;
    u_int len;

    len = qemu_cdc();
    check(cdc_find_net_function(cfg, len, &f) == 0, "QEMU usb-net CDC configuration: found");
    check(f.kind == CDC_ECM, "... as ECM");
    check(f.comm_iface == 0 && f.data_iface == 1 && f.data_alt == 1, "... interfaces 0 and 1, data in alternate setting 1");
    check(f.mac_string == 3 && f.max_segment == 1514, "... MAC string 3, 1514-byte segments");
    check(f.notify && f.notify->bEndpointAddress == 0x81 && f.notify->wMaxPacketSize == 16 && f.notify->bInterval == 32,
          "... notifications on 0x81");
    check(f.in && f.in->bEndpointAddress == 0x82 && f.in->wMaxPacketSize == 64, "... bulk IN 0x82, 64-byte packets");
    check(f.out && f.out->bEndpointAddress == 0x02, "... bulk OUT 0x02");
    check(!f.in_comp && !f.out_comp && !f.notify_comp, "... no SuperSpeed companions");
    check(usb_config_class(cfg, len) == USB_CLASS_COMM, "its first interface is communications");
    check(usb_find_interface(cfg, len, 1, 1) != NULL && usb_find_interface(cfg, len, 1, 2) == NULL, "usb_find_interface");

    len = qemu_rndis();
    check(cdc_find_net_function(cfg, len, &f) == -1, "QEMU usb-net RNDIS configuration: not ECM or NCM");

    len = rtl8153_ecm();
    check(cdc_find_net_function(cfg, len, &f) == 0 && f.kind == CDC_ECM, "RTL8153-style configuration: ECM");
    check(f.in && f.in->bEndpointAddress == 0x81 && f.in->wMaxPacketSize == 1024, "... bulk IN 0x81, 1024-byte packets");
    check(f.in_comp && f.in_comp->bMaxBurst == 3 && f.out_comp && f.out_comp->bMaxBurst == 3, "... companions: bursts of 4");
    check(f.notify && f.notify->bEndpointAddress == 0x83 && f.notify_comp != NULL, "... notifications on 0x83 with a companion");

    len = ncm();
    check(cdc_find_net_function(cfg, len, &f) == 0 && f.kind == CDC_NCM, "NCM configuration: NCM");
    check(f.ncm_caps == 0x21 && f.data_alt == 1 && f.in && f.out, "... capabilities, data interface");

    /* both in one configuration: ECM wins */
    begin(1, 4);
    ADD(9, USB_DT_INTERFACE, 0, 0, 0, 0x02, 0x0d, 0x00, 0);
    ADD(5, 0x24, 0x06, 0x00, 0x01);
    ADD(9, USB_DT_INTERFACE, 1, 1, 2, 0x0a, 0, 1, 0);
    ADD(7, USB_DT_ENDPOINT, 0x81, 0x02, 0x00, 0x02, 0);
    ADD(7, USB_DT_ENDPOINT, 0x01, 0x02, 0x00, 0x02, 0);
    ADD(9, USB_DT_INTERFACE, 2, 0, 0, 0x02, 0x06, 0x00, 0);
    ADD(5, 0x24, 0x06, 0x02, 0x03);
    ADD(9, USB_DT_INTERFACE, 3, 1, 2, 0x0a, 0, 0, 0);
    ADD(7, USB_DT_ENDPOINT, 0x82, 0x02, 0x00, 0x02, 0);
    ADD(7, USB_DT_ENDPOINT, 0x02, 0x02, 0x00, 0x02, 0);
    len = end();
    check(cdc_find_net_function(cfg, len, &f) == 0 && f.kind == CDC_ECM && f.comm_iface == 2 && f.data_iface == 3,
          "ECM is preferred over NCM in the same configuration");
    check(f.mac_string == 0 && f.max_segment == 1514 && !f.notify, "... no MAC string, no notifications: defaults");

    /* a data interface without both bulk endpoints is no function */
    begin(1, 2);
    ADD(9, USB_DT_INTERFACE, 0, 0, 0, 0x02, 0x06, 0x00, 0);
    ADD(9, USB_DT_INTERFACE, 1, 1, 1, 0x0a, 0, 0, 0);
    ADD(7, USB_DT_ENDPOINT, 0x81, 0x02, 0x00, 0x02, 0);
    len = end();
    check(cdc_find_net_function(cfg, len, &f) == -1, "a data interface with one endpoint: none");

    /* broken descriptors end the walk */
    len = qemu_cdc();
    cfg[9 + 9] = 0;                                                 /* the header's bLength */
    check(cdc_find_net_function(cfg, len, &f) == -1, "a zero bLength stops the walk");
    len = qemu_cdc();
    check(cdc_find_net_function(cfg, len - 3, &f) == -1, "a descriptor running past the end is not used");
    check(usb_desc_next(cfg, 9, NULL) == cfg && usb_desc_next(cfg, 9, cfg) == NULL, "usb_desc_next at the end");
    check(cdc_find_net_function(cfg, 8, &f) == -1, "shorter than a configuration descriptor");
    cfg[1] = USB_DT_DEVICE;
    check(cdc_find_net_function(cfg, len, &f) == -1, "not a configuration descriptor");
}

static void test_mac(void) {
    u8 m[6];
    check(cdc_parse_mac("400102030405", m) && m[0] == 0x40 && m[5] == 0x05, "MAC 400102030405");
    check(cdc_parse_mac("00E04C680001", m) && m[1] == 0xe0 && m[2] == 0x4c, "MAC in capitals");
    check(cdc_parse_mac("5254001234ab", m) && m[5] == 0xab, "MAC in lower case");
    check(!cdc_parse_mac("12345", m), "too short");
    check(!cdc_parse_mac("4001020304050", m), "too long");
    check(!cdc_parse_mac("40010203040g", m), "not hex");
    check(!cdc_parse_mac("010203040506", m), "a group address");
    check(!cdc_parse_mac("000000000000", m), "all zeros");
}

/**** NCM blocks ****/

static u16 get16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 get32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, (u16)v); put16(p + 2, (u16)(v >> 16)); }

struct seen {
    int n;
    u_int len[8];
    const u8 *at[8];
};

static void collect(void *arg, const u8 *frame, u_int len) {
    struct seen *s = (struct seen *)arg;
    if(s->n < 8) { s->len[s->n] = len; s->at[s->n] = frame; }
    s->n++;
}

static void test_ntb_build(void) {
    static u8 frame[1514], out[4096];
    struct ncm_out_params p = { 16384, 4, 4, 0, 512 };
    struct seen s;
    u_int n, i;
    for(i = 0; i < sizeof(frame); i++) frame[i] = (u8)(i * 7 + 1);

    n = ncm_build_ntb16(out, sizeof(out), 7, frame, 60, &p);
    check(n == 28 + 60, "NTB: header 12, NDP 16, then the frame");
    check(get32(out) == 0x484d434e && get16(out + 4) == 12 && get16(out + 6) == 7 && get16(out + 8) == n &&
          get16(out + 10) == 12, "NTH16: signature, length, sequence, block length, NDP index");
    check(get32(out + 12) == 0x304d434e && get16(out + 16) == 16 && get16(out + 18) == 0, "NDP16: NCM0, 16 bytes, last");
    check(get16(out + 20) == 28 && get16(out + 22) == 60 && get32(out + 24) == 0, "NDP16: the datagram, then the terminator");
    check(!memcmp(out + 28, frame, 60), "the frame is copied");
    memset(&s, 0, sizeof(s));
    check(ncm_parse_ntb16(out, n, collect, &s) == 1 && s.len[0] == 60 && !memcmp(s.at[0], frame, 60), "a built block parses back");

    p.div = 8; p.rem = 2;
    n = ncm_build_ntb16(out, sizeof(out), 0, frame, 100, &p);
    check(get16(out + 20) % 8 == 2 && get16(out + 20) >= 28 && n == get16(out + 20) + 100u, "divisor 8, remainder 2");
    p.div = 4; p.rem = 0; p.align = 16;
    n = ncm_build_ntb16(out, sizeof(out), 0, frame, 100, &p);
    check(get16(out + 10) == 16 && get16(out + 24) == 32, "NDP aligned to 16");
    p.align = 4;

    n = ncm_build_ntb16(out, sizeof(out), 0, frame, 512 - 28, &p);
    check(n == 513 && get16(out + 8) == 513 && out[512] == 0, "a block that would fill its last packet gets a byte more");
    memset(&s, 0, sizeof(s));
    check(ncm_parse_ntb16(out, n, collect, &s) == 1 && s.len[0] == 512 - 28, "... and still holds the frame");
    n = ncm_build_ntb16(out, sizeof(out), 0, frame, 1514, &p);
    check(n == 28 + 1514, "a full-size frame");
    check(ncm_build_ntb16(out, 100, 0, frame, 1514, &p) == 0, "does not fit the buffer: 0");
    p.max = 1024;
    check(ncm_build_ntb16(out, sizeof(out), 0, frame, 1514, &p) == 0, "larger than dwNtbOutMaxSize: 0");
    p.max = 16384;
    check(ncm_build_ntb16(out, sizeof(out), 0, frame, 0, &p) == 0, "an empty frame: 0");
}

/* two NDPs: the first with two datagrams, the second (after the data) with one */
static u_int two_ndp_block(u8 *b) {
    u_int i;
    memset(b, 0, 600);
    put32(b, 0x484d434e); put16(b + 4, 12); put16(b + 6, 1); put16(b + 8, 600); put16(b + 10, 12);
    put32(b + 12, 0x304d434e); put16(b + 16, 20); put16(b + 18, 500);       /* NDP: 2 entries + terminator */
    put16(b + 20, 40); put16(b + 22, 60);
    put16(b + 24, 100); put16(b + 26, 80);
    for(i = 40; i < 180; i++) b[i] = (u8)i;
    put32(b + 500, 0x304d434e); put16(b + 504, 16); put16(b + 506, 0);
    put16(b + 508, 200); put16(b + 510, 64);
    return 600;
}

static void test_ntb_parse(void) {
    static u8 b[2048];
    struct seen s;
    u_int len;

    len = two_ndp_block(b);
    memset(&s, 0, sizeof(s));
    check(ncm_parse_ntb16(b, len, collect, &s) == 3, "two NDPs, three datagrams");
    check(s.len[0] == 60 && s.at[0] == b + 40 && s.len[1] == 80 && s.at[1] == b + 100 && s.len[2] == 64 && s.at[2] == b + 200,
          "... in order, where the tables say");

    len = two_ndp_block(b);
    put32(b + 12, 0x314d434e);                                              /* NCM1: each carries a CRC */
    memset(&s, 0, sizeof(s));
    check(ncm_parse_ntb16(b, len, collect, &s) == 3 && s.len[0] == 56 && s.len[1] == 76, "NCM1: the CRC is not passed on");

    len = two_ndp_block(b);
    put32(b, 0x484d434f);
    check(ncm_parse_ntb16(b, len, collect, &s) == -1, "a bad NTH signature: -1");
    len = two_ndp_block(b);
    check(ncm_parse_ntb16(b, 599, collect, &s) == -1, "a block length beyond what arrived: -1");
    check(ncm_parse_ntb16(b, 11, collect, &s) == -1, "shorter than the header: -1");
    len = two_ndp_block(b);
    put16(b + 24, 590);                                                     /* the second datagram runs past the end */
    memset(&s, 0, sizeof(s));
    check(ncm_parse_ntb16(b, len, collect, &s) == 2 && s.len[1] == 64, "a datagram outside the block is skipped");
    len = two_ndp_block(b);
    put16(b + 506, 12);                                                     /* the second NDP points back at the first */
    memset(&s, 0, sizeof(s));
    check(ncm_parse_ntb16(b, len, collect, &s) == 3, "an NDP chain that loops back ends");
    len = two_ndp_block(b);
    put16(b + 10, 598);                                                     /* the NDP index at the very end */
    memset(&s, 0, sizeof(s));
    check(ncm_parse_ntb16(b, len, collect, &s) == 0, "an NDP outside the block: no datagrams");
    len = two_ndp_block(b);
    put16(b + 16, 6);                                                       /* NDP shorter than its header */
    memset(&s, 0, sizeof(s));
    check(ncm_parse_ntb16(b, len, collect, &s) == 0, "a short NDP: no datagrams");
}

int main(void) {
    test_functions();
    test_mac();
    test_ntb_build();
    test_ntb_parse();
    printf("USB UNIT TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    exit(failures ? 1 : 0);
    return 0;
}
