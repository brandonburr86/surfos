/* Host-side unit tests for driver/pciinfo.c (10/7/26): lspci -v's and wifiinfo's reading
   of a PCI function's 256 bytes of configuration space. The configuration spaces are
   built by hand: one shaped like the Ice Lake chipset's CNVi Wi-Fi with an AX201 module
   (the IdeaPad's likely card: power management, MSI, PCI Express as an integrated
   endpoint, MSI-X), one like a Realtek RTL8822CE behind a root port (with a link), a
   root port, and broken capability lists (a loop, a pointer below 40, a capability cut
   off at the end). Built like the kernel objects and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <surfos/types.h>
#include <sys/pciinfo.h>

void exit(int status);

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

/**** the lines, collected ****/

static char lines[64][160];
static int nlines;

static void collect(void *ctx, const char *line) {
    (void)ctx;
    if(nlines < 64) {
        strncpy(lines[nlines], line, sizeof(lines[0]) - 1);
        lines[nlines][sizeof(lines[0]) - 1] = 0;
    }
    nlines++;
}

static void details(const u8 *cfg) {
    nlines = 0;
    pciinfo_details(cfg, collect, NULL);
}

/* a line that has text in it */
static int has(const char *text) {
    int i;
    for(i = 0; i < nlines && i < 64; i++) if(strstr(lines[i], text)) return 1;
    return 0;
}

static void expect(const char *text) {
    char what[200];
    snprintf(what, sizeof(what), "a line with \"%s\"", text);
    check(has(text), what);
    if(!has(text)) {
        int i;
        for(i = 0; i < nlines && i < 64; i++) printf("    | %s\n", lines[i]);
    }
}

/**** configuration spaces ****/

static u8 cfg[PCI_CONFIG_SIZE];

static void put16(u_int off, u16 v) { cfg[off] = (u8)v; cfg[off + 1] = (u8)(v >> 8); }
static void put32(u_int off, u32 v) { put16(off, (u16)v); put16(off + 2, (u16)(v >> 16)); }

static void header(u16 vendor, u16 device, u8 base, u8 sub, u8 prog, u8 rev, u8 type) {
    memset(cfg, 0, sizeof(cfg));
    put16(0x00, vendor);
    put16(0x02, device);
    cfg[0x08] = rev;
    cfg[0x09] = prog;
    cfg[0x0a] = sub;
    cfg[0x0b] = base;
    cfg[0x0e] = type;
}

static void cap(u_int off, u8 id, u8 next) { cfg[off] = id; cfg[off + 1] = next; }

static void caps_at(u8 first) {
    put16(0x06, 0x0010);                                /* status: a capability list */
    cfg[0x34] = first;
}

/* the Ice Lake chipset's CNVi Wi-Fi, with an AX201 module (subsystem 8086:0070) */
static void ax201(void) {
    header(0x8086, 0x34f0, 0x02, 0x80, 0x00, 0x30, 0x00);
    put16(0x04, 0x0406);                                /* memory, bus master, INTx off */
    put32(0x10, 0xc4000004);                            /* a 64-bit memory BAR */
    put16(0x2c, 0x8086);
    put16(0x2e, 0x0070);
    cfg[0x3c] = 0xff;
    cfg[0x3d] = 1;
    caps_at(0xc8);
    cap(0xc8, 0x01, 0xd0);                              /* power management */
    put16(0xca, 0xc823);                                /* PME from D0, D3hot, D3cold */
    put16(0xcc, 0x0008);                                /* in D0 */
    cap(0xd0, 0x05, 0x40);                              /* MSI */
    put16(0xd2, 0x0080);                                /* 64-bit, 1 vector, off */
    cap(0x40, 0x10, 0x80);                              /* PCI Express */
    put16(0x42, 0x0092);                                /* version 2, a root complex integrated endpoint */
    cap(0x80, 0x11, 0x00);                              /* MSI-X */
    put16(0x82, 0x000f);                                /* 16 vectors, off */
}

