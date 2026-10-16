/*
SurfOS IdeaPad I2C Touchpad
-------------------------
10/12/26. A deliberately narrow platform binding for the observed Lenovo
81WE Ice Lake firmware. AML is inspected, never executed. Other systems keep
the ordinary PS/2 and USB paths. Transport/protocol parsers are separate.
*/
#include <surfos/acpi.h>
#include <surfos/multiboot.h>
#include <surfos/task.h>
#include <surfos/timer.h>
#include <surfos/irq.h>
#include <surfos/console.h>
#include <mm/paging.h>
#include <mm/kalloc.h>
#include <sys/pci.h>
#include <sys/pointer.h>
#include <sys/amlfield.h>
#include <sys/i2cdw.h>
#include <sys/i2chid.h>
#include <sys/hidpointer.h>
#include <sys/touchpad.h>
#include <stdio.h>
#include <string.h>

struct touchpad_state {
    struct pci_dev *pci;
    struct i2cdw bus;
    struct i2chid_desc hid;
    struct hid_pointer_layout layout;
    volatile u32 *regs, *gpio;
    u32 bar, gpio_address, window_base, window_size;
    u32 gpio_initial[2], gpio_last, reports, decoded, errors, ignored, zero_length, reinits;
    u32 mode_line[2];                   /* The interrupt pad before and after the mode transfer. */
    u16 hcnt, lcnt, hold, descriptor_reg;
    u8 address, variant, last[256];
    u_int last_len, buttons, consecutive_errors;
    bool found, ready, busy, thread, requested;
    u8 *report_descriptor;
    u_int report_descriptor_len;
    int parse_result;
    const char *stage, *mode;
    struct i2cdw_status mode_status;    /* The mode transfer that failed, if one did. */
};
static struct touchpad_state tp;

static bool overlaps(u64 a, u64 n, u64 b, u64 m) {
    return n && m && a < b + m && b < a + n;
}

/* NVS is normally sufficient. Lenovo puts LFCN in reserved DRAM instead;
   permit only that checked AML region, below the PCI aperture, and never RAM,
   modules or the framebuffer. No user-selected physical address enters here. */
static bool firmware_read(const struct aml_field *f, u_int width, bool lfcn, u64 *value) {
    u64 address, end, checked_address, checked_length, checked_end;
    u_int i;
    const void *p;
    bool contained = false, ok;
    if(f->bit_width != width || (f->bit_offset & 7) || !width || width > 64 || (width & 7) ||
       f->region_length > 4096 || (u64)f->bit_offset + width > (u64)f->region_length * 8)
        return false;
    address = f->address + f->bit_offset / 8;
    end = address + width / 8;
    if(!address || address < f->address || end > 0x100000000ULL || end < address) return false;
    checked_address = lfcn ? f->address : address;
    checked_length = lfcn ? f->region_length : width / 8;
    checked_end = checked_address + checked_length;
    if(checked_end < checked_address || checked_end > 0x100000000ULL) return false;
    if(lfcn && (f->region_length != 0xfd || checked_address < 0x100000 || checked_end > tp.window_base)) return false;
    for(i = 0; i < bootinfo.nregions; i++) {
        const struct mb_region *r = &bootinfo.regions[i];
        if(r->base + r->len < r->base) return false;
        if(!overlaps(checked_address, checked_length, r->base, r->len)) continue;
        if(r->type != 4 && !(lfcn && r->type == 2)) return false;
        if(checked_address >= r->base && checked_end <= r->base + r->len) contained = true;
    }
    if(!contained) return false;
    for(i = 0; i < bootinfo.nmods; i++)
        if(overlaps(checked_address, checked_length, bootinfo.mods[i].start,
                    bootinfo.mods[i].end - bootinfo.mods[i].start)) return false;
    if(bootinfo.fb.present && overlaps(checked_address, checked_length, bootinfo.fb.addr,
                                      (u64)bootinfo.fb.pitch * bootinfo.fb.height)) return false;
    *value = 0;
    kcritical_enter();
    p = vmm_temp_map(address, width / 8);
    ok = p != NULL;
    if(ok) memcpy(value, p, width / 8);
    kcritical_leave();
    return ok;
}

static bool field(const u8 *table, u32 bytes, const char *region, const char *name,
                  u_int width, bool lfcn, u64 *value) {
    struct aml_field f;
    return aml_field_find(table, bytes, region, name, &f) == 0 && firmware_read(&f, width, lfcn, value);
}

