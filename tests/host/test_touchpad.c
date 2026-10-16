/*
SurfOS Touchpad Platform Safety Tests
--------------------
File: test_touchpad.c Date: 10/12/26
--------------------
Compile the actual private discovery and range guards with safe memory/PCI
fixtures. Function-section GC discards all hardware activation and transfer
code. No hardware I/O implementation is linked into this executable.
10/15/26: the HID start and input path run against a simulated touchpad behind
a fake i2cdw_xfer() (the real I2C framing and HID parsing are linked).
*/
#include "../../driver/touchpad.c"
#include "touchpad_06cb_ce2d.h"

void *malloc(unsigned long);
void free(void *);
static unsigned checks, failures, critical, maps, live, requests;
static bool fail_map;
struct boot_info bootinfo;
struct acpi_info acpi;
struct pci_dev *pci_devices;
static struct pci_dev host, controller, peer;
static u32 peer_config[64];
static u8 physical[5][4096];
static const u64 bases[5] = { 0x200000, 0x210000, 0x220000, 0x230000, 0x240000 };
struct table_data { u8 bytes[512]; u32 size; };
static struct table_data dsdt, ssdt[2];
static u32 ssdt_count;

static void check(bool yes, const char *what) {
    checks++;
    if(!yes) { failures++; printf("FAIL: %s\n", what); }
}
void kcritical_enter(void) { critical++; }
void kcritical_leave(void) { check(critical == 1, "mapping/snapshot critical section is balanced"); critical--; }
void *kalloc(size_t bytes) {
    void *p;
    check(!critical, "allocation occurs outside the critical section");
    p = malloc(bytes);
    if(p) live++;
    return p;
}
void kfree(void *p) {
    check(!critical, "free occurs outside the critical section");
    if(p) { live--; free(p); }
}
void *vmm_temp_map(u64 address, u_long bytes) {
    u32 i;
    maps++;
    check(critical == 1, "firmware mapping occurs inside the critical section");
    if(fail_map) return NULL;
    for(i = 0; i < 5; i++) if(address >= bases[i] && address - bases[i] < 4096 &&
            bytes <= 4096 - (address - bases[i])) return physical[i] + (address - bases[i]);
    return NULL;
}
struct pci_dev *pci_find_at(u8 bus, u8 slot, u8 function) {
    if(bus) return NULL;
    if(!slot && !function) return &host;
    return slot == 0x15 && function == 1 ? &controller : NULL;
}
u32 pci_dev_read32(struct pci_dev *d, u8 reg) {
    check(d == &peer, "only simulated bridge configuration is read");
    return peer_config[reg / 4];
}
int acpi_aml_copy(const char *signature, u_int index, u8 **out, u32 *size) {
    const struct table_data *t;
    requests++;
    *out = NULL; *size = 0;
    if(!strcmp(signature, "DSDT") && !index) t = &dsdt;
    else if(!strcmp(signature, "SSDT") && index < ssdt_count) t = &ssdt[index];
    else return ACPI_COPY_ABSENT;
    *out = kalloc(t->size);
    if(!*out) return ACPI_COPY_NOMEM;
    memcpy(*out, t->bytes, t->size); *size = t->size;
    return ACPI_COPY_OK;
}

