/*
SurfOS PCI Bus Driver
--------------------
File: pci.c Date: 7/1/04, rebuilt 10/2026 (roadmap D1)
--------------------
(C)2004 Brandon Burr

Configuration mechanism 1. Every bus reachable through PCI-to-PCI bridges, every
function of multi-function devices, BARs sized with the decoders turned off. The
reconstructed 2004 scanner did bus 0, function 0 only (audit D3).
10/7/26: lspci -v and wifiinfo read each function's 256 bytes of configuration space
(reads only) and describe them with driver/pciinfo.c.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/irq.h>
#include <surfos/multiboot.h>
#include <asm/io.h>
#include <mm/kalloc.h>
#include <sys/pci.h>
#include <sys/pciinfo.h>
#include <blibc_common.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC
#define PCI_ADDR(bus, slot, func, reg) (0x80000000 | ((u32)(bus) << 16) | ((u32)(slot) << 11) | ((u32)(func) << 8) | ((reg) & 0xFC))

struct pci_dev *pci_devices;
static struct pci_dev *pci_tail;
static u_int ndevices;
static bool scanned[256];

/**** configuration space ****/

u32 pci_read32(u8 bus, u8 slot, u8 func, u8 reg) {
    u_long flags = irq_save();
    u32 v;
    outl(PCI_CONFIG_ADDR, PCI_ADDR(bus, slot, func, reg));
    v = inl(PCI_CONFIG_DATA);
    irq_restore(flags);
    return v;
}

u16 pci_read16(u8 bus, u8 slot, u8 func, u8 reg) {
    return (pci_read32(bus, slot, func, reg) >> ((reg & 2) * 8)) & 0xFFFF;
}

u8 pci_read8(u8 bus, u8 slot, u8 func, u8 reg) {
    return (pci_read32(bus, slot, func, reg) >> ((reg & 3) * 8)) & 0xFF;
}

void pci_write32(u8 bus, u8 slot, u8 func, u8 reg, u32 val) {
    u_long flags = irq_save();
    outl(PCI_CONFIG_ADDR, PCI_ADDR(bus, slot, func, reg));
    outl(PCI_CONFIG_DATA, val);
    irq_restore(flags);
}

void pci_write16(u8 bus, u8 slot, u8 func, u8 reg, u16 val) {
    u_long flags = irq_save();
    outl(PCI_CONFIG_ADDR, PCI_ADDR(bus, slot, func, reg));
    outw(PCI_CONFIG_DATA + (reg & 2), val);
    irq_restore(flags);
}

void pci_write8(u8 bus, u8 slot, u8 func, u8 reg, u8 val) {
    u_long flags = irq_save();
    outl(PCI_CONFIG_ADDR, PCI_ADDR(bus, slot, func, reg));
    outb(PCI_CONFIG_DATA + (reg & 3), val);
    irq_restore(flags);
}

u8 pci_dev_read8(struct pci_dev *d, u8 reg) { return pci_read8(d->bus, d->slot, d->func, reg); }
u16 pci_dev_read16(struct pci_dev *d, u8 reg) { return pci_read16(d->bus, d->slot, d->func, reg); }
u32 pci_dev_read32(struct pci_dev *d, u8 reg) { return pci_read32(d->bus, d->slot, d->func, reg); }
void pci_dev_write8(struct pci_dev *d, u8 reg, u8 val) { pci_write8(d->bus, d->slot, d->func, reg, val); }
void pci_dev_write16(struct pci_dev *d, u8 reg, u16 val) { pci_write16(d->bus, d->slot, d->func, reg, val); }
void pci_dev_write32(struct pci_dev *d, u8 reg, u32 val) { pci_write32(d->bus, d->slot, d->func, reg, val); }

/**** enumeration ****/

/* Size the BARs: write all ones, read back, restore; with the decoders off so a live
   device does not answer at a half-written address in between. */
