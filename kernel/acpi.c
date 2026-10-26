/*
SurfOS ACPI Tables
--------------------
File: acpi.c    Date: 10/5/26 (USB boot)
--------------------
See include/surfos/acpi.h. The root pointer comes from the boot loader (GRUB copies it
into the Multiboot 2 block on UEFI); on a BIOS machine it is looked for in the EBDA and
at 0xE0000-0xFFFFF. Tables are read one at a time through vmm_temp_map() and every
length is checked before it is used, so a broken table is listed and skipped.
10/25/26 (robustness, L20, L21): soft off (S5) for poweroff, from the FADT and \_S5_.
10/26/26 (L21b): and before it, the GPEs disabled and the PM1 and GPE status cleared.
*/

#include <asm/io.h>
#include <surfos/types.h>
#include <surfos/acpi.h>
#include <surfos/multiboot.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/irq.h>
#include <surfos/timer.h>
#include <sys/amlfield.h>
#include <mm/memory.h>
#include <mm/paging.h>
#include <mm/kalloc.h>
#include <blibc_common.h>

struct acpi_info acpi;

static u16 rd16(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }
static u32 rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static u64 rd64(const u8 *p) { u64 v; memcpy(&v, p, 8); return v; }

static u8 sum8(const u8 *p, u32 n) {
    u8 s = 0;
    while(n--) s += *p++;
    return s;
}

static void read_gas(const u8 *p, struct acpi_gas *g) {
    g->space = p[0];
    g->width = p[1];
    g->offset = p[2];
    g->access = p[3];
    g->addr = rd64(p + 4);
}

/* a whole table, checked; the pointer is good until the next call */
static const u8 *map_table(u64 addr, u32 *len_out, bool *ok) {
    const u8 *t;
    u32 len;
    *ok = false;
    *len_out = 0;
    if(!addr || addr > 0x100000000ULL - 36) return NULL;
    t = (const u8 *)vmm_temp_map(addr, 36);
    if(!t) return NULL;
    len = rd32(t + 4);
    *len_out = len;
    if(len < 36 || len > TMPMAP_END - TMPMAP_START - (addr & (PAGE_SIZE - 1)) ||
       addr + len > 0x100000000ULL) return t;
    t = (const u8 *)vmm_temp_map(addr, len);
    if(t && !sum8(t, len)) *ok = true;
    return t;
}

/**** the root pointer on a BIOS machine ****/

static bool rsdp_at(const u8 *p, struct boot_acpi *a) {
    if(memcmp(p, "RSD PTR ", 8) || sum8(p, 20)) return false;
    memset(a, 0, sizeof(*a));
    a->revision = p[15];
    memcpy(a->oem, p + 9, 6);
    a->rsdt = rd32(p + 16);
    if(a->revision >= 2 && rd32(p + 20) >= 36 && rd32(p + 20) <= 64 && !sum8(p, rd32(p + 20))) a->xsdt = rd64(p + 24);
    a->present = true;
    return true;
}

/* the BIOS data area, through the firmware window: page 0 is unmapped once the drivers
   have read it (kernel/main.c), and since 10/25/26 poweroff may read the tables later */
static u16 bda_word(u_long addr) {
    const u8 *p = (const u8 *)vmm_temp_map(addr, 2);
    return p ? rd16(p) : 0;
}

static bool find_rsdp_bios(struct boot_acpi *a) {
    u_long ebda = (u_long)bda_word(0x40E) << 4, p;
    if(ebda >= 0x80000 && ebda < 0xA0000) {
        for(p = ebda; p < ebda + 1024; p += 16) if(rsdp_at((const u8 *)p, a)) return true;
    }
    for(p = 0xE0000; p < 0x100000; p += 16) if(rsdp_at((const u8 *)p, a)) return true;
    return false;
}

/**** the three tables the laptop profile uses ****/

/* 10/26/26 (L21b): a block of n bytes at an I/O port, all of it below 0x10000; else 0 */
static u16 io_block(u64 port, u32 n) {
    return port && port + n <= 0x10000 ? (u16)port : 0;
}