static void put(u32 bank, u32 offset, u64 value, u32 bytes) {
    u32 i;
    for(i = 0; i < bytes; i++) physical[bank][offset + i] = value >> (i * 8);
}
static void emit(struct table_data *t, u8 byte) { t->bytes[t->size++] = byte; }
static void emit_name(struct table_data *t, const char *name) {
    memcpy(t->bytes + t->size, name, 4); t->size += 4;
}
static void emit_integer(struct table_data *t, u32 n, u32 bytes) {
    u32 i;
    emit(t, bytes == 4 ? 0x0c : 0x0b);
    for(i = 0; i < bytes; i++) emit(t, n >> (8 * i));
}
static void emit_length(struct table_data *t, u32 n) {
    if(n < 64) emit(t, n);
    else if(n < 4096) { emit(t, 0x40 | (n & 15)); emit(t, n >> 4); }
    else { emit(t, 0x80 | (n & 15)); emit(t, n >> 4); emit(t, n >> 12); }
}
static void table_begin(struct table_data *t, const char *signature) {
    memset(t, 0, sizeof(*t)); t->size = 36;
    memcpy(t->bytes, signature, 4); t->bytes[8] = 2;
    memcpy(t->bytes + 10, "LENOVO", 6); memcpy(t->bytes + 16, "CB-01   ", 8);
}
struct field_data { const char *name; u32 offset, bits; };
static void table_region(struct table_data *t, const char *name, u32 address, u32 length,
                         const struct field_data *fields, u32 count) {
    u32 at, bits = 0, i, package_length;
    emit(t, 0x5b); emit(t, 0x80); emit_name(t, name); emit(t, 0);
    emit_integer(t, address, 4); emit_integer(t, length, 2);
    emit(t, 0x5b); emit(t, 0x81); at = t->size; emit(t, 0); emit(t, 0);
    emit_name(t, name); emit(t, 0x10);
    for(i = 0; i < count; i++) {
        if(fields[i].offset * 8 != bits) { emit(t, 0); emit_length(t, fields[i].offset * 8 - bits); }
        emit_name(t, fields[i].name); emit_length(t, fields[i].bits);
        bits = fields[i].offset * 8 + fields[i].bits;
    }
    package_length = t->size - at;
    t->bytes[at] = 0x40 | (package_length & 15); t->bytes[at + 1] = package_length >> 4;
}
static void table_end(struct table_data *t) {
    u32 i; u8 sum = 0;
    memcpy(t->bytes + 4, &t->size, 4); t->bytes[9] = 0;
    for(i = 0; i < t->size; i++) sum += t->bytes[i];
    t->bytes[9] = -sum;
}
static void setup(void) {
    static const struct field_data pnva[] = {
        { "PCHS", 0, 16 }, { "SBRG", 0x8b, 32 }, { "IM01", 0x11a, 8 }, { "IC01", 0x127, 64 }
    };
    static const struct field_data gnvs[] = {
        { "FMH1", 0x243, 16 }, { "FML1", 0x245, 16 }, { "FMD1", 0x247, 16 }
    };
    static const struct field_data lfcn[] = { { "TPTY", 0x84, 8 } };
    static const struct field_data sanv[] = { { "M32B", 0xf2, 32 }, { "M32L", 0xf6, 32 } };
    static const struct field_data partial[] = { { "M32B", 0xf2, 32 } };
    u32 i;
    check(!critical && !live, "prior fixture leaked no critical section or heap allocation");
    memset(&tp, 0, sizeof(tp)); memset(&bootinfo, 0, sizeof(bootinfo)); memset(&acpi, 0, sizeof(acpi));
    memset(&host, 0, sizeof(host)); memset(&controller, 0, sizeof(controller)); memset(&peer, 0, sizeof(peer));
    memset(peer_config, 0, sizeof(peer_config)); memset(physical, 0, sizeof(physical));
    maps = requests = 0; fail_map = false;
    bootinfo.valid = bootinfo.laptop = acpi.present = true; memcpy(acpi.oem, "LENOVO", 7);
    bootinfo.nregions = 5;
    for(i = 0; i < 5; i++) {
        bootinfo.regions[i].base = bases[i]; bootinfo.regions[i].len = 4096;
        bootinfo.regions[i].type = i == 2 ? 2 : 4;
    }
    host.vendor = controller.vendor = 0x8086;
    host.device = 0x8a02; controller.device = 0x34e9;
    host.subsys_vendor = controller.subsys_vendor = 0x17aa;
    host.subsys_id = 0x3808; controller.subsys_id = 0x3818;
    host.next = &controller; pci_devices = &host; tp.pci = &controller;
    tp.window_base = 0x65000000; tp.window_size = 0x01000000;
    put(0, 0, 2, 2); put(0, 0x8b, 0xfd000000, 4); put(0, 0x11a, 1, 1); put(0, 0x127, 0xa9000, 8);
    put(1, 0x243, 110, 2); put(1, 0x245, 207, 2); put(1, 0x247, 30, 2);
    put(2, 0x84, 1, 1); put(3, 0xf2, 0x65000000, 4); put(3, 0xf6, 0x01000000, 4);
    put(4, 0xf2, 0x71000000, 4);
    table_begin(&dsdt, "DSDT");
    table_region(&dsdt, "PNVA", bases[0], 0x234, pnva, 4);
    table_region(&dsdt, "GNVS", bases[1], 0x7d4, gnvs, 3);
    table_region(&dsdt, "LFCN", bases[2], 0xfd, lfcn, 1); table_end(&dsdt);
    table_begin(&ssdt[0], "SSDT"); table_region(&ssdt[0], "SANV", bases[3], 0x1f4, sanv, 2); table_end(&ssdt[0]);
    table_begin(&ssdt[1], "SSDT"); table_region(&ssdt[1], "SANV", bases[4], 0x1f4, partial, 1); table_end(&ssdt[1]);
    ssdt_count = 1;
}