static bool discover(void) {
    u8 *dsdt = NULL, *ssdt = NULL;
    u32 bytes, n;
    u64 base = 0, length = 0, v, mode, config, pch, sideband;
    u_int i, windows = 0;
    bool ok = false;
    struct pci_dev *host = pci_find_at(0, 0, 0);
    tp.stage = "firmware discovery";
    if(!bootinfo.laptop || !acpi.present || strcmp(acpi.oem, "LENOVO") ||
       !host || host->vendor != 0x8086 || host->device != 0x8a02 || host->subsys_vendor != 0x17aa ||
       host->subsys_id != 0x3808) return false;
    tp.pci = pci_find_at(0, 0x15, 1);
    if(!tp.pci || tp.pci->vendor != 0x8086 || tp.pci->device != 0x34e9 ||
       tp.pci->subsys_vendor != 0x17aa || tp.pci->subsys_id != 0x3818) return false;
    if(acpi_aml_copy("DSDT", 0, &dsdt, &bytes) != ACPI_COPY_OK) return false;
    if(bytes < 36 || memcmp(dsdt + 10, "LENOVO", 6) || memcmp(dsdt + 16, "CB-01   ", 8)) goto out;
    for(i = 0; i < ACPI_MAX_TABLES; i++) {
        u64 candidate_base, candidate_length;
        int rc = acpi_aml_copy("SSDT", i, &ssdt, &n);
        if(rc == ACPI_COPY_ABSENT) break;
        if(rc != ACPI_COPY_OK) continue;
        if(field(ssdt, n, "SANV", "M32B", 32, false, &candidate_base) &&
           field(ssdt, n, "SANV", "M32L", 32, false, &candidate_length)) {
            base = candidate_base; length = candidate_length; windows++;
        }
        kfree(ssdt); ssdt = NULL;
    }
    tp.stage = "firmware PCI aperture";
    if(windows != 1 || base < 0x10000000 || !length || base + length > 0x100000000ULL) goto out;
    tp.window_base = (u32)base; tp.window_size = (u32)length;
    tp.stage = "firmware I2C1 configuration";
    if(!field(dsdt, bytes, "PNVA", "IM01", 8, false, &mode) || mode != 1 ||
       !field(dsdt, bytes, "PNVA", "IC01", 64, false, &config) || config != 0xa9000 ||
       !field(dsdt, bytes, "PNVA", "PCHS", 16, false, &pch) || pch != 2 ||
       !field(dsdt, bytes, "PNVA", "SBRG", 32, false, &sideband) || sideband != 0xfd000000) goto out;
    tp.stage = "firmware touchpad variant";
    if(!field(dsdt, bytes, "LFCN", "TPTY", 8, true, &v)) goto out;
    tp.variant = (u8)v;
    switch(tp.variant) {
    case 1: tp.address = 0x15; tp.descriptor_reg = 1; break;
    case 2: case 4: tp.address = 0x2c; tp.descriptor_reg = 0x20; break;
    case 5: tp.address = 0x38; tp.descriptor_reg = 1; break;
    default: tp.stage = "firmware has no supported touchpad variant"; goto out;
    }
    /* Values supplied by this firmware's I2C1 timing bank at 133 MHz. */
    tp.stage = "firmware I2C1 timings";
    if(!field(dsdt, bytes, "GNVS", "FMH1", 16, false, &v) || v < 6 || v > 65535) goto out;
    tp.hcnt = (u16)v;
    if(!field(dsdt, bytes, "GNVS", "FML1", 16, false, &v) || v < 8 || v > 65535) goto out;
    tp.lcnt = (u16)v;
    if(!field(dsdt, bytes, "GNVS", "FMD1", 16, false, &v) || !v || v >= tp.lcnt) goto out;
    tp.hold = (u16)v;
    tp.gpio_address = (u32)sideband + 0x6e0780; /* AML GPC0: group 1, pad 16, PCH-L. */
    ok = true;
out:
    if(ssdt) kfree(ssdt);
    kfree(dsdt);
    return ok;
}

/* Choose inside PCI0's actual producer window. Unknown-size BARs below the
   candidate conservatively block it. Bridge apertures and RAM are excluded. */