static void parse_fadt(const u8 *t, u32 len) {
    u32 pm = 0, pmlen = 0, g;
    acpi.fadt = true;
    acpi.fadt_revision = t[8];
    if(len >= 44) acpi.dsdt_addr = rd32(t + 40);
    if(len >= 148 && rd64(t + 140)) acpi.dsdt_addr = rd64(t + 140);
    if(len >= 48) acpi.sci_irq = rd16(t + 46);
    if(len >= 80) pm = rd32(t + 76);
    if(len >= 92) pmlen = t[91];
    if(len >= 111) acpi.boot_arch = rd16(t + 109);
    if(len >= 116) acpi.fadt_flags = rd32(t + 112);
    if(len >= 129) {
        read_gas(t + 116, &acpi.reset_reg);
        acpi.reset_value = t[128];
        acpi.reset_ok = (acpi.fadt_flags & ACPI_FADT_RESET_REG_SUP) && acpi.reset_reg.addr &&
                        (acpi.reset_reg.space == 0 || (acpi.reset_reg.space == 1 && acpi.reset_reg.addr < 0x10000));
                                                        /* 10/25/26 (the review of B4): a port is 16 bits */
    }
    /* 10/25/26 (L21): soft off's registers: SMI_CMD and ACPI_ENABLE, the PM1 control blocks (their extended forms when
       those name I/O ports), PM1_CNT_LEN at least 2, and the sleep control register of hardware-reduced ACPI */
    if(len >= 53 && rd32(t + 48) < 0x10000) {
        acpi.smi_cmd = (u16)rd32(t + 48);
        acpi.acpi_enable = t[52];
    }
    if(len >= 90 && t[89] >= 2) {
        if(rd32(t + 64) < 0x10000) acpi.pm1a_cnt = (u16)rd32(t + 64);
        if(rd32(t + 68) < 0x10000) acpi.pm1b_cnt = (u16)rd32(t + 68);
    }
    if(len >= 196) {
        struct acpi_gas x;
        read_gas(t + 172, &x);
        if(x.space == 1 && x.addr && x.addr < 0x10000) acpi.pm1a_cnt = (u16)x.addr;
        read_gas(t + 184, &x);
        if(x.space == 1 && x.addr && x.addr < 0x10000) acpi.pm1b_cnt = (u16)x.addr;
    }
    if(len >= 256) read_gas(t + 244, &acpi.sleep_ctl);
    /* 10/26/26 (L21b): the PM1 event blocks (PM1_EVT_LEN bytes: the status half, then the enable half) and the two
       GPE blocks (status bytes, then as many enable bytes), whose status soft off clears first, as ACPICA does; their
       extended forms when those name I/O ports. Each is used only when it is a port with all of it below 0x10000,
       PM1_EVT_LEN at least 4 and a GPE block's length even; the sleep status register of hardware-reduced ACPI */
    if(len >= 89 && t[88] >= 4) acpi.pm1_evt_len = t[88];
    if(len >= 64 && acpi.pm1_evt_len) {
        acpi.pm1a_evt = io_block(rd32(t + 56), acpi.pm1_evt_len);
        acpi.pm1b_evt = io_block(rd32(t + 60), acpi.pm1_evt_len);
    }
    for(g = 0; g < 2 && len >= 94; g++) {
        if(t[92 + g] < 2 || (t[92 + g] & 1)) continue;
        acpi.gpe_len[g] = t[92 + g];
        acpi.gpe_blk[g] = io_block(rd32(t + 80 + 4 * g), acpi.gpe_len[g]);
    }
    if(len >= 172 && acpi.pm1_evt_len) {
        struct acpi_gas x;
        read_gas(t + 148, &x);
        if(x.space == 1 && io_block(x.addr, acpi.pm1_evt_len)) acpi.pm1a_evt = (u16)x.addr;
        read_gas(t + 160, &x);
        if(x.space == 1 && io_block(x.addr, acpi.pm1_evt_len)) acpi.pm1b_evt = (u16)x.addr;
    }
    for(g = 0; g < 2 && len >= 244; g++) {
        struct acpi_gas x;
        read_gas(t + 220 + 12 * g, &x);
        if(acpi.gpe_len[g] && x.space == 1 && io_block(x.addr, acpi.gpe_len[g])) acpi.gpe_blk[g] = (u16)x.addr;
    }
    if(len >= 268) read_gas(t + 256, &acpi.sleep_sts);
    if(len >= 220) {                                  /* X_PM_TMR_BLK overrides PM_TMR_BLK when given */
        struct acpi_gas x;
        read_gas(t + 208, &x);
        if(x.space == 1 && x.addr && x.addr < 0x10000) { pm = (u32)x.addr; pmlen = 4; }
    }
    if(pm && pm < 0x10000 && pmlen == 4 && !(acpi.fadt_flags & ACPI_FADT_HW_REDUCED)) {
        acpi.pm_timer_port = (u16)pm;
        acpi.pm_timer_32 = (acpi.fadt_flags & ACPI_FADT_TMR_VAL_EXT) ? true : false;
    }
}