static void refused(const struct aml_field *f, u32 width, bool lfcn, const char *why) {
    unsigned before = maps; u64 value = 0;
    check(!firmware_read(f, width, lfcn, &value), why);
    check(maps == before && !critical, "invalid firmware ranges never map memory");
}
static void test_firmware(void) {
    struct aml_field f = { 0x200000, 0x234, 0, 16 };
    u64 value;
    setup();
    check(firmware_read(&f, 16, false, &value) && value == 2, "validated NVS field is copied");
    refused(&f, 8, false, "width mismatch refused");
    f.bit_offset = 1; refused(&f, 16, false, "unaligned bit field refused"); f.bit_offset = 0;
    f.region_length = 1; refused(&f, 16, false, "field beyond region refused"); f.region_length = 0x234;
    bootinfo.regions[0].type = 1; refused(&f, 16, false, "ordinary RAM refused");
    bootinfo.regions[0].type = 2; refused(&f, 16, false, "reserved memory needs LFCN exception");
    bootinfo.regions[0].type = 4;
    bootinfo.regions[4].base = 0x200001; bootinfo.regions[4].len = 1; bootinfo.regions[4].type = 2;
    refused(&f, 16, false, "overlapping non-NVS entry defeats broad NVS entry");
    setup(); f = (struct aml_field){ 0x220000, 0xfd, 0x84 * 8, 8 };
    check(firmware_read(&f, 8, true, &value) && value == 1, "whole reserved LFCN region is admissible");
    bootinfo.regions[2].len = 0x85; refused(&f, 8, true, "containing TPTY byte alone does not contain LFCN");
    bootinfo.regions[2].len = 4096;
    tp.window_base = 0x220090; refused(&f, 8, true, "LFCN end may not cross the PCI aperture");
    tp.window_base = 0x65000000;
    bootinfo.nmods = 1; bootinfo.mods[0].start = 0x220010; bootinfo.mods[0].end = 0x220020;
    refused(&f, 8, true, "module outside TPTY but inside LFCN rejects the region");
    bootinfo.nmods = 0; bootinfo.fb.present = true; bootinfo.fb.addr = 0x220010;
    bootinfo.fb.pitch = 4; bootinfo.fb.height = 1;
    refused(&f, 8, true, "framebuffer outside TPTY but inside LFCN rejects the region");
    bootinfo.fb.present = false; f.address = 0x90000;
    refused(&f, 8, true, "low reserved memory is not a LFCN exception");
    f.address = 0xfffffffffffffff0ULL; refused(&f, 8, true, "wrapped field address refused");
    f.address = 0x220000; fail_map = true;
    check(!firmware_read(&f, 8, true, &value) && !critical, "mapping failure releases the critical section");
}