static bool aperture_free(u32 base) {
    struct pci_dev *d;
    u_int i;
    if((u64)base < tp.window_base || (u64)base + 4096 > (u64)tp.window_base + tp.window_size) return false;
    for(i = 0; i < bootinfo.nregions; i++) {
        struct mb_region *r = &bootinfo.regions[i];
        if(r->type != 2 && overlaps(base, 4096, r->base, r->len)) return false;
    }
    if(bootinfo.fb.present && overlaps(base, 4096, bootinfo.fb.addr,
                                      (u64)bootinfo.fb.pitch * bootinfo.fb.height)) return false;
    for(i = 0; i < bootinfo.nmods; i++)
        if(overlaps(base, 4096, bootinfo.mods[i].start,
                    bootinfo.mods[i].end - bootinfo.mods[i].start)) return false;
    for(d = pci_devices; d; d = d->next) {
        for(i = 0; i < (d->header_type == PCI_HEADER_TYPE_BRIDGE ? 2U : 6U); i++) {
            u64 b = ((u64)d->bar_hi[i] << 32) | d->bar[i];
            if((d == tp.pci && i == 0) || d->bar_io[i] || !b) continue;
            if(!d->bar_size[i] ? b < (u64)base + 4096 : overlaps(base, 4096, b, d->bar_size[i])) return false;
        }
        if(d->header_type == PCI_HEADER_TYPE_BRIDGE) {
            u32 m = pci_dev_read32(d, 0x20), p = pci_dev_read32(d, 0x24);
            u64 b = (m & 0xfff0) << 16, e = (m & 0xfff00000) | 0xfffff;
            if(b <= e && overlaps(base, 4096, b, e - b + 1)) return false;
            b = (p & 0xfff0) << 16; e = (p & 0xfff00000) | 0xfffff;
            if((p & 15) == 1) { b |= (u64)pci_dev_read32(d, 0x28) << 32; e |= (u64)pci_dev_read32(d, 0x2c) << 32; }
            if(b <= e && overlaps(base, 4096, b, e - b + 1)) return false;
        }
    }
    return true;
}

static u32 reg_read(void *ctx, u32 offset) { return ((volatile u32 *)ctx)[offset / 4]; }
static void reg_write(void *ctx, u32 offset, u32 value) { ((volatile u32 *)ctx)[offset / 4] = value; }
static void reg_delay(void *ctx, u32 us) { (void)ctx; udelay(us); }