static void parse_madt(const u8 *t, u32 len) {
    u32 off, elen;
    acpi.madt = true;
    acpi.lapic_addr = rd32(t + 36);
    acpi.madt_flags = rd32(t + 40);
    for(off = 44; off + 2 <= len; off += elen) {
        u8 type = t[off];
        elen = t[off + 1];
        if(elen < 2 || off + elen > len) break;
        switch(type) {
        case 0:                                       /* processor local APIC */
            if(elen < 8) break;
            acpi.ncpus++;
            if(rd32(t + off + 4) & 1) acpi.ncpus_enabled++;
            break;
        case 1:                                       /* I/O APIC */
            if(elen < 12 || acpi.nioapics == ACPI_MAX_IOAPICS) break;
            acpi.ioapic[acpi.nioapics].id = t[off + 2];
            acpi.ioapic[acpi.nioapics].addr = rd32(t + off + 4);
            acpi.ioapic[acpi.nioapics].gsi_base = rd32(t + off + 8);
            acpi.nioapics++;
            break;
        case 2:                                       /* interrupt source override */
            if(elen < 10 || acpi.nisos == ACPI_MAX_ISOS) break;
            acpi.iso[acpi.nisos].bus = t[off + 2];
            acpi.iso[acpi.nisos].source = t[off + 3];
            acpi.iso[acpi.nisos].gsi = rd32(t + off + 4);
            acpi.iso[acpi.nisos].flags = rd16(t + off + 8);
            acpi.nisos++;
            break;
        case 5:                                       /* local APIC address override */
            if(elen >= 12 && rd64(t + off + 4) < 0x100000000ULL) acpi.lapic_addr = (u32)rd64(t + off + 4);
            break;
        case 9:                                       /* processor local x2APIC */
            if(elen < 16) break;
            acpi.ncpus++;
            if(rd32(t + off + 8) & 1) acpi.ncpus_enabled++;
            break;
        default:
            break;
        }
    }
}

static void parse_hpet(const u8 *t, u32 len) {
    struct acpi_gas g;
    if(len < 56) return;
    read_gas(t + 40, &g);
    if(g.space != 0 || !g.addr) return;
    acpi.hpet = true;
    acpi.hpet_addr = g.addr;
}

/* the tables, read once: at boot with profile=laptop (said on the console), or quietly when poweroff first needs
   them (10/25/26: the legacy profile's boot reads none) */
static bool acpi_read_done;