static void test_aperture(void) {
    setup();
    check(aperture_free(0x65000000), "unused address inside producer window is admissible");
    check(!aperture_free(0x64000000) && !aperture_free(0x65fff800), "entire BAR must fit producer window");
    bootinfo.regions[4] = (struct mb_region){ 0x65000000, 4096, 1 };
    check(!aperture_free(0x65000000), "PCI BAR cannot cover RAM");
    bootinfo.regions[4].type = 4; check(!aperture_free(0x65000000), "PCI BAR cannot cover NVS");
    bootinfo.regions[4].type = 2; check(aperture_free(0x65000000), "reserved PCI aperture permits allocation");
    bootinfo.nmods = 1; bootinfo.mods[0].start = 0x65000100; bootinfo.mods[0].end = 0x65000200;
    check(!aperture_free(0x65000000), "module overlap blocks the PCI BAR"); bootinfo.nmods = 0;
    bootinfo.fb.present = true; bootinfo.fb.addr = 0x65000000; bootinfo.fb.pitch = 4096; bootinfo.fb.height = 1;
    check(!aperture_free(0x65000000), "framebuffer overlap blocks the PCI BAR"); bootinfo.fb.present = false;
    controller.next = &peer; peer.bar[0] = 0x65000000; peer.bar_size[0] = 4096;
    check(!aperture_free(0x65000000) && aperture_free(0x65001000), "assigned BAR blocks only overlapping candidate");
    peer.bar_size[0] = 0;
    check(!aperture_free(0x65001000), "unknown BAR size below candidate conservatively blocks allocation");
    peer.bar[0] = 0x65001000;
    check(aperture_free(0x65000000), "unknown BAR entirely above candidate cannot overlap it");
    peer.bar[0] = 0x65000000; peer.bar_io[0] = true;
    check(aperture_free(0x65000000), "I/O BAR does not occupy memory aperture");
    peer.bar_io[0] = false; peer.bar_hi[0] = 1;
    check(aperture_free(0x65000000), "64-bit BAR above 4GiB does not block lower aperture");
    memset(&peer, 0, sizeof(peer)); peer.header_type = PCI_HEADER_TYPE_BRIDGE;
    peer_config[0x20 / 4] = 0x65006500;
    check(!aperture_free(0x65000000), "bridge memory aperture blocks allocation");
    peer_config[0x20 / 4] = 0x0000fff0; peer_config[0x24 / 4] = 0x65016501;
    check(!aperture_free(0x65000000), "bridge prefetchable aperture blocks allocation");
    peer_config[0x28 / 4] = peer_config[0x2c / 4] = 1;
    check(aperture_free(0x65000000), "64-bit bridge prefetch aperture above 4GiB is separate");
    controller.next = NULL; controller.bar[0] = 0x65000000; controller.bar_size[0] = 4096;
    check(aperture_free(0x65000000), "controller may retain its own BAR0");
    controller.bar[2] = 0x65000000; controller.bar_size[2] = 4096;
    check(!aperture_free(0x65000000), "controller's other BAR cannot overlap BAR0");
}

static void test_discover(void) {
    setup();
    check(discover() && tp.address == 0x15 && tp.descriptor_reg == 1 && tp.hcnt == 110 && tp.lcnt == 207 &&
          tp.hold == 30 && tp.gpio_address == 0xfd6e0780, "actual discovery consumes parsed firmware values");
    check(!live && !critical, "successful discovery frees all copied AML tables");
    setup(); ssdt_count = 2;
    check(discover() && tp.window_base == 0x65000000 && tp.window_size == 0x01000000,
          "later M32B-only SSDT cannot replace half of a paired window");
    setup(); ssdt[1] = ssdt[0]; ssdt_count = 2;
    check(!discover(), "duplicate complete PCI producer windows are rejected");
    setup(); host.subsys_id ^= 1;
    check(!discover() && !requests, "unmatched hardware cannot read firmware");
    setup(); bootinfo.laptop = false;
    check(!discover() && !requests, "emulator profile cannot read firmware");
    setup(); put(0, 0x11a, 2, 1);
    check(!discover(), "unsupported I2C mode rejected");
    setup(); put(1, 0x247, 207, 2);
    check(!discover(), "SDA hold must be shorter than the clock low count");
    setup(); put(2, 0x84, 2, 1);
    check(discover() && tp.address == 0x2c && tp.descriptor_reg == 0x20, "Synaptics variant uses its AML address and register");
    setup(); put(2, 0x84, 5, 1);
    check(discover() && tp.address == 0x38 && tp.descriptor_reg == 1, "FocalTech variant uses its AML address and register");
    setup(); put(2, 0x84, 3, 1);
    check(!discover(), "unsupported touchpad variant rejected");
    setup(); bootinfo.regions[2].len = 0x85;
    check(!discover(), "discovery uses full-LFCN containment guard");
    check(!live && !critical, "failed discovery frees all copied AML tables");
}

static void test_snapshot(void) {
    setup();
    tp.report_descriptor = kalloc(3); tp.report_descriptor_len = 3;
    memcpy(tp.report_descriptor, "abc", 3);
    tp.hid.report_desc_len = I2CHID_MAX_REPORT_DESCRIPTOR;
    touchpad_descriptor_print();
    check(live == 1 && !critical, "descriptor snapshot uses allocation's captured length, not a newer HID length");
    tp.busy = true; touchpad_descriptor_print();
    check(live == 1 && !critical, "busy retry produces no descriptor snapshot");
    kfree(tp.report_descriptor); tp.report_descriptor = NULL;
}

/* A simulated I2C HID touchpad, transaction by transaction. Its report descriptor
   is the laptop's Synaptics 06cb:ce2d one; its HID descriptor registers are
   illustrative (the laptop's were not recorded). Like the laptop's device, it
   NACKs the opcode byte of GET_REPORT for the InputMode feature by default. */