static bool controller_start(void) {
    u32 raw, chosen, status, caps;
    u16 command, pm;
    u8 cap;
    u_int i;
    struct i2cdw_io io;
    struct i2cdw_config timing;
    tp.stage = "PCI resources";
    if(tp.pci->bar_size[0] != 4096 || tp.pci->bar_io[0] || tp.pci->bar_hi[0]) return false;
    raw = pci_dev_read32(tp.pci, PCI_BASE_ADDRESS_0);
    chosen = raw & ~15U;
    if(!chosen) {
        chosen = (tp.window_base + 4095U) & ~4095U;
        for(i = 0; i < 256 && !aperture_free(chosen); i++) chosen += 4096;
        if(i == 256 || !aperture_free(chosen)) return false;
    } else if(!aperture_free(chosen)) return false;
    cap = pci_find_capability(tp.pci, 1);
    if(!cap || cap > 0xf8) return false;
    pm = pci_dev_read16(tp.pci, cap + 4);
    pci_dev_write16(tp.pci, cap + 4, pm & ~0x8003U);
    mdelay(10);
    if(pci_dev_read16(tp.pci, cap + 4) & 3) { tp.stage = "PCI D0 refused"; return false; }
    command = pci_dev_read16(tp.pci, PCI_COMMAND);
    pci_dev_write16(tp.pci, PCI_COMMAND, command & ~(PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER));
    pci_dev_write32(tp.pci, PCI_BASE_ADDRESS_0, chosen | (raw & 15));
    if((raw & 6) == 4) pci_dev_write32(tp.pci, PCI_BASE_ADDRESS_0 + 4, 0);
    if((pci_dev_read32(tp.pci, PCI_BASE_ADDRESS_0) & ~15U) != chosen) { tp.stage = "BAR assignment refused"; return false; }
    tp.pci->bar[0] = chosen;
    pci_dev_write16(tp.pci, PCI_COMMAND, (command & ~(PCI_COMMAND_IO | PCI_COMMAND_MASTER)) |
                       PCI_COMMAND_MEMORY | PCI_COMMAND_INTX_DISABLE);
    if(!tp.regs) { tp.regs = ioremap(chosen, 4096, false); tp.bar = chosen; }
    if(!tp.regs || tp.bar != chosen) return false;
    tp.stage = "LPSS power and reset";
    caps = reg_read((void *)tp.regs, 0x2fc);
    if(caps == 0xffffffff || ((caps >> 4) & 15)) return false;
    for(i = 0; i < 1000 && (reg_read((void *)tp.regs, 0x24c) & 1); i++) udelay(10);
    if(i == 1000) return false;
    status = reg_read((void *)tp.regs, 0x24c);
    if(status & 4) {
        reg_write((void *)tp.regs, 0x24c, status & ~12U);
        for(i = 0; i < 1000 && (reg_read((void *)tp.regs, 0x24c) & 1); i++) udelay(10);
        if(i == 1000) return false;
    }
    status = reg_read((void *)tp.regs, 0x24c);
    if(status & 4) return false;
    if(status & 8) reg_write((void *)tp.regs, 0x24c, status); /* acknowledge restore separately */
    reg_write((void *)tp.regs, 0x204, 0);
    udelay(10);
    reg_write((void *)tp.regs, 0x204, 7);
    reg_write((void *)tp.regs, 0x240, chosen);
    reg_write((void *)tp.regs, 0x244, 0);
    io = (struct i2cdw_io){ (void *)tp.regs, reg_read, reg_write, reg_delay };
    timing = (struct i2cdw_config){ I2CDW_FAST, tp.hcnt, tp.lcnt, tp.hold | 0x10000, 10, 200000 };
    tp.stage = "DesignWare setup";
    if(i2cdw_init(&tp.bus, &io, &timing) != I2CDW_OK) return false;
    if(!tp.gpio) tp.gpio = ioremap(tp.gpio_address, 8, false);
    if(!tp.gpio) return false;
    tp.gpio_initial[0] = tp.gpio[0]; tp.gpio_initial[1] = tp.gpio[1];
    /* Firmware sets the pin mux. Do not change its polarity or route an IRQ. */
    if(tp.gpio[0] == 0xffffffff || (tp.gpio[0] & (7U << 10)) || (tp.gpio[0] & (1U << 9))) {
        tp.stage = "touchpad GPIO is not an enabled input"; return false;
    }
    return true;
}

static bool transfer(const u8 *out, u_int sent, u8 *in, u_int received) {
    /* 10/15/26: a controller whose error cleanup failed is set up again
       (bounded) rather than refusing every later transfer. */
    if(!tp.bus.ready) {
        struct i2cdw_io io = tp.bus.io;
        struct i2cdw_config config = tp.bus.config;
        tp.reinits++;
        if(i2cdw_init(&tp.bus, &io, &config) != I2CDW_OK) { tp.errors++; return false; }
    }
    if(i2cdw_xfer(&tp.bus, tp.address, out, sent, in, received) == I2CDW_OK) return true;
    tp.errors++;
    return false;
}

static bool read_register(u16 reg, u8 *data, u_int bytes) {
    u8 out[2] = { reg & 255, reg >> 8 };
    return transfer(out, 2, data, bytes);
}

static bool input_ready(void) {
    tp.gpio_last = tp.gpio[0];
    /* AML declares a level-low interrupt. RXSTATE is the pad's input level. */
    return !(tp.gpio_last & 2);
}

static bool command(int bytes, const u8 *data) {
    return bytes > 0 && transfer(data, (u_int)bytes, NULL, 0);
}

/* 10/15/26: Microsoft's configuration collection has the host set InputMode (0 is
   the mouse collection) after reading the report descriptor; nothing reads it
   back, and Linux only sets it. The IdeaPad's Synaptics 06cb:ce2d NACKs
   GET_REPORT's opcode byte for it (abort 03000008: 6 commands flushed). A report
   holding nothing else is therefore built whole and set; one shared with other
   feature data is read, patched and written back. Our reset already returned
   the mode to the device's default collection, so a refusal is recorded and
   input goes on: reports from another collection are counted as ignored.
   https://learn.microsoft.com/en-us/windows-hardware/design/component-guidelines/touchpad-configuration-collection */
