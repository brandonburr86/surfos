/*
SurfOS PCI Configuration Space, Described
--------------------
File: pciinfo.c Date: 10/7/26 (the Wi-Fi report: docs/WIFI-PLANNING.md)
--------------------
lspci -v and wifiinfo: a PCI function's 256 bytes of configuration space as lines of
text (include/sys/pciinfo.h). No I/O and no allocation: tests/host/test_pciinfo.c runs
this on the host against hand-built configuration spaces. The capability list is
followed only while it stays inside the 256 bytes, on dword-aligned offsets from 0x40,
and visits no offset twice, so a broken list ends with a line saying so instead of
looping or reading past the end. Extended capabilities (from 0x100) would need PCI
Express's memory-mapped configuration space, which SurfOS does not map: not read.

The tables of chips name what an id most likely is (from the ids Linux's drivers
match); the ids printed with them are what counts.
*/

#include <surfos/types.h>
#include <sys/pciinfo.h>
#include <blibc_common.h>

#define LINE 160

static u16 rd16(const u8 *c, u_int off) { return (u16)(c[off] | (c[off + 1] << 8)); }
static u32 rd32(const u8 *c, u_int off) { return (u32)rd16(c, off) | ((u32)rd16(c, off + 2) << 16); }

/* "a, b, c" into buf, which holds size bytes */
static void join(char *buf, size_t size, const char *word) {
    if(buf[0]) strlcat(buf, ", ", size);
    strlcat(buf, word, size);
}

/**** the header ****/

static void header_lines(const u8 *cfg, pci_line_fn out, void *ctx) {
    char line[LINE], what[64];
    u8 type = cfg[0x0e] & 0x7f;
    u16 cmd = rd16(cfg, 0x04), status = rd16(cfg, 0x06);

    if(type == 0)
        snprintf(line, sizeof(line), "        ids %04x:%04x, subsystem %04x:%04x, class %02x%02x%02x, revision %02x",
                 rd16(cfg, 0x00), rd16(cfg, 0x02), rd16(cfg, 0x2c), rd16(cfg, 0x2e), cfg[0x0b], cfg[0x0a], cfg[0x09],
                 cfg[0x08]);
    else
        snprintf(line, sizeof(line), "        ids %04x:%04x, class %02x%02x%02x, revision %02x, %s", rd16(cfg, 0x00),
                 rd16(cfg, 0x02), cfg[0x0b], cfg[0x0a], cfg[0x09], cfg[0x08],
                 type == 1 ? "a PCI-to-PCI bridge" : type == 2 ? "a CardBus bridge" : "an unknown header type");
    out(ctx, line);

    what[0] = 0;
    if(cmd & 0x0001) join(what, sizeof(what), "I/O");
    if(cmd & 0x0002) join(what, sizeof(what), "memory");
    if(cmd & 0x0004) join(what, sizeof(what), "bus master");
    if(cmd & 0x0400) join(what, sizeof(what), "INTx off");
    snprintf(line, sizeof(line), "        command: %s; status: %s%s", what[0] ? what : "nothing on",
             (status & 0x0010) ? "capability list" : "no capability list", (status & 0x0008) ? ", interrupt pending" : "");
    out(ctx, line);

    if(cfg[0x3d] >= 1 && cfg[0x3d] <= 4)
        snprintf(line, sizeof(line), "        interrupt: pin %c, line %u", 'A' + cfg[0x3d] - 1, cfg[0x3c]);
    else
        snprintf(line, sizeof(line), "        interrupt: no pin");
    out(ctx, line);

    if(type == 1) {
        snprintf(line, sizeof(line), "        buses: primary %02x, secondary %02x, subordinate %02x", cfg[0x18], cfg[0x19],
                 cfg[0x1a]);
        out(ctx, line);
    }
}

/**** capabilities ****/

static const char *link_speed(u_int code) {
    switch(code) {
    case 1: return "2.5 GT/s";
    case 2: return "5 GT/s";
    case 3: return "8 GT/s";
    case 4: return "16 GT/s";
    case 5: return "32 GT/s";
    default: return "an unknown speed";
    }
}

static const char *port_type(u_int type) {
    switch(type) {
    case 0: return "endpoint";
    case 1: return "legacy endpoint";
    case 4: return "root port";
    case 5: return "switch upstream port";
    case 6: return "switch downstream port";
    case 7: return "PCI Express to PCI bridge";
    case 8: return "PCI to PCI Express bridge";
    case 9: return "root complex integrated endpoint";
    case 10: return "root complex event collector";
    default: return "an unknown port type";
    }
}