static int acpi_read(bool quiet) {
    struct boot_acpi rsdp = bootinfo.acpi;
    u64 entries[ACPI_MAX_TABLES];
    u_int i, n, esize;
    const u8 *t;
    u32 len;
    bool ok;

    acpi_read_done = true;
    memset(&acpi, 0, sizeof(acpi));
    if(!quiet) kprintf("\nACPI\n");
    if(!rsdp.present && !bootinfo.efi) find_rsdp_bios(&rsdp);
    if(!rsdp.present) {
        if(!quiet) kprintf("*no ACPI root pointer\n");
        return 1;
    }
    acpi.revision = rsdp.revision;
    memcpy(acpi.oem, rsdp.oem, sizeof(acpi.oem));

    acpi.xsdt = rsdp.revision >= 2 && rsdp.xsdt && rsdp.xsdt < 0x100000000ULL;
    acpi.root = acpi.xsdt ? rsdp.xsdt : rsdp.rsdt;
    t = map_table(acpi.root, &len, &ok);
    if(!ok || memcmp(t, acpi.xsdt ? "XSDT" : "RSDT", 4)) {
        if(!acpi.xsdt || !rsdp.rsdt) {
            if(!quiet) kprintf("*the root table at 0x%llx is not valid\n", acpi.root);
            return 1;
        }
        acpi.xsdt = false;                            /* a bad XSDT: the RSDT lists the same tables */
        acpi.root = rsdp.rsdt;
        t = map_table(acpi.root, &len, &ok);
        if(!ok || memcmp(t, "RSDT", 4)) {
            if(!quiet) kprintf("*no valid XSDT or RSDT\n");
            return 1;
        }
    }
    esize = acpi.xsdt ? 8 : 4;
    n = (len - 36) / esize;
    if(n > ACPI_MAX_TABLES) {
        acpi.nskipped = n - ACPI_MAX_TABLES;
        n = ACPI_MAX_TABLES;
    }
    for(i = 0; i < n; i++) entries[i] = acpi.xsdt ? rd64(t + 36 + i * 8) : rd32(t + 36 + i * 4);
    acpi.present = true;

    for(i = 0; i < n; i++) {
        struct acpi_table_ref *r = &acpi.tables[acpi.ntables++];
        r->addr = entries[i];
        t = map_table(entries[i], &len, &ok);
        r->len = len;
        r->ok = ok;
        if(!t) { strlcpy(r->sig, "????", sizeof(r->sig)); continue; }
        memcpy(r->sig, t, 4);
        r->sig[4] = 0;
        if(!ok) continue;
        if(!memcmp(t, "FACP", 4) && !acpi.fadt) parse_fadt(t, len);
        else if(!memcmp(t, "APIC", 4) && !acpi.madt) parse_madt(t, len);
        else if(!memcmp(t, "HPET", 4) && !acpi.hpet) parse_hpet(t, len);
    }

    if(quiet) return 0;
    kprintf("*%s at 0x%llx, OEM \"%s\", %u tables", acpi.xsdt ? "XSDT" : "RSDT", acpi.root, acpi.oem, acpi.ntables);
    for(i = 0; i < acpi.ntables; i++) kprintf("%s%s%s", i ? " " : ": ", acpi.tables[i].sig, acpi.tables[i].ok ? "" : "(bad)");
    kprintf("\n");
    if(acpi.fadt)
        kprintf("*FADT: boot flags 0x%x (8042 %s, legacy devices %s), PM timer %s0x%x, reset %s\n", acpi.boot_arch,
                (acpi.boot_arch & ACPI_BOOT_8042) ? "yes" : "not claimed",
                (acpi.boot_arch & ACPI_BOOT_LEGACY_DEVICES) ? "yes" : "not claimed",
                acpi.pm_timer_port ? (acpi.pm_timer_32 ? "32-bit at " : "24-bit at ") : "none ", acpi.pm_timer_port,
                acpi.reset_ok ? "register" : "no register");
    if(acpi.madt)
        kprintf("*MADT: local APIC 0x%08x, %s8259s, %u CPUs (%u enabled), %u I/O APICs, %u overrides\n",
                acpi.lapic_addr, (acpi.madt_flags & ACPI_MADT_PCAT_COMPAT) ? "" : "no ", acpi.ncpus,
                acpi.ncpus_enabled, acpi.nioapics, acpi.nisos);
    if(acpi.hpet) kprintf("*HPET at 0x%llx\n", acpi.hpet_addr);
    return 0;
}

int init_acpi(void) {
    return acpi_read(false);
}

u32 acpi_isa_gsi(u8 irq) {
    u_int i;
    for(i = 0; i < acpi.nisos; i++) if(acpi.iso[i].bus == 0 && acpi.iso[i].source == irq) return acpi.iso[i].gsi;
    return irq;
}

/* The window is shared by all firmware readers; no mapping pointer survives
   this bounded copy. A chunk is at most 64 KiB, including unaligned tables. */
static bool copy_firmware(u64 address, void *destination, u32 length) {
    const void *mapped;
    bool ok;
    kcritical_enter();
    mapped = vmm_temp_map(address, length);
    ok = mapped != NULL;
    if(ok) memcpy(destination, mapped, length);
    kcritical_leave();
    return ok;
}

/* Only these two firmware table types can cross the diagnostic interface.
   The boot parser's window limit does not limit this chunked export. Recheck
   the header after copying and checksum the complete owned image before use. */