static const char *select_mouse_mode(void) {
    u8 out[288], report[I2CHID_MAX_INPUT];
    const u8 *payload;
    u_int total = tp.layout.mode.report_bytes, prefix = tp.layout.numbered ? 1 : 0, bytes;
    enum hid_pointer_error e;
    i64 mode;
    int n;
    if(!tp.layout.mode.field.bit_size) return "device default (no InputMode feature)";
    if(total <= prefix || total > sizeof(report) - 2) return "device default (InputMode report size unsupported)";
    e = hid_pointer_mode_report(&tp.layout, report, total, 0);
    if(e == HID_POINTER_OK) {
        n = i2chid_set_report(&tp.hid, I2CHID_REPORT_FEATURE, tp.layout.mode.report_id,
                              report + prefix, total - prefix, out, sizeof(out));
        if(command(n, out)) return "mouse (InputMode 0 set)";
        tp.mode_status = tp.bus.last;
        return "device default (SET_REPORT refused)";
    }
    if(e != HID_POINTER_UNSUPPORTED) return "device default (InputMode report invalid)";
    n = i2chid_get_report(&tp.hid, I2CHID_REPORT_FEATURE, tp.layout.mode.report_id, out, sizeof(out));
    if(n < 0 || !transfer(out, (u_int)n, report, total + 2)) {
        tp.mode_status = tp.bus.last;
        return "device default (GET_REPORT refused)";
    }
    if(i2chid_get_input(report, total + 2, total + 2, &payload, &bytes) != I2CHID_INPUT_DATA ||
       bytes != total || hid_pointer_get_mode(&tp.layout, payload, bytes, &mode) != HID_POINTER_OK)
        return "device default (unreadable InputMode report)";
    if(mode == 0) return "mouse (InputMode read: 0)";
    if(hid_pointer_set_mode(&tp.layout, report + 2, bytes, 0) != HID_POINTER_OK)
        return "device default (InputMode report invalid)";
    n = i2chid_set_report(&tp.hid, I2CHID_REPORT_FEATURE, tp.layout.mode.report_id,
                          report + 2 + prefix, bytes - prefix, out, sizeof(out));
    if(command(n, out)) return "mouse (InputMode read, 0 set)";
    tp.mode_status = tp.bus.last;
    return "device default (SET_REPORT refused)";
}

static bool hid_start(void) {
    u8 desc[I2CHID_DESCRIPTOR_LENGTH], out[288], input[256];
    const u8 *payload;
    u_int bytes, i;
    int n;
    bool reset_done = false;
    tp.report_descriptor_len = 0;
    if(tp.report_descriptor) { kfree(tp.report_descriptor); tp.report_descriptor = NULL; }
    memset(&tp.hid, 0, sizeof(tp.hid));
    tp.mode = NULL;
    memset(&tp.mode_status, 0, sizeof(tp.mode_status));
    tp.stage = "HID descriptor";
    if(!read_register(tp.descriptor_reg, desc, sizeof(desc)) ||
       !i2chid_parse_descriptor(desc, sizeof(desc), &tp.hid)) return false;
    tp.stage = "HID power on";
    n = i2chid_set_power(&tp.hid, I2CHID_POWER_ON, out, sizeof(out));
    if(!command(n, out)) return false;
    sleep_ms(60);
    tp.stage = "HID reset";
    n = i2chid_reset(&tp.hid, out, sizeof(out));
    if(!command(n, out)) return false;
    for(i = 0; i < 100; i++) {
        sleep_ms(10);
        if(!input_ready()) continue;
        if(!transfer(NULL, 0, input, tp.hid.max_input_len)) return false;
        if(i2chid_get_input(input, tp.hid.max_input_len, tp.hid.max_input_len,
                           &payload, &bytes) == I2CHID_INPUT_RESET) { reset_done = true; break; }
    }
    if(!reset_done) { tp.stage = "HID reset did not complete"; return false; }
    /* ELAN controllers reject an extra wake immediately after reset. */
    if(tp.hid.vendor_id != 0x04f3) {
        n = i2chid_set_power(&tp.hid, I2CHID_POWER_ON, out, sizeof(out));
        if(!command(n, out)) return false;
        sleep_ms(60);
    }
    tp.stage = "HID report descriptor";
    tp.report_descriptor = kalloc(tp.hid.report_desc_len);
    if(!tp.report_descriptor || !read_register(tp.hid.report_desc_reg, tp.report_descriptor,
                                              tp.hid.report_desc_len)) return false;
    tp.report_descriptor_len = tp.hid.report_desc_len;
    tp.parse_result = hid_pointer_parse(tp.report_descriptor, tp.hid.report_desc_len, &tp.layout);
    if(tp.parse_result != HID_POINTER_OK || !tp.layout.has_pointer) {
        tp.stage = "HID descriptor has no supported mouse reports"; return false;
    }
    tp.stage = "HID select mouse mode";
    tp.mode_line[0] = tp.gpio[0];
    tp.mode = select_mouse_mode();
    tp.mode_line[1] = tp.gpio[0];
    tp.bus.config.timeout_us = 20000;
    tp.stage = "ready (I2C HID mouse reports)";
    return true;
}