static const char *cap_name(u8 id) {
    switch(id) {
    case 0x02: return "AGP";
    case 0x03: return "vital product data";
    case 0x04: return "slot numbering";
    case 0x06: return "hot swap";
    case 0x07: return "PCI-X";
    case 0x08: return "HyperTransport";
    case 0x0a: return "debug port";
    case 0x0c: return "PCI hot-plug";
    case 0x0e: return "AGP 8x";
    case 0x0f: return "secure device";
    case 0x12: return "SATA";
    case 0x13: return "advanced features";
    case 0x14: return "enhanced allocation";
    default: return NULL;
    }
}

/* one capability at off (id, and the bytes after it that stay inside the 256) as a line */
static void cap_line(const u8 *cfg, u_int off, char *line, size_t size, bool *pcie) {
    u8 id = cfg[off];
    char text[LINE];
    text[0] = 0;
    switch(id) {
    case 0x01:                                          /* power management */
        if(off + 6 <= PCI_CONFIG_SIZE) {
            static const char *const state[] = { "D0", "D1", "D2", "D3hot" };
            u16 pmc = rd16(cfg, off + 2), pmcsr = rd16(cfg, off + 4);
            char pme[48];
            pme[0] = 0;
            if(pmc & 0x0800) join(pme, sizeof(pme), "D0");
            if(pmc & 0x1000) join(pme, sizeof(pme), "D1");
            if(pmc & 0x2000) join(pme, sizeof(pme), "D2");
            if(pmc & 0x4000) join(pme, sizeof(pme), "D3hot");
            if(pmc & 0x8000) join(pme, sizeof(pme), "D3cold");
            if(pme[0]) snprintf(text, sizeof(text), "power management, now in %s; wake-up (PME) from %s", state[pmcsr & 3], pme);
            else snprintf(text, sizeof(text), "power management, now in %s; no wake-up (PME)", state[pmcsr & 3]);
        }
        break;
    case 0x05:                                          /* MSI */
        if(off + 4 <= PCI_CONFIG_SIZE) {
            u16 ctl = rd16(cfg, off + 2);
            u_int n = 1u << ((ctl >> 1) & 7);
            snprintf(text, sizeof(text), "MSI, %s, up to %u vector%s%s, %s", (ctl & 0x0080) ? "64-bit" : "32-bit", n,
                     n == 1 ? "" : "s", (ctl & 0x0100) ? ", maskable" : "", (ctl & 1) ? "on" : "off");
        }
        break;
    case 0x09:                                          /* vendor-specific */
        if(off + 3 <= PCI_CONFIG_SIZE) snprintf(text, sizeof(text), "vendor-specific, %u bytes", cfg[off + 2]);
        break;
    case 0x0d:                                          /* a bridge's subsystem ids */
        if(off + 8 <= PCI_CONFIG_SIZE)
            snprintf(text, sizeof(text), "subsystem %04x:%04x", rd16(cfg, off + 4), rd16(cfg, off + 6));
        break;
    case 0x10:                                          /* PCI Express */
        if(off + 4 <= PCI_CONFIG_SIZE) {
            u16 caps = rd16(cfg, off + 2);
            u_int type = (caps >> 4) & 0xf;
            *pcie = true;
            if(type != 9 && type != 10 && off + 0x14 <= PCI_CONFIG_SIZE) {   /* those two have no link */
                u32 lcap = rd32(cfg, off + 0x0c);
                u16 lsta = rd16(cfg, off + 0x12);
                if((lsta >> 4) & 0x3f)
                    snprintf(text, sizeof(text), "PCI Express %u, %s; link %s x%u (up to %s x%u)", caps & 0xf,
                             port_type(type), link_speed(lsta & 0xf), (lsta >> 4) & 0x3f, link_speed(lcap & 0xf),
                             (u_int)((lcap >> 4) & 0x3f));
                else
                    snprintf(text, sizeof(text), "PCI Express %u, %s; link down (up to %s x%u)", caps & 0xf,
                             port_type(type), link_speed(lcap & 0xf), (u_int)((lcap >> 4) & 0x3f));
            } else {
                snprintf(text, sizeof(text), "PCI Express %u, %s", caps & 0xf, port_type(type));
            }
        }
        break;
    case 0x11:                                          /* MSI-X */
        if(off + 4 <= PCI_CONFIG_SIZE) {
            u16 ctl = rd16(cfg, off + 2);
            snprintf(text, sizeof(text), "MSI-X, %u vectors, %s", (ctl & 0x7ff) + 1, (ctl & 0x8000) ? "on" : "off");
        }
        break;
    default:
        if(cap_name(id)) snprintf(text, sizeof(text), "%s", cap_name(id));
        else snprintf(text, sizeof(text), "capability %02x", id);
        break;
    }
    if(!text[0]) snprintf(text, sizeof(text), "capability %02x, cut off by the end of the 256 bytes", id);
    snprintf(line, size, "        at %02x: %s", off, text);
}