static void test_ax201(void) {
    ax201();
    details(cfg);
    expect("        ids 8086:34f0, subsystem 8086:0070, class 028000, revision 30");
    expect("command: memory, bus master, INTx off; status: capability list");
    expect("interrupt: pin A, line 255");
    expect("        capabilities:");
    expect("        at c8: power management, now in D0; wake-up (PME) from D0, D3hot, D3cold");
    expect("        at d0: MSI, 64-bit, up to 1 vector, off");
    expect("        at 40: PCI Express 2, root complex integrated endpoint");
    check(!has("link"), "an integrated endpoint has no link line");
    expect("        at 80: MSI-X, 16 vectors, off");
    expect("(extended capabilities, from 100, are not read");
    check(!has("ends here"), "a sound list ends at its end");
    check(pciinfo_is_wireless(cfg), "the CNVi Wi-Fi is wireless");
    check(pciinfo_chip(0x8086, 0x34f0) && strstr(pciinfo_chip(0x8086, 0x34f0), "AX201") &&
          strstr(pciinfo_chip(0x8086, 0x34f0), "9560"), "8086:34f0: an AX201 or a 9560 module");
}

/* a Realtek RTL8822CE behind a root port: an endpoint with a link at 2.5 GT/s of 5 */
static void test_rtl8822ce(void) {
    header(0x10ec, 0xc822, 0x02, 0x80, 0x00, 0x00, 0x00);
    put16(0x04, 0x0006);
    put16(0x2c, 0x17aa);
    put16(0x2e, 0xc123);
    cfg[0x3d] = 1;
    cfg[0x3c] = 11;
    caps_at(0x40);
    cap(0x40, 0x01, 0x50);
    put16(0x42, 0x0003);                                /* no PME */
    put16(0x44, 0x0003);                                /* in D3hot */
    cap(0x50, 0x05, 0x70);
    put16(0x52, 0x0001);                                /* 32-bit, 1 vector, on */
    cap(0x70, 0x10, 0x00);
    put16(0x72, 0x0002);                                /* version 2, an endpoint */
    put32(0x7c, 0x00000012);                            /* link: up to 5 GT/s x1 */
    put16(0x82, 0x1011);                                /* now 2.5 GT/s x1 */
    details(cfg);
    expect("        ids 10ec:c822, subsystem 17aa:c123, class 028000, revision 00");
    expect("interrupt: pin A, line 11");
    expect("        at 40: power management, now in D3hot; no wake-up (PME)");
    expect("        at 50: MSI, 32-bit, up to 1 vector, on");
    expect("        at 70: PCI Express 2, endpoint; link 2.5 GT/s x1 (up to 5 GT/s x1)");
    check(pciinfo_is_wireless(cfg), "the RTL8822CE is wireless");
    check(pciinfo_chip(0x10ec, 0xc822) && strstr(pciinfo_chip(0x10ec, 0xc822), "RTL8822CE"), "10ec:c822: RTL8822CE");
    put16(0x82, 0x0001);                                /* no width: the link is down */
    details(cfg);
    expect("        at 70: PCI Express 2, endpoint; link down (up to 5 GT/s x1)");
}

/* a root port: a bridge, its buses, a link, and its subsystem in a capability */
static void test_root_port(void) {
    header(0x8086, 0x34b0, 0x06, 0x04, 0x00, 0x30, 0x01);
    put16(0x04, 0x0007);
    cfg[0x18] = 0x00;
    cfg[0x19] = 0x02;
    cfg[0x1a] = 0x02;
    caps_at(0x40);
    cap(0x40, 0x10, 0x80);
    put16(0x42, 0x0042);                                /* version 2, a root port */
    put32(0x4c, 0x00000043);                            /* up to 8 GT/s x4 */
    put16(0x52, 0x0013);                                /* now 8 GT/s x1 */
    cap(0x80, 0x0d, 0x90);
    put16(0x84, 0x17aa);
    put16(0x86, 0x3801);
    cap(0x90, 0x09, 0x00);
    cfg[0x92] = 0x14;
    details(cfg);
    expect("        ids 8086:34b0, class 060400, revision 30, a PCI-to-PCI bridge");
    expect("command: I/O, memory, bus master; status: capability list");
    expect("interrupt: no pin");
    expect("        buses: primary 00, secondary 02, subordinate 02");
    expect("        at 40: PCI Express 2, root port; link 8 GT/s x1 (up to 8 GT/s x4)");
    expect("        at 80: subsystem 17aa:3801");
    expect("        at 90: vendor-specific, 20 bytes");
    check(!pciinfo_is_wireless(cfg), "a root port is not wireless");
}