int acpi_aml_copy(const char *signature, u_int instance, u8 **data, u32 *length) {
    u8 header[36], final_header[36], sum = 0;
    u8 *copy;
    u64 address = 0;
    u32 size, offset, chunk;
    u_int i, ordinal = 0;
    bool dsdt;
    if(data) *data = NULL;
    if(length) *length = 0;
    if(!data || !length || !signature) return ACPI_COPY_REQUEST;
    dsdt = !strcmp(signature, "DSDT");
    if((!dsdt && strcmp(signature, "SSDT")) || (dsdt && instance)) return ACPI_COPY_REQUEST;
    if(!acpi.present) return ACPI_COPY_ABSENT;
    if(dsdt) address = acpi.dsdt_addr;
    else for(i = 0; i < acpi.ntables; i++) {
        const struct acpi_table_ref *ref = &acpi.tables[i];
        if(strcmp(ref->sig, "SSDT")) continue;
        if(ordinal++ != instance) continue;
        /* ref->ok may be false solely because this table exceeds the boot
           parser's mapping window. This path validates all its bytes again. */
        address = ref->addr;
        break;
    }
    if(!address) return ACPI_COPY_ABSENT;
    if(address > 0x100000000ULL - sizeof(header) ||
       !copy_firmware(address, header, sizeof(header)) || memcmp(header, signature, 4))
        return ACPI_COPY_INVALID;
    size = rd32(header + 4);
    if(size < sizeof(header) || size > ACPI_AML_MAX_BYTES || address > 0x100000000ULL - size)
        return ACPI_COPY_INVALID;

    /* Allocation may page or schedule; no firmware view is held across it. */
    copy = kalloc(size);
    if(!copy) return ACPI_COPY_NOMEM;
    for(offset = 0; offset < size; offset += chunk) {
        chunk = size - offset;
        if(chunk > 16 * PAGE_SIZE) chunk = 16 * PAGE_SIZE;
        if(!copy_firmware(address + offset, copy + offset, chunk)) goto invalid;
        if(!offset && memcmp(copy, header, sizeof(header))) goto invalid;
        sum += sum8(copy + offset, chunk);
    }
    if(sum || !copy_firmware(address, final_header, sizeof(final_header)) ||
       memcmp(final_header, header, sizeof(header))) goto invalid;
    *data = copy;
    *length = size;
    return ACPI_COPY_OK;
invalid:
    kfree(copy);
    return ACPI_COPY_INVALID;
}

/* Only loader-owned NVS ranges are admissible. In particular a broad NVS
   entry cannot override a narrower reserved or usable-RAM entry. The boot
   memory map is immutable after initialization. No AML or hardware writes. */
int acpi_nvs_copy(u64 address, u32 length, u8 *data) {
    u64 end, region_end;
    u_int i;
    bool contained = false;
    if(!data || !length || length > ACPI_NVS_MAX_BYTES || address > 0x100000000ULL - length)
        return ACPI_COPY_REQUEST;
    if(!bootinfo.valid || !bootinfo.laptop || !acpi.present) return ACPI_COPY_ABSENT;
    if(bootinfo.nregions > MB_MAX_REGIONS) return ACPI_COPY_INVALID;
    end = address + length;
    for(i = 0; i < bootinfo.nregions; i++) {
        const struct mb_region *region = &bootinfo.regions[i];
        if(!region->len) continue;
        region_end = region->base + region->len;
        if(region_end < region->base) return ACPI_COPY_INVALID;
        if(region->type == 4) {
            if(address >= region->base && end <= region_end) contained = true;
        } else if(address < region_end && end > region->base) return ACPI_COPY_INVALID;
    }
    if(!contained) return ACPI_COPY_INVALID;
    return copy_firmware(address, data, length) ? ACPI_COPY_OK : ACPI_COPY_INVALID;
}