static void size_bars(struct pci_dev *d, int nbars) {
    u16 cmd = pci_dev_read16(d, PCI_COMMAND);
    int i;

    pci_dev_write16(d, PCI_COMMAND, cmd & ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY));
    for(i = 0; i < nbars; i++) {
        u8 reg = PCI_BASE_ADDRESS_0 + i * 4;
        u32 orig = pci_dev_read32(d, reg), probe, mask;
        pci_dev_write32(d, reg, 0xFFFFFFFF);
        probe = pci_dev_read32(d, reg);
        pci_dev_write32(d, reg, orig);
        if(!probe || probe == 0xFFFFFFFF) continue;
        if(orig & PCI_BASE_ADDRESS_SPACE_IO) {
            /* an I/O decoder may be 16 bits wide and leave the upper half as zeros */
            mask = (probe & PCI_BASE_ADDRESS_IO_MASK) | 0xFFFF0000;
            d->bar_io[i] = true;
            d->bar[i] = orig & PCI_BASE_ADDRESS_IO_MASK;
            d->bar_size[i] = ~mask + 1;
        } else {
            mask = probe & PCI_BASE_ADDRESS_MEM_MASK;
            d->bar[i] = orig & PCI_BASE_ADDRESS_MEM_MASK;
            d->bar_size[i] = mask ? (~mask + 1) : 0;
            if((orig & PCI_BASE_ADDRESS_MEM_TYPE_MASK) == PCI_BASE_ADDRESS_MEM_TYPE_64 && i + 1 < nbars) {
                d->bar_hi[i] = pci_dev_read32(d, reg + 4);   /* the upper half follows; not sized */
                i++;
            }
        }
    }
    pci_dev_write16(d, PCI_COMMAND, cmd);
}

/* the addresses only, sizes unknown (0): for a device whose decoding must not stop */
static void read_bars(struct pci_dev *d, int nbars) {
    int i;
    for(i = 0; i < nbars; i++) {
        u32 v = pci_dev_read32(d, PCI_BASE_ADDRESS_0 + i * 4);
        d->bar_io[i] = (v & PCI_BASE_ADDRESS_SPACE_IO) ? true : false;
        d->bar[i] = v & (d->bar_io[i] ? PCI_BASE_ADDRESS_IO_MASK : PCI_BASE_ADDRESS_MEM_MASK);
        d->bar_size[i] = 0;
        if(!d->bar_io[i] && (v & PCI_BASE_ADDRESS_MEM_TYPE_MASK) == PCI_BASE_ADDRESS_MEM_TYPE_64 && i + 1 < nbars) {
            d->bar_hi[i] = pci_dev_read32(d, PCI_BASE_ADDRESS_0 + (i + 1) * 4);
            i++;
        }
    }
}

static void scan_bus(u8 bus);

static void add_function(u8 bus, u8 slot, u8 func) {
    struct pci_dev *d = (struct pci_dev *)kcalloc(1, sizeof(struct pci_dev));
    u32 class;
    if(!d) return;
    d->bus = bus; d->slot = slot; d->func = func;
    d->vendor = pci_read16(bus, slot, func, PCI_VENDOR_ID);
    d->device = pci_read16(bus, slot, func, PCI_DEVICE_ID);
    class = pci_read32(bus, slot, func, PCI_REVISION_ID);
    d->revision = class & 0xFF;
    d->prog_if = (class >> 8) & 0xFF;
    d->subclass = (class >> 16) & 0xFF;
    d->class_code = (class >> 24) & 0xFF;
    d->header_type = pci_read8(bus, slot, func, PCI_HEADER_TYPE) & 0x7F;
    d->irq = pci_read8(bus, slot, func, PCI_INTERRUPT_LINE);
    d->irq_pin = pci_read8(bus, slot, func, PCI_INTERRUPT_PIN);
    if(d->header_type == PCI_HEADER_TYPE_NORMAL) {
        d->subsys_vendor = pci_read16(bus, slot, func, PCI_SUBSYSTEM_VENDOR_ID);
        d->subsys_id = pci_read16(bus, slot, func, PCI_SUBSYSTEM_ID);
        /* 10/5/26: sizing turns the decoders off for a moment; on a laptop the display
           controller's BAR is the console's framebuffer, so it is left alone there */
        if(bootinfo.laptop && d->class_code == PCI_CLASS_DISPLAY) read_bars(d, 6);
        else size_bars(d, 6);
    } else if(d->header_type == PCI_HEADER_TYPE_BRIDGE) {
        size_bars(d, 2);
    }

    d->next = NULL;
    if(pci_tail) pci_tail->next = d;
    else pci_devices = d;
    pci_tail = d;
    ndevices++;

    if(d->class_code == PCI_CLASS_BRIDGE && d->subclass == PCI_SUBCLASS_PCI_BRIDGE) {
        u8 secondary = pci_read8(bus, slot, func, PCI_SECONDARY_BUS);
        if(secondary && !scanned[secondary]) scan_bus(secondary);
    }
}