static void cap_lines(const u8 *cfg, pci_line_fn out, void *ctx) {
    char line[LINE];
    u8 type = cfg[0x0e] & 0x7f;
    u_int off, n = 0;
    u32 seen[PCI_CONFIG_SIZE / 4 / 32];                 /* one bit per dword */
    bool pcie = false, broken = false;

    if(!(rd16(cfg, 0x06) & 0x0010)) return;
    memset(seen, 0, sizeof(seen));
    off = cfg[type == 2 ? 0x14 : 0x34];
    out(ctx, "        capabilities:");
    while(off) {
        if(off < 0x40 || off > PCI_CONFIG_SIZE - 2 || (off & 3)) {
            snprintf(line, sizeof(line), "        (the list goes on at %02x, which is no capability's place: ends here)", off);
            out(ctx, line);
            broken = true;
            break;
        }
        if(seen[off / 4 / 32] & (1u << ((off / 4) % 32))) {
            snprintf(line, sizeof(line), "        (the list comes back to %02x: ends here)", off);
            out(ctx, line);
            broken = true;
            break;
        }
        seen[off / 4 / 32] |= 1u << ((off / 4) % 32);
        cap_line(cfg, off, line, sizeof(line), &pcie);
        out(ctx, line);
        n++;
        off = cfg[off + 1] & 0xfc;
    }
    if(!n && !broken) out(ctx, "        (none: the list is empty)");
    if(pcie) out(ctx, "        (extended capabilities, from 100, are not read: SurfOS reaches 256 bytes)");
}

void pciinfo_details(const u8 *cfg, pci_line_fn out, void *ctx) {
    header_lines(cfg, out, ctx);
    cap_lines(cfg, out, ctx);
}

void pciinfo_dump(const u8 *cfg, pci_line_fn out, void *ctx) {
    char line[LINE];
    u_int row, i, n;
    for(row = 0; row < PCI_CONFIG_SIZE; row += 16) {
        n = (u_int)snprintf(line, sizeof(line), "        %02x:", row);
        for(i = 0; i < 16; i++) n += (u_int)snprintf(line + n, sizeof(line) - n, " %02x", cfg[row + i]);
        out(ctx, line);
    }
}

/**** what a chip is ****/

static const struct { u16 vendor, device; const char *what; } chips[] = {
    /* Intel CNVi: the Wi-Fi controller is in the chipset, the radio on the M.2 module (Linux: iwlwifi) */
    { 0x8086, 0x34f0, "Intel CNVi Wi-Fi of the Ice Lake chipset, with an AX201 (Wi-Fi 6) or a 9560 (Wi-Fi 5) radio "
                      "module: the subsystem and the Bluetooth half tell which; Linux: iwlwifi" },
    { 0x8086, 0x02f0, "Intel CNVi Wi-Fi of the Comet Lake chipset (an AX201 or 9560 module); Linux: iwlwifi" },
    { 0x8086, 0x06f0, "Intel CNVi Wi-Fi of the Comet Lake-H chipset (an AX201 or 9560 module); Linux: iwlwifi" },
    { 0x8086, 0x9df0, "Intel CNVi Wi-Fi of the Cannon Lake chipset (a 9560-family module); Linux: iwlwifi" },
    { 0x8086, 0xa370, "Intel CNVi Wi-Fi of the Cannon Lake-H chipset (a 9560-family module); Linux: iwlwifi" },
    { 0x8086, 0xa0f0, "Intel CNVi Wi-Fi of the Tiger Lake chipset (an AX201 module); Linux: iwlwifi" },
    /* Intel cards with the whole chip on the card */
    { 0x8086, 0x2723, "Intel Wi-Fi 6 AX200; Linux: iwlwifi" },
    { 0x8086, 0x2725, "Intel Wi-Fi 6E AX210; Linux: iwlwifi" },
    { 0x8086, 0x2526, "Intel Wireless-AC 9260; Linux: iwlwifi" },
    { 0x8086, 0x24fd, "Intel Wireless 8265 or 8275; Linux: iwlwifi" },
    { 0x8086, 0x24f3, "Intel Wireless 8260; Linux: iwlwifi" },
    { 0x8086, 0x095a, "Intel Wireless 7265; Linux: iwlwifi" },
    { 0x8086, 0x095b, "Intel Wireless 7265; Linux: iwlwifi" },
    { 0x8086, 0x3165, "Intel Wireless 3165; Linux: iwlwifi" },
    /* Realtek */
    { 0x10ec, 0xc822, "Realtek RTL8822CE, Wi-Fi 5 (802.11ac) 2x2; Linux: rtw88 (rtw88_8822ce)" },
    { 0x10ec, 0xc821, "Realtek RTL8821CE, Wi-Fi 5 (802.11ac) 1x1; Linux: rtw88 (rtw88_8821ce)" },
    { 0x10ec, 0xb822, "Realtek RTL8822BE, Wi-Fi 5 (802.11ac) 2x2; Linux: rtw88 (rtw88_8822be)" },
    { 0x10ec, 0xd723, "Realtek RTL8723DE, Wi-Fi 4 (802.11n); Linux: rtw88 (rtw88_8723de)" },
    { 0x10ec, 0x8852, "Realtek RTL8852AE, Wi-Fi 6 2x2; Linux: rtw89" },
    { 0x10ec, 0xb852, "Realtek RTL8852BE, Wi-Fi 6 2x2; Linux: rtw89" },
    { 0x10ec, 0xc852, "Realtek RTL8852CE, Wi-Fi 6E; Linux: rtw89" },
    /* Qualcomm (Atheros) */
    { 0x168c, 0x003e, "Qualcomm Atheros QCA6174, Wi-Fi 5 2x2; Linux: ath10k" },
    { 0x168c, 0x0042, "Qualcomm Atheros QCA9377, Wi-Fi 5 1x1; Linux: ath10k" },
    { 0x17cb, 0x1101, "Qualcomm QCA6390, Wi-Fi 6; Linux: ath11k" },
    { 0x17cb, 0x1103, "Qualcomm WCN6855, Wi-Fi 6E; Linux: ath11k" },
    /* MediaTek */
    { 0x14c3, 0x7961, "MediaTek MT7921, Wi-Fi 6; Linux: mt7921e" },
    { 0x14c3, 0x0608, "MediaTek MT7921K (RZ608), Wi-Fi 6E; Linux: mt7921e" },
    { 0x14c3, 0x0616, "MediaTek MT7922 (RZ616), Wi-Fi 6E; Linux: mt7921e" },
};