void acpi_print(void) {
    u_int i, ssdt = 0;
    if(!acpi.present) {
        printf("    no ACPI tables were read (they are read with profile=laptop)\n");
        return;
    }
    printf("    ACPI %s, OEM \"%s\", %s at 0x%llx\n", acpi.revision >= 2 ? "2.0+" : "1.0", acpi.oem,
           acpi.xsdt ? "XSDT" : "RSDT", acpi.root);
    for(i = 0; i < acpi.ntables; i++) {
        printf("    %-4s 0x%09llx %6u bytes%s", acpi.tables[i].sig, acpi.tables[i].addr, acpi.tables[i].len,
               acpi.tables[i].ok ? "" : "  (failed its checks, not used)");
        if(!strcmp(acpi.tables[i].sig, "SSDT")) printf("  instance %u", ssdt++);
        printf("\n");
    }
    if(acpi.nskipped) printf("    %u more tables not listed\n", acpi.nskipped);
    if(acpi.fadt) {
        if(acpi.dsdt_addr) printf("    DSDT 0x%09llx (from FADT; acpidump DSDT)\n", acpi.dsdt_addr);
        printf("    FADT rev %u: IAPC_BOOT_ARCH 0x%04x, flags 0x%08x, SCI IRQ %u\n", acpi.fadt_revision, acpi.boot_arch,
               acpi.fadt_flags, acpi.sci_irq);
        printf("      8042 %s, legacy devices %s, CMOS RTC %s, %s\n",
               (acpi.boot_arch & ACPI_BOOT_8042) ? "present" : "not claimed",
               (acpi.boot_arch & ACPI_BOOT_LEGACY_DEVICES) ? "present" : "not claimed",
               (acpi.boot_arch & ACPI_BOOT_NO_CMOS_RTC) ? "absent" : "present",
               (acpi.fadt_flags & ACPI_FADT_HW_REDUCED) ? "hardware-reduced ACPI" : "full ACPI hardware");
        if(acpi.pm_timer_port) printf("      PM timer at port 0x%x (%u-bit)\n", acpi.pm_timer_port, acpi.pm_timer_32 ? 32 : 24);
        if(acpi.reset_ok) printf("      reset register: %s 0x%llx, value 0x%02x\n", acpi.reset_reg.space ? "port" : "memory",
                                 acpi.reset_reg.addr, acpi.reset_value);
    }
    if(acpi.madt) {
        printf("    MADT: local APIC 0x%08x, %s, %u CPUs (%u enabled)\n", acpi.lapic_addr,
               (acpi.madt_flags & ACPI_MADT_PCAT_COMPAT) ? "8259s present" : "no 8259s", acpi.ncpus, acpi.ncpus_enabled);
        for(i = 0; i < acpi.nioapics; i++)
            printf("      I/O APIC %u at 0x%08x, GSI base %u\n", acpi.ioapic[i].id, acpi.ioapic[i].addr, acpi.ioapic[i].gsi_base);
        for(i = 0; i < acpi.nisos; i++)
            printf("      ISA IRQ %u -> GSI %u, flags 0x%x\n", acpi.iso[i].source, acpi.iso[i].gsi, acpi.iso[i].flags);
    }
    if(acpi.hpet) printf("    HPET at 0x%llx\n", acpi.hpet_addr);
}

/**** 10/25/26 (robustness, L20, L21): soft off ****/

#define PM1_SCI_EN   0x0001
#define PM1_SLP_TYP  0x1c00     /* bits 10-12 */
#define PM1_SLP_EN   0x2000
#define SLPCTL_SLP_EN 0x20      /* the sleep control register: SLP_TYP in bits 2-4, SLP_EN bit 5 */

/* Task context: the registers from the FADT, and \_S5_'s SLP_TYP values from the DSDT (read, never run). The legacy
   profile reads the tables the first time this is asked, quietly, with no other task using the firmware window. */
int acpi_s5_find(struct acpi_s5 *s) {
    u8 *dsdt;
    u32 len;
    int r;
    memset(s, 0, sizeof(*s));
    if(!acpi_read_done) {
        kcritical_enter();
        acpi_read(true);
        kcritical_leave();
    }
    if(!acpi.present) s->why = "no ACPI tables were found";
    else if(!acpi.fadt) s->why = "the ACPI tables have no FADT";
    else {
        s->hw_reduced = (acpi.fadt_flags & ACPI_FADT_HW_REDUCED) ? true : false;
        if(s->hw_reduced && !(acpi.sleep_ctl.addr && ((acpi.sleep_ctl.space == 0 && acpi.sleep_ctl.addr < 0x100000000ULL) ||
                                                      (acpi.sleep_ctl.space == 1 && acpi.sleep_ctl.addr < 0x10000))))
            s->why = "the FADT names no sleep control register SurfOS can write (hardware-reduced ACPI)";
        else if(!s->hw_reduced && !acpi.pm1a_cnt) s->why = "the FADT names no PM1 control block";
    }
    if(s->why) return -1;
    r = acpi_aml_copy("DSDT", 0, &dsdt, &len);
    if(r) {
        s->why = r == ACPI_COPY_NOMEM ? "there was no memory to read the DSDT" : "the DSDT could not be read";
        return -1;
    }
    r = aml_sleep_type(dsdt, len, "_S5_", &s->typa, &s->typb);
    kfree(dsdt);
    if(r) {
        s->why = r == AML_FIELD_NOT_FOUND ? "the DSDT declares no \\_S5_" :
                 r == AML_FIELD_UNSUPPORTED ? "the DSDT's \\_S5_ is not a package of numbers at the root, outside any If" :
                 "the DSDT's \\_S5_ could not be read";
        return -1;
    }
    /* 10/25/26 (the review of B4): SLP_TYP is 3 bits; a larger value is refused, never cut to 3 bits (0x0D would be 5,
       which is S3 on Intel's: the machine would suspend, not turn off) */
    if(s->typa > 7 || (!s->hw_reduced && acpi.pm1b_cnt && s->typb > 7)) {
        s->why = "\\_S5_'s SLP_TYP is out of range";
        return -1;
    }
    s->ok = true;
    return 0;
}