static void scan_bus(u8 bus) {
    u8 slot, func;
    scanned[bus] = true;
    for(slot = 0; slot < 32; slot++) {
        u8 nfunc;
        if(pci_read16(bus, slot, 0, PCI_VENDOR_ID) == 0xFFFF) continue;
        nfunc = (pci_read8(bus, slot, 0, PCI_HEADER_TYPE) & 0x80) ? 8 : 1;
        for(func = 0; func < nfunc; func++) {
            if(pci_read16(bus, slot, func, PCI_VENDOR_ID) == 0xFFFF) continue;
            add_function(bus, slot, func);
        }
    }
}

u_int pci_device_count(void) { return ndevices; }

struct pci_dev *pci_find_device(u16 vendor, u16 device, struct pci_dev *after) {
    struct pci_dev *d = after ? after->next : pci_devices;
    for(; d; d = d->next) if(d->vendor == vendor && d->device == device) return d;
    return NULL;
}

struct pci_dev *pci_find_class(u8 class_code, u8 subclass, struct pci_dev *after) {
    struct pci_dev *d = after ? after->next : pci_devices;
    for(; d; d = d->next) if(d->class_code == class_code && d->subclass == subclass) return d;
    return NULL;
}

void pci_enable_device(struct pci_dev *d) {
    u16 cmd = pci_dev_read16(d, PCI_COMMAND);
    int i;
    for(i = 0; i < 6; i++) {
        if(!d->bar_size[i]) continue;
        cmd |= d->bar_io[i] ? PCI_COMMAND_IO : PCI_COMMAND_MEMORY;
    }
    cmd |= PCI_COMMAND_MASTER;
    pci_dev_write16(d, PCI_COMMAND, cmd);
}

static void print_size(u32 size) {
    if(size >= 1024 * 1024) kprintf("%uM", size / (1024 * 1024));
    else if(size >= 1024) kprintf("%uK", size / 1024);
    else kprintf("%u", size);
}

/* 10/7/26: the 256 bytes of configuration space that mechanism 1 reaches, a dword at a time */
void pci_read_config(struct pci_dev *d, u8 *cfg) {
    int i;
    for(i = 0; i < PCI_CONFIG_SIZE; i += 4) {
        u32 v = pci_dev_read32(d, (u8)i);
        cfg[i] = (u8)v;
        cfg[i + 1] = (u8)(v >> 8);
        cfg[i + 2] = (u8)(v >> 16);
        cfg[i + 3] = (u8)(v >> 24);
    }
}

struct pci_dev *pci_find_at(u8 bus, u8 slot, u8 func) {
    struct pci_dev *d;
    for(d = pci_devices; d; d = d->next) if(d->bus == bus && d->slot == slot && d->func == func) return d;
    return NULL;
}