const char *pciinfo_chip(u16 vendor, u16 device) {
    u_int i;
    for(i = 0; i < sizeof(chips) / sizeof(chips[0]); i++)
        if(chips[i].vendor == vendor && chips[i].device == device) return chips[i].what;
    return NULL;
}

/* class 02 (network) subclass 80 (other: where Wi-Fi chips put themselves), class 0d (wireless),
   or an id in the table above */
bool pciinfo_is_wireless(const u8 *cfg) {
    u16 vendor = rd16(cfg, 0x00);
    if(vendor == 0xffff || vendor == 0) return false;
    if(cfg[0x0b] == 0x02 && cfg[0x0a] == 0x80) return true;
    if(cfg[0x0b] == 0x0d) return true;
    return pciinfo_chip(vendor, rd16(cfg, 0x02)) != NULL;
}

/* the Bluetooth half of a combo card sits on a USB port inside the laptop, and its id names the card */
static const struct { u16 vendor, product; const char *what; } bluetooth[] = {
    { 0x8087, 0x0026, "Intel AX201 Bluetooth: the Wi-Fi radio module is an AX201" },
    { 0x8087, 0x0aaa, "Intel 9560-family Bluetooth (Jefferson Peak): the Wi-Fi radio module is a 9560" },
    { 0x8087, 0x0029, "Intel AX200 Bluetooth" },
    { 0x8087, 0x0032, "Intel AX210 Bluetooth" },
    { 0x8087, 0x0033, "Intel AX211 Bluetooth" },
    { 0x8087, 0x0025, "Intel 9260 Bluetooth" },
    { 0x8087, 0x0a2b, "Intel 8265 Bluetooth" },
    { 0x0bda, 0xc822, "Realtek RTL8822CE Bluetooth: the Wi-Fi is an RTL8822CE" },
};

const char *pciinfo_bluetooth(u16 vendor, u16 product, u8 usb_class) {
    u_int i;
    for(i = 0; i < sizeof(bluetooth) / sizeof(bluetooth[0]); i++)
        if(bluetooth[i].vendor == vendor && bluetooth[i].product == product) return bluetooth[i].what;
    if(usb_class == 0xe0) {                             /* wireless controller: Bluetooth */
        if(vendor == 0x8087) return "Intel Bluetooth: the Wi-Fi is probably Intel too";
        if(vendor == 0x0bda) return "Realtek Bluetooth: the Wi-Fi is probably Realtek too";
        return "a Bluetooth controller: on a laptop, the Bluetooth half of the Wi-Fi card";
    }
    return NULL;
}