/* lists that are broken in every way that could loop or read past the end */
static void test_broken_lists(void) {
    header(0x1234, 0x5678, 0x02, 0x00, 0x00, 0x01, 0x00);
    caps_at(0x40);
    cap(0x40, 0x01, 0x50);
    cap(0x50, 0x05, 0x40);                              /* back to the first */
    details(cfg);
    expect("        at 40: power management");
    expect("        at 50: MSI");
    expect("(the list comes back to 40: ends here)");
    check(nlines < 12, "a loop ends");

    header(0x1234, 0x5678, 0x02, 0x00, 0x00, 0x01, 0x00);
    caps_at(0x20);                                      /* inside the header */
    details(cfg);
    expect("(the list goes on at 20, which is no capability's place: ends here)");
    check(!has("(none"), "a broken list is not called empty");

    header(0x1234, 0x5678, 0x02, 0x00, 0x00, 0x01, 0x00);
    caps_at(0x40);
    cap(0x40, 0x09, 0xfe);                              /* not dword-aligned: the low bits are reserved, read as fc */
    cfg[0x42] = 4;
    cap(0xfc, 0x01, 0x00);                              /* power management needs 6 bytes, 4 are left */
    details(cfg);
    expect("        at fc: capability 01, cut off by the end of the 256 bytes");

    header(0x1234, 0x5678, 0x02, 0x00, 0x00, 0x01, 0x00);
    cfg[0x34] = 0x40;                                   /* a pointer, but the status says no list */
    details(cfg);
    check(!has("capabilities"), "no capability list when the status says so");
    expect("status: no capability list");

    header(0x1234, 0x5678, 0x02, 0x00, 0x00, 0x01, 0x00);
    caps_at(0x00);
    details(cfg);
    expect("(none: the list is empty)");
}

static void test_wireless(void) {
    header(0x8086, 0x100e, 0x02, 0x00, 0x00, 0x03, 0x00);  /* QEMU's e1000: Ethernet */
    check(!pciinfo_is_wireless(cfg), "an Ethernet controller is not wireless");
    check(pciinfo_chip(0x8086, 0x100e) == NULL, "8086:100e is no Wi-Fi chip");
    header(0x1234, 0x0001, 0x0d, 0x80, 0x00, 0x00, 0x00);
    check(pciinfo_is_wireless(cfg), "class 0d (wireless) is wireless");
    header(0x8086, 0x2723, 0x02, 0x00, 0x00, 0x1a, 0x00);  /* an AX200 that calls itself Ethernet */
    check(pciinfo_is_wireless(cfg), "a known chip is wireless whatever its class");
    header(0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0x7f);
    check(!pciinfo_is_wireless(cfg), "nothing there is not wireless");
}

static void test_bluetooth(void) {
    const char *s;
    s = pciinfo_bluetooth(0x8087, 0x0026, 0xe0);
    check(s && strstr(s, "AX201"), "8087:0026: an AX201");
    s = pciinfo_bluetooth(0x8087, 0x0aaa, 0xe0);
    check(s && strstr(s, "9560"), "8087:0aaa: a 9560");
    s = pciinfo_bluetooth(0x0bda, 0xc822, 0xe0);
    check(s && strstr(s, "RTL8822CE"), "0bda:c822: an RTL8822CE");
    s = pciinfo_bluetooth(0x8087, 0x9999, 0xe0);
    check(s && strstr(s, "Intel"), "another Intel Bluetooth: Intel");
    s = pciinfo_bluetooth(0x13d3, 0x3548, 0xe0);
    check(s && strstr(s, "Bluetooth half of the Wi-Fi card"), "any Bluetooth class device");
    check(pciinfo_bluetooth(0x0525, 0xa4a2, 0x02) == NULL, "a USB network adapter says nothing about Wi-Fi");
    check(pciinfo_bluetooth(0x46f4, 0x0001, 0x08) == NULL, "a USB stick says nothing about Wi-Fi");
}

static void test_dump(void) {
    ax201();
    nlines = 0;
    pciinfo_dump(cfg, collect, NULL);
    check(nlines == 16, "16 lines of 16 bytes");
    check(!strcmp(lines[0], "        00: 86 80 f0 34 06 04 10 00 30 00 80 02 00 00 00 00"), "the first line");
    check(!strcmp(lines[2], "        20: 00 00 00 00 00 00 00 00 00 00 00 00 86 80 70 00"), "the subsystem's line");
    check(!strncmp(lines[15], "        f0:", 11), "the last line is f0");
}

int main(void) {
    test_ax201();
    test_rtl8822ce();
    test_root_port();
    test_broken_lists();
    test_wireless();
    test_bluetooth();
    test_dump();
    printf("PCIINFO UNIT TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    exit(failures ? 1 : 0);
    return 0;
}