/* Reads one input report; true only for a data report, so a line that stays low
   without data costs one read per wake. */
static bool read_input(void) {
    u8 input[256];
    const u8 *payload;
    u_int bytes;
    struct hid_pointer_sample sample;
    enum i2chid_input_result result;
    if(!transfer(NULL, 0, input, tp.hid.max_input_len)) {
        if(++tp.consecutive_errors >= 5) {
            tp.ready = false;
            tp.stage = "I2C input stopped after repeated errors; touchpad up retries";
            tp.buttons = 0;
            pointer_device(POINTER_I2C, false);
        }
        return false;
    }
    tp.consecutive_errors = 0;
    tp.reports++;
    result = i2chid_get_input(input, tp.hid.max_input_len, tp.hid.max_input_len, &payload, &bytes);
    if(result == I2CHID_INPUT_DATA) {
        kcritical_enter();
        memcpy(tp.last, payload, bytes); tp.last_len = bytes;
        kcritical_leave();
        if(hid_pointer_decode(&tp.layout, payload, bytes, &sample) == HID_POINTER_OK) {
            tp.buttons = (tp.buttons & ~sample.button_mask) | sample.buttons;
            pointer_report(POINTER_I2C, sample.dx, sample.dy, 0, tp.buttons);
            tp.decoded++;
        } else tp.ignored++;
        return true;
    }
    /* Zero length: a device-initiated reset, or a read with nothing waiting.
       As Linux does, count it and keep reading. The device stays powered and
       reports from its default (or kept) collection, and its next report
       carries the real button state. Releasing here would break a drag. */
    if(result == I2CHID_INPUT_RESET) tp.zero_length++;
    else if(result == I2CHID_INPUT_INVALID) tp.errors++;
    return false;
}

/* The device holds its line low while reports wait; a few per wake keep up
   with a moving finger without letting one wake take long (each read polls). */
#define TOUCHPAD_DRAIN 3
static void poll_input(void) {
    u_int n;
    for(n = 0; n < TOUCHPAD_DRAIN && tp.ready && input_ready(); n++)
        if(!read_input()) break;
}

/* One dedicated task owns all transfers, even when several consoles request
   activation. A disconnected telnet session cannot interrupt hardware setup. */
static void touchpad_thread(void *arg) {
    (void)arg;
    for(;;) {
        if(tp.requested) {
            tp.requested = false; tp.busy = true; tp.ready = false;
            tp.buttons = tp.consecutive_errors = 0;
            pointer_device(POINTER_I2C, false);
            memset(&tp.layout, 0, sizeof(tp.layout));
            if(discover() && controller_start() && hid_start()) {
                tp.ready = true;
                pointer_device(POINTER_I2C, true);
            }
            tp.busy = false;
            kprintf("+TOUCHPAD: %s; variant=%u address=%02x device=%04x:%04x\n",
                    tp.stage, tp.variant, tp.address, tp.hid.vendor_id, tp.hid.product_id);
        }
        if(tp.ready) poll_input();
        sleep_ms(10);
    }
}

int init_touchpad(void) {
    struct pci_dev *d = pci_find_at(0, 0x15, 1), *h = pci_find_at(0, 0, 0);
    tp.stage = "not present on this platform";
    if(!bootinfo.laptop || !d || d->vendor != 0x8086 || d->device != 0x34e9 ||
       d->subsys_vendor != 0x17aa || d->subsys_id != 0x3818 || !h || h->vendor != 0x8086 ||
       h->device != 0x8a02 || h->subsys_vendor != 0x17aa || h->subsys_id != 0x3808) return 1;
    tp.found = true; tp.requested = true; tp.stage = "starting";
    if(!kthread_create("touchpad", touchpad_thread, NULL, PL_NORMAL, TF_DETACHED)) {
        tp.requested = false; tp.stage = "could not create input task"; return -1;
    }
    tp.thread = true;
    return 0;
}