enum { SIM_ADDRESS = 0x2c, SIM_HID = 0x20, SIM_REPORT = 0x21, SIM_INPUT = 0x22,
       SIM_OUTPUT = 0x23, SIM_COMMAND = 0x24, SIM_DATA = 0x25,
       GPIO_IDLE = 0x80100102, GPIO_ASSERTED = 0x80100100 };
static struct sim {
    const u8 *descriptor;
    u_int descriptor_len, mode_report;  /* mode_report: 1 InputMode byte, or 2 with an index */
    bool nack_get, nack_set, keep_mode; /* keep_mode: a reset leaves InputMode alone */
    bool retire;                        /* a NACK leaves the engine down (cleanup failed) */
    u8 queue[8][64];
    u_int lengths[8], head, queued, fail_reads;
    u_int power_on, resets, gets, sets, reads, set_len, inits, ncommands;
    u8 set[32], mode, index, commands[16];
    u32 gpio[2];
} sim;
struct sent { unsigned source; int dx, dy; unsigned buttons; };
static struct sent events[16];
static unsigned nevents, sleeps, devices_on, devices_off;

static void sim_line(void) { sim.gpio[0] = sim.queued ? GPIO_ASSERTED : GPIO_IDLE; }
static void sim_queue(const u8 *report, u_int length) {
    u_int slot = (sim.head + sim.queued) % 8;
    check(sim.queued < 8 && length <= 64, "simulated input queue holds the report");
    memset(sim.queue[slot], 0, 64); memcpy(sim.queue[slot], report, length);
    sim.lengths[slot] = length; sim.queued++; sim_line();
}
static enum i2cdw_error sim_nack(struct i2cdw *d, u_int sent) {
    /* TXDATA_NOACK; the flush count is what the laptop reported for GET_REPORT. */
    d->last.error = I2CDW_ABORT; d->last.abort_source = 0x03000008;
    d->last.tx_queued = sent; d->last.rx_done = 0;
    d->last.recovered = d->ready = !sim.retire;
    return I2CDW_ABORT;
}
enum i2cdw_error i2cdw_init(struct i2cdw *d, const struct i2cdw_io *io, const struct i2cdw_config *config) {
    sim.inits++;
    d->io = *io; d->config = *config; d->ready = true;
    memset(&d->last, 0, sizeof(d->last));
    return I2CDW_OK;
}
static u16 le(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
enum i2cdw_error i2cdw_xfer(struct i2cdw *d, u16 address, const u8 *tx, u_int txlen, u8 *rx, u_int rxlen) {
    u8 hid[30] = { 30, 0, 0, 1, 0, 0, SIM_REPORT, 0, SIM_INPUT, 0, 64, 0,
                   SIM_OUTPUT, 0, 32, 0, SIM_COMMAND, 0, SIM_DATA, 0,
                   0xcb, 0x06, 0x2d, 0xce, 0x02, 0x01, 0, 0, 0, 0 };
    u_int type, id, opcode;
    hid[4] = (u8)sim.descriptor_len; hid[5] = (u8)(sim.descriptor_len >> 8);
    memset(&d->last, 0, sizeof(d->last));
    check(!critical, "no bus transfer inside a critical section");
    if(!d->ready) return d->last.error = I2CDW_NOT_READY;
    if(address != SIM_ADDRESS) return sim_nack(d, 0);
    if(!txlen && rxlen) {                       /* the input register */
        sim.reads++;
        if(sim.fail_reads) { sim.fail_reads--; return sim_nack(d, 0); }
        memset(rx, 0, rxlen);
        if(sim.queued) {
            memcpy(rx, sim.queue[sim.head], rxlen < 64 ? rxlen : 64);
            sim.head = (sim.head + 1) % 8; sim.queued--; sim_line();
        }
        d->last.rx_done = rxlen;
        return I2CDW_OK;
    }
    if(txlen == 2 && rxlen) {                   /* a register read */
        if(le(tx) == SIM_HID && rxlen == 30) memcpy(rx, hid, 30);
        else if(le(tx) == SIM_REPORT && rxlen == sim.descriptor_len) memcpy(rx, sim.descriptor, rxlen);
        else return sim_nack(d, 2);
        d->last.tx_queued = 2; d->last.rx_done = rxlen;
        return I2CDW_OK;
    }
    if(txlen < 4 || le(tx) != SIM_COMMAND) return sim_nack(d, 1);
    type = (tx[2] >> 4) & 3; id = tx[2] & 15; opcode = tx[3] & 15;
    if(sim.ncommands < sizeof(sim.commands)) sim.commands[sim.ncommands++] = (u8)opcode;
    if(opcode == 8 && txlen == 4 && !rxlen) {
        check(tx[2] == 0, "SET_POWER always asks for ON (state 0)");
        sim.power_on++;
    }
    else if(opcode == 1 && txlen == 4 && !rxlen) {
        static const u8 reset[2] = { 0, 0 };
        sim.resets++; sim.queued = 0; sim_queue(reset, 2);
        if(!sim.keep_mode) sim.mode = 0;   /* not kept across a host reset (Microsoft) */
    } else if(opcode == 2 && txlen == 6 && le(tx + 4) == SIM_DATA && type == 3 && id == 4) {
        sim.gets++;
        if(sim.nack_get) return sim_nack(d, 6);
        check(rxlen == sim.mode_report + 3, "GET_REPORT reads the length, the ID and the report");
        memset(rx, 0, rxlen);
        rx[0] = (u8)(sim.mode_report + 3); rx[2] = 4; rx[3] = sim.mode;
        if(sim.mode_report == 2) rx[4] = sim.index;
    } else if(opcode == 3 && txlen >= 9 && le(tx + 4) == SIM_DATA && type == 3 && id == 4) {
        sim.sets++;
        if(sim.nack_set) return sim_nack(d, txlen);
        check(txlen <= sizeof(sim.set) && le(tx + 6) == txlen - 6 && tx[8] == 4,
              "SET_REPORT carries its own length and the report ID");
        sim.set_len = txlen; memcpy(sim.set, tx, txlen);
        sim.mode = tx[9];
        if(sim.mode_report == 2 && txlen > 10) sim.index = tx[10];
    } else return sim_nack(d, 4);
    d->last.tx_queued = txlen; d->last.rx_done = rxlen;
    return I2CDW_OK;
}
void sleep_ms(u_long ms) { (void)ms; sleeps++; }
void pointer_report(unsigned source, int dx, int dy, int wheel, unsigned buttons) {
    (void)wheel;
    if(nevents < 16) events[nevents++] = (struct sent){ source, dx, dy, buttons };
}
void pointer_device(unsigned source, bool present) {
    check(source == POINTER_I2C, "touchpad registers only its own pointer source");
    if(present) devices_on++; else devices_off++;
}

/* A small descriptor: a mouse in report 2 and InputMode beside a Device Index in report 4. */
static const u8 shared_mode[] = {
    0x05,1, 0x09,2, 0xa1,1, 0x85,2, 0x09,1, 0xa1,0, 0x05,9, 0x19,1, 0x29,2,
    0x15,0, 0x25,1, 0x75,1, 0x95,2, 0x81,2, 0x95,6, 0x81,1,
    0x05,1, 0x09,0x30, 0x09,0x31, 0x15,0x81, 0x25,0x7f, 0x75,8, 0x95,2, 0x81,6, 0xc0, 0xc0,
    0x05,0x0d, 0x09,0x0e, 0xa1,1, 0x85,4, 0x09,0x52, 0x09,0x53, 0x15,0, 0x25,10,
    0x75,8, 0x95,2, 0xb1,2, 0xc0
};
static void hid_setup(const u8 *descriptor, u_int length, u_int mode_report) {
    setup();
    memset(&sim, 0, sizeof(sim));
    sim.descriptor = descriptor; sim.descriptor_len = length; sim.mode_report = mode_report;
    sim.nack_get = true; sim_line();
    tp.address = SIM_ADDRESS; tp.descriptor_reg = SIM_HID; tp.gpio = sim.gpio; tp.bus.ready = true;
    nevents = sleeps = devices_on = devices_off = 0;
}
static void hid_done(void) {
    if(tp.report_descriptor) { kfree(tp.report_descriptor); tp.report_descriptor = NULL; }
    tp.gpio = NULL;
}

static void test_hid_start(void) {
    static const u8 set_mouse[10] = { SIM_COMMAND, 0, 0x34, 0x03, SIM_DATA, 0, 4, 0, 4, 0 };
    static const u8 order[4] = { 8, 1, 8, 3 };  /* power on, reset, power on, SET_REPORT */
    hid_setup(synaptics_ce2d_descriptor, sizeof(synaptics_ce2d_descriptor), 1);
    sim.keep_mode = true; sim.mode = 3;   /* so a mode of 0 afterwards proves the SET */
    check(hid_start(), "Synaptics 06cb:ce2d: start completes without ever sending GET_REPORT");
    check(!strcmp(tp.stage, "ready (I2C HID mouse reports)") && !strcmp(tp.mode, "mouse (InputMode 0 set)"),
          "Synaptics 06cb:ce2d: stage and mode report the mouse collection");
    check(sim.gets == 0 && sim.sets == 1 && sim.set_len == sizeof(set_mouse) &&
          !memcmp(sim.set, set_mouse, sizeof(set_mouse)) && sim.mode == 0,
          "Synaptics 06cb:ce2d: InputMode 0 sent as one SET_REPORT, never read");
    check(sim.power_on == 2 && sim.resets == 1 && !sim.queued && !tp.errors && !tp.mode_status.error &&
          sim.ncommands == sizeof(order) && !memcmp(sim.commands, order, sizeof(order)),
          "power on, reset, its response read, power on again, then the mode: in that order");
    check(tp.mode_line[0] == GPIO_IDLE && tp.mode_line[1] == GPIO_IDLE && !sim.inits,
          "the interrupt line around the mode transfer is recorded");
    check(tp.hid.vendor_id == 0x06cb && tp.hid.product_id == 0xce2d && tp.hid.command_reg == SIM_COMMAND &&
          tp.report_descriptor_len == sizeof(synaptics_ce2d_descriptor) &&
          !memcmp(tp.report_descriptor, synaptics_ce2d_descriptor, sizeof(synaptics_ce2d_descriptor)),
          "HID descriptor parsed and the report descriptor kept as read");
    check(tp.bus.config.timeout_us == 20000, "input transfers use the short timeout");
    hid_done();

    hid_setup(synaptics_ce2d_descriptor, sizeof(synaptics_ce2d_descriptor), 1);
    sim.nack_set = true;
    check(hid_start() && !strcmp(tp.mode, "device default (SET_REPORT refused)") &&
          tp.mode_status.error == I2CDW_ABORT && tp.mode_status.abort_source == 0x03000008 &&
          tp.mode_status.tx_queued == 10 && tp.mode_status.recovered && tp.errors == 1 && sim.gets == 0,
          "a refused SET_REPORT is recorded and does not stop the start");
    hid_done();

    /* The refusal's cleanup fails and leaves the engine down: the next
       transfer sets it up again instead of failing for good. */
    hid_setup(synaptics_ce2d_descriptor, sizeof(synaptics_ce2d_descriptor), 1);
    sim.nack_set = sim.retire = true;
    check(hid_start() && !tp.mode_status.recovered && !tp.bus.ready && !sim.inits,
          "a mode refusal whose cleanup failed still lets the start finish");
    {
        static const u8 move[6] = { 6, 0, 2, 0, 3, 4 };
        sim_queue(move, sizeof(move)); tp.ready = true; poll_input();
        check(sim.inits == 1 && tp.reinits == 1 && tp.bus.ready && tp.decoded == 1 && nevents == 1 &&
              events[0].dx == 3 && events[0].dy == 4, "the engine is set up again and input flows");
    }
    hid_done();

    hid_setup(shared_mode, sizeof(shared_mode), 2);
    check(hid_start() && !strcmp(tp.mode, "device default (GET_REPORT refused)") &&
          sim.gets == 1 && sim.sets == 0 && tp.mode_status.tx_queued == 6,
          "a shared mode report is never written without reading it first");
    hid_done();

    hid_setup(shared_mode, sizeof(shared_mode), 2);
    sim.nack_get = false; sim.mode = 3; sim.index = 7;
    check(hid_start() && sim.gets == 1 && sim.sets == 0 && !strcmp(tp.mode, "mouse (InputMode read: 0)"),
          "shared report already in mouse mode after the reset: read, not written");
    hid_done();
}

static void test_hid_shared_patch(void) {
    static const u8 set_mouse[11] = { SIM_COMMAND, 0, 0x34, 0x03, SIM_DATA, 0, 5, 0, 4, 0, 7 };
    hid_setup(shared_mode, sizeof(shared_mode), 2);
    sim.nack_get = false; sim.keep_mode = true; sim.mode = 3; sim.index = 7;
    check(hid_start() && !strcmp(tp.mode, "mouse (InputMode read, 0 set)") && sim.gets == 1 &&
          sim.sets == 1 && sim.set_len == sizeof(set_mouse) && !memcmp(sim.set, set_mouse, sizeof(set_mouse)) &&
          sim.mode == 0 && sim.index == 7,
          "shared report: read, InputMode patched to 0, the other field written back unchanged");
    hid_done();
}

static void test_input(void) {
    static const u8 move[6] = { 6, 0, 2, 1, 5, 0xfb }, lift[6] = { 6, 0, 2, 0, 0xfe, 3 };
    static const u8 contact[32] = { 32, 0, 3, 3 }, empty[2] = { 2, 0 }, reset[2] = { 0, 0 };
    static const u8 bad[6] = { 0, 1, 2, 0, 0, 0 };
    u_int i;
    hid_setup(synaptics_ce2d_descriptor, sizeof(synaptics_ce2d_descriptor), 1);
    check(hid_start(), "input fixture starts");
    tp.ready = true;
    i = sim.reads; poll_input();
    check(sim.reads == i, "an idle line is not read");
    sim_queue(move, sizeof(move)); poll_input();
    check(nevents == 1 && events[0].source == POINTER_I2C && events[0].dx == 5 && events[0].dy == -5 &&
          events[0].buttons == 1 && tp.decoded == 1 && tp.buttons == 1 && tp.last_len == 4,
          "a mouse report moves the pointer and holds the left button");
    sim_queue(contact, sizeof(contact)); poll_input();
    check(nevents == 1 && tp.ignored == 1 && tp.last_len == 30,
          "a touchpad-collection report is kept for diagnosis but never moves the pointer");
    for(i = 0; i < 5; i++) sim_queue(lift, sizeof(lift));
    poll_input();
    check(nevents == 1 + TOUCHPAD_DRAIN && sim.queued == 5 - TOUCHPAD_DRAIN && events[1].dx == -2 &&
          events[1].dy == 3 && !events[1].buttons && !tp.buttons,
          "one wake reads a bounded number of waiting reports and releases the button");
    poll_input();
    check(nevents == 6 && !sim.queued, "the next wake reads the rest");
    sim_queue(move, sizeof(move)); sim_queue(reset, sizeof(reset)); sim_queue(move, sizeof(move));
    poll_input();
    check(nevents == 7 && events[6].buttons == 1 && tp.buttons == 1 && tp.zero_length == 1 && tp.ready &&
          sim.queued == 1, "a zero-length read is counted and ends the wake; a held button stays held");
    poll_input();
    check(nevents == 8 && events[7].buttons == 1 && tp.ready, "input continues after a zero-length read");
    sim_queue(empty, sizeof(empty)); sim_queue(bad, sizeof(bad)); poll_input();
    check(nevents == 8 && sim.queued == 1 && !tp.errors, "an empty report ends the wake quietly");
    poll_input();
    check(nevents == 8 && tp.errors == 1 && tp.ready, "an impossible length counts as an error");
    i = sim.reads; sim.gpio[0] = GPIO_ASSERTED; poll_input();
    check(sim.reads == i + 1 && tp.zero_length == 2, "a line held low without data costs one read per wake");
    sim.queued = 0; sim_line();
    sim_queue(move, sizeof(move)); sim.fail_reads = 5;
    for(i = 0; i < 4; i++) poll_input();
    check(tp.ready && tp.consecutive_errors == 4, "four failed reads in a row are tolerated");
    poll_input();
    check(!tp.ready && !tp.buttons && devices_off == 1 && strstr(tp.stage, "touchpad up retries"),
          "a fifth failed read stops input and drops the touchpad's buttons");
    i = sim.reads; poll_input();
    check(sim.reads == i, "a stopped touchpad is not read again until restarted");
    hid_done();
}

static void test_start_requests(void) {
    setup();
    check(touchpad_start() == -1 && !tp.requested, "touchpad up elsewhere does nothing");
    tp.found = tp.thread = true;
    check(touchpad_start() == 1 && tp.requested, "touchpad up starts a stopped touchpad");
    tp.requested = false; tp.ready = true;
    check(touchpad_start() == 2 && tp.requested, "touchpad up restarts a working touchpad");
    tp.requested = false; tp.busy = true;
    check(touchpad_start() == 0 && !tp.requested, "touchpad up during a start leaves it alone");
}

int main(void) {
    test_firmware(); test_aperture(); test_discover(); test_snapshot();
    test_hid_start(); test_hid_shared_patch(); test_input(); test_start_requests();
    check(!critical && !live, "all platform fixtures finish without leaks");
    printf("TOUCHPAD PLATFORM UNIT TESTS %s: %u checks, %u failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