static void sleep_ctl_write(u8 value) {
    if(acpi.sleep_ctl.space == 1) outb((u_short)acpi.sleep_ctl.addr, value);
    else {
        volatile u8 *r = (volatile u8 *)ioremap((u_long)acpi.sleep_ctl.addr, 1, false);
        if(r) *r = value;
    }
}

/* 10/26/26 (L21b): hardware-reduced ACPI's sleep status register, where SurfOS can write it */
static bool sleep_sts_ok(void) {
    const struct acpi_gas *r = &acpi.sleep_sts;
    return r->addr && ((r->space == 0 && r->addr < 0x100000000ULL) || (r->space == 1 && r->addr < 0x10000));
}

/* 10/26/26 (L21b): ACPICA's preparation for a sleep (AcpiHwLegacySleep(), AcpiHwExtendedSleep()), for soft off: the
   GPEs disabled first, so that none wakes the machine straight up again, then the fixed status bits of PM1a and PM1b
   cleared (written as ones), WAK_STS among them, then each GPE status byte. Hardware-reduced ACPI: WAK_STS in the
   sleep status register (a port below 0x10000, or memory below 4 GB). The wakeup GPEs ACPICA then enables are
   unknown here (the DSDT is not run): a soft off by the power button alone. The blocks were checked by parse_fadt(). */
u_int acpi_s5_clear(acpi_write_fn out, void *arg) {
    u_int n = 0, g, i, half;
    if(acpi.fadt_flags & ACPI_FADT_HW_REDUCED) {
        if(!sleep_sts_ok()) return 0;
        out(arg, acpi.sleep_sts.space, acpi.sleep_sts.addr, 8, ACPI_SLEEP_WAK_STS);
        return 1;
    }
    for(g = 0; g < 2; g++)                                  /* each GPE enable byte: the second half of its block */
        for(half = acpi.gpe_blk[g] ? acpi.gpe_len[g] / 2 : 0, i = 0; i < half; i++, n++)
            out(arg, 1, acpi.gpe_blk[g] + half + i, 8, 0);
    if(acpi.pm1a_evt) {                                     /* the PM1 status: the first half of the event block */
        out(arg, 1, acpi.pm1a_evt, 16, ACPI_PM1_STS_FIXED);
        n++;
    }
    if(acpi.pm1b_evt) {
        out(arg, 1, acpi.pm1b_evt, 16, ACPI_PM1_STS_FIXED);
        n++;
    }
    for(g = 0; g < 2; g++)                                  /* each GPE status byte: the first half */
        for(half = acpi.gpe_blk[g] ? acpi.gpe_len[g] / 2 : 0, i = 0; i < half; i++, n++)
            out(arg, 1, acpi.gpe_blk[g] + i, 8, 0xFF);
    return n;
}

/* acpi_s5_clear()'s writes, made: a register in memory is mapped then, as the sleep control register is */
static void s5_write(void *arg, u8 space, u64 addr, u8 bits, u16 value) {
    (void)arg;
    if(space == 1 && bits == 16) outw((u_short)addr, value);
    else if(space == 1) outb((u_short)addr, (u8)value);
    else {
        volatile u8 *r = (volatile u8 *)ioremap((u_long)addr, 1, false);
        if(r) *r = (u8)value;
    }
}

/* Interrupts off, caches written back. Not yet in ACPI mode (SCI_EN clear): the switch through SMI_CMD, waited for
   300 ms at most. 10/26/26 (L21b): the GPEs disabled and the PM1 and GPE status cleared (acpi_s5_clear()). Then
   SLP_TYP into the PM1a and PM1b control blocks, and SLP_TYP with SLP_EN (as ACPICA does), or, after WAK_STS, the
   sleep control register's one write. The machine is off before the 3 s are up; if not, -1. */