/* 1: a start was requested; 2: a working touchpad will be started again (its
   reset and mode selection rerun, 10/15/26); 0: a start is already running. */
int touchpad_start(void) {
    bool ready = tp.ready;
    if(!tp.found || !tp.thread) return -1;
    if(tp.busy) return 0;
    tp.requested = true;
    return ready ? 2 : 1;
}

void touchpad_print(void) {
    printf("    touchpad: %s\n", tp.stage ? tp.stage : "not present on this platform");
    if(!tp.found) return;
    printf("    I2C1: BAR=%08x window=%08x+%08x address=%02x variant=%u descriptor=%04x\n",
           tp.bar, tp.window_base, tp.window_size, tp.address, tp.variant, tp.descriptor_reg);
    printf("    timing: fast high=%u low=%u hold=%u; transport=%u abort=%08x tx=%u rx=%u bus=%s reinits=%u\n",
           tp.hcnt, tp.lcnt, tp.hold, tp.bus.last.error, tp.bus.last.abort_source,
           tp.bus.last.tx_queued, tp.bus.last.rx_done, tp.bus.ready ? "ready" : "down", tp.reinits);
    printf("    GPIO: %08x initial=%08x/%08x current=%08x\n", tp.gpio_address,
           tp.gpio_initial[0], tp.gpio_initial[1], tp.gpio ? tp.gpio[0] : 0);
    printf("    HID: %04x:%04x version=%04x report=%u input=%u parse=%d\n",
           tp.hid.vendor_id, tp.hid.product_id, tp.hid.version_id, tp.hid.report_desc_len,
           tp.hid.max_input_len, tp.parse_result);
    printf("    registers: report=%04x input=%04x output=%04x/%u command=%04x data=%04x\n",
           tp.hid.report_desc_reg, tp.hid.input_reg, tp.hid.output_reg, tp.hid.max_output_len,
           tp.hid.command_reg, tp.hid.data_reg);
    printf("    mode: %s; line %08x/%08x", tp.mode ? tp.mode : "not selected",
           tp.mode_line[0], tp.mode_line[1]);
    if(tp.mode_status.error)
        printf("; transport=%u abort=%08x tx=%u rx=%u recovered=%u", tp.mode_status.error,
               tp.mode_status.abort_source, tp.mode_status.tx_queued, tp.mode_status.rx_done,
               tp.mode_status.recovered);
    printf("\n");
    printf("    input: ready=%u reports=%u decoded=%u ignored=%u errors=%u zero-length=%u buttons=%u\n",
           tp.ready, tp.reports, tp.decoded, tp.ignored, tp.errors, tp.zero_length, tp.buttons);
}

void touchpad_report_print(void) {
    u8 copy[256];
    u_int n, i;
    kcritical_enter(); n = tp.last_len; memcpy(copy, tp.last, n); kcritical_leave();
    printf("    last HID input: %u bytes\n", n);
    for(i = 0; i < n; i++) printf("%s%02x%s", i % 16 ? " " : "    ", copy[i],
                                 i % 16 == 15 || i + 1 == n ? "\n" : "");
}

void touchpad_descriptor_print(void) {
    u8 *copy;
    u_int n, i;
    /* Copy before printing: a retry can replace the descriptor while the
       console pager sleeps. Allocation stays outside the critical section. */
    copy = kalloc(I2CHID_MAX_REPORT_DESCRIPTOR);
    if(!copy) { printf("    no memory\n"); return; }
    kcritical_enter();
    n = tp.busy || !tp.report_descriptor ? 0 : tp.report_descriptor_len;
    if(n) memcpy(copy, tp.report_descriptor, n);
    kcritical_leave();
    printf("    HID report descriptor: %u bytes\n", n);
    for(i = 0; i < n; i++) printf("%s%02x%s", i % 16 ? " " : "    ", copy[i],
                                 i % 16 == 15 || i + 1 == n ? "\n" : "");
    kfree(copy);
}