/* 10/10/26 (Wi-Fi): where a capability is in configuration space (PCI_CAP_ID_*), 0 when the
   device does not list it; a looping or broken list ends after 48 entries */
u8 pci_find_capability(struct pci_dev *d, u8 id) {
    u8 off;
    int n;
    if(!(pci_dev_read16(d, PCI_STATUS) & PCI_STATUS_CAP_LIST)) return 0;
    off = pci_dev_read8(d, d->header_type == PCI_HEADER_TYPE_CARDBUS ? 0x14 : PCI_CAPABILITY_LIST) & 0xfc;
    for(n = 0; off >= 0x40 && n < 48; n++) {
        if(pci_dev_read8(d, off) == id) return off;
        off = pci_dev_read8(d, off + 1) & 0xfc;
    }
    return 0;
}

static void line_out(void *ctx, const char *line) {
    (void)ctx;
    kprintf("%s\n", line);
}

/* one device as lspci shows it: its line and its BARs; 10/7/26: with details, what its configuration
   space says (pciinfo.c) and what a known wireless chip probably is; with dump, the 256 bytes too */
void pci_print_device(struct pci_dev *d, bool details, bool dump) {
    char name[96];
    int i;
    kprintf("%02x:%02x.%x %02x%02x %s: %s %s (rev %02x)", d->bus, d->slot, d->func, d->class_code, d->subclass,
            pci_class_name(d->class_code, d->subclass), pci_vendor_name(d->vendor),
            pci_device_name(d->vendor, d->device, name, sizeof(name)), d->revision);
    if(d->irq_pin) kprintf(" irq %u", d->irq);
    kprintf("\n");
    for(i = 0; i < 6; i++) {
        if(!d->bar_size[i] && !d->bar_hi[i] && !(d->bar[i] && !d->bar_io[i] && d->class_code == PCI_CLASS_DISPLAY)) continue;
        if(d->bar_hi[i]) kprintf("        BAR%i mem 0x%x%08x [", i, d->bar_hi[i], d->bar[i]);
        else kprintf("        BAR%i %s 0x%08x [", i, d->bar_io[i] ? "io " : "mem", d->bar[i]);
        if(d->bar_size[i]) print_size(d->bar_size[i]);
        else kprintf("size not probed");
        kprintf("]\n");
    }
    if(details || dump) {
        u8 cfg[PCI_CONFIG_SIZE];
        pci_read_config(d, cfg);
        if(details) {
            const char *what = pciinfo_chip(d->vendor, d->device);
            pciinfo_details(cfg, line_out, NULL);
            if(what) kprintf("        probably: %s\n", what);
        }
        if(dump) {
            kprintf("        configuration space:\n");
            pciinfo_dump(cfg, line_out, NULL);
        }
    }
}

void pci_print(bool details) {
    struct pci_dev *d;
    for(d = pci_devices; d; d = d->next) pci_print_device(d, details, false);
    kprintf("%u devices\n", ndevices);
}

int init_pci(void) {
    u32 save, check;
    struct pci_dev *d;

    kprintf("\nInitializing PCI Bus\n");
    /* is configuration mechanism 1 there? */
    save = inl(PCI_CONFIG_ADDR);
    outl(PCI_CONFIG_ADDR, 0x80000000);
    check = inl(PCI_CONFIG_ADDR);
    outl(PCI_CONFIG_ADDR, save);
    if(check != 0x80000000) {
        kprintf("*No PCI Bus Detected\n");
        return 1;
    }

    memset(scanned, 0, sizeof(scanned));
    pci_devices = pci_tail = NULL;
    ndevices = 0;
    scan_bus(0);
    for(d = pci_devices; d; d = d->next) {
        kprintf("+PCI: %02x:%02x.%x %04x:%04x %s\n", d->bus, d->slot, d->func, d->vendor, d->device, pci_class_name(d->class_code, d->subclass));
    }
    kprintf("*%u PCI Devices Found\n*DONE\n", ndevices);
    return 0;
}