int acpi_s5_enter(const struct acpi_s5 *s) {
    u16 a = 0, b = 0;
    u_long ms;
    if(!s->ok) return -1;
    irq_disable();
    asm volatile("wbinvd" ::: "memory");
    if(s->hw_reduced) {
        acpi_s5_clear(s5_write, NULL);
        sleep_ctl_write((u8)((s->typa << 2) | SLPCTL_SLP_EN));
    } else {
        if(!(inw(acpi.pm1a_cnt) & PM1_SCI_EN) && acpi.smi_cmd && acpi.acpi_enable) {
            outb(acpi.smi_cmd, acpi.acpi_enable);
            for(ms = 0; ms < 300 && !(inw(acpi.pm1a_cnt) & PM1_SCI_EN); ms++) mdelay(1);
        }
        acpi_s5_clear(s5_write, NULL);
        a = (inw(acpi.pm1a_cnt) & ~(PM1_SLP_TYP | PM1_SLP_EN)) | (s->typa << 10);     /* 0-7: acpi_s5_find() */
        if(acpi.pm1b_cnt) b = (inw(acpi.pm1b_cnt) & ~(PM1_SLP_TYP | PM1_SLP_EN)) | (s->typb << 10);
        outw(acpi.pm1a_cnt, a);
        if(acpi.pm1b_cnt) outw(acpi.pm1b_cnt, b);
        asm volatile("wbinvd" ::: "memory");
        outw(acpi.pm1a_cnt, a | PM1_SLP_EN);
        if(acpi.pm1b_cnt) outw(acpi.pm1b_cnt, b | PM1_SLP_EN);
    }
    for(ms = 0; ms < 3000; ms++) mdelay(1);
    return -1;
}

/* 10/26/26 (L21b): what acpi_s5_clear() would write first, said (poweroff -n) */
static void s5_print_clear(void) {
    u_int g, said = 0;
    printf("    before the sleep, as ACPICA does: ");
    if(acpi.fadt_flags & ACPI_FADT_HW_REDUCED) {
        if(sleep_sts_ok())
            printf("WAK_STS cleared in the sleep status register (%s 0x%llx)\n", acpi.sleep_sts.space ? "port" : "memory",
                   acpi.sleep_sts.addr);
        else printf("nothing (the FADT names no sleep status register SurfOS can write)\n");
        return;
    }
    for(g = 0; g < 2; g++) {
        if(!acpi.gpe_blk[g]) continue;
        printf("%sGPE%u at port 0x%x off and cleared (%u bytes each)", said++ ? "; " : "", g, acpi.gpe_blk[g],
               acpi.gpe_len[g] / 2);
    }
    if(acpi.pm1a_evt) {
        printf("%sthe PM1 status cleared at port 0x%x", said++ ? "; " : "", acpi.pm1a_evt);
        if(acpi.pm1b_evt) printf(" and 0x%x", acpi.pm1b_evt);
    }
    printf("%s\n", said ? "" : "nothing (the FADT names no PM1 event block or GPE block SurfOS can write)");
}

void acpi_s5_print(const struct acpi_s5 *s) {
    if(!s->ok) {
        printf("    soft off (ACPI S5): not possible, %s\n", s->why);
        return;
    }
    if(s->hw_reduced) {
        printf("    soft off (ACPI S5): SLP_TYP %u from \\_S5_, into the sleep control register (%s 0x%llx)\n", s->typa,
               acpi.sleep_ctl.space ? "port" : "memory", acpi.sleep_ctl.addr);
        s5_print_clear();
        return;
    }
    printf("    soft off (ACPI S5): SLP_TYPa %u and SLP_TYPb %u from \\_S5_, into PM1a at port 0x%x", s->typa,
           s->typb, acpi.pm1a_cnt);
    if(acpi.pm1b_cnt) printf(" and PM1b at port 0x%x", acpi.pm1b_cnt);
    else printf(" (no PM1b)");
    printf("\n    ACPI mode: %s\n", (inw(acpi.pm1a_cnt) & PM1_SCI_EN) ? "on already" :
           acpi.smi_cmd ? "off, switched on first (SMI_CMD)" : "off, and the FADT names no way to switch it on");
    if(!(inw(acpi.pm1a_cnt) & PM1_SCI_EN) && acpi.smi_cmd)
        printf("    SMI_CMD port 0x%x, value 0x%02x\n", acpi.smi_cmd, acpi.acpi_enable);
    s5_print_clear();
}
