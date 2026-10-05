/*
SurfOS ACPI Tables
--------------------
File: acpi.c    Date: 10/5/26 (USB boot)
--------------------
See include/surfos/acpi.h. The root pointer comes from the boot loader (GRUB copies it
into the Multiboot 2 block on UEFI); on a BIOS machine it is looked for in the EBDA and
at 0xE0000-0xFFFFF. Tables are read one at a time through vmm_temp_map() and every
length is checked before it is used, so a broken table is listed and skipped.
*/

#include <surfos/types.h>
#include <surfos/acpi.h>
#include <surfos/multiboot.h>
#include <surfos/console.h>
#include <mm/memory.h>
#include <mm/paging.h>
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
    if(!addr || addr + 36 > 0x100000000ULL) return NULL;
    t = (const u8 *)vmm_temp_map(addr, 36);
    if(!t) return NULL;
    len = rd32(t + 4);
    *len_out = len;
    if(len < 36 || len > TMPMAP_END - TMPMAP_START - PAGE_SIZE || addr + len > 0x100000000ULL) return t;
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

/* the BIOS data area through a pointer GCC cannot see as a constant: it takes any
   constant address below 4 KB for a zero-sized object (-Warray-bounds) */
static u16 bda_word(u_long addr) {
    volatile u16 *p;
    asm("" : "=r"(p) : "0"(addr));
    return *p;
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

static void parse_fadt(const u8 *t, u32 len) {
    u32 pm = 0, pmlen = 0;
    acpi.fadt = true;
    acpi.fadt_revision = t[8];
    if(len >= 48) acpi.sci_irq = rd16(t + 46);
    if(len >= 80) pm = rd32(t + 76);
    if(len >= 92) pmlen = t[91];
    if(len >= 111) acpi.boot_arch = rd16(t + 109);
    if(len >= 116) acpi.fadt_flags = rd32(t + 112);
    if(len >= 129) {
        read_gas(t + 116, &acpi.reset_reg);
        acpi.reset_value = t[128];
        acpi.reset_ok = (acpi.fadt_flags & ACPI_FADT_RESET_REG_SUP) && acpi.reset_reg.addr &&
                        (acpi.reset_reg.space == 0 || acpi.reset_reg.space == 1);
    }
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

int init_acpi(void) {
    struct boot_acpi rsdp = bootinfo.acpi;
    u64 entries[ACPI_MAX_TABLES];
    u_int i, n, esize;
    const u8 *t;
    u32 len;
    bool ok;

    memset(&acpi, 0, sizeof(acpi));
    kprintf("\nACPI\n");
    if(!rsdp.present && !bootinfo.efi) find_rsdp_bios(&rsdp);
    if(!rsdp.present) {
        kprintf("*no ACPI root pointer\n");
        return 1;
    }
    acpi.revision = rsdp.revision;
    memcpy(acpi.oem, rsdp.oem, sizeof(acpi.oem));

    acpi.xsdt = rsdp.revision >= 2 && rsdp.xsdt && rsdp.xsdt < 0x100000000ULL;
    acpi.root = acpi.xsdt ? rsdp.xsdt : rsdp.rsdt;
    t = map_table(acpi.root, &len, &ok);
    if(!ok || memcmp(t, acpi.xsdt ? "XSDT" : "RSDT", 4)) {
        if(!acpi.xsdt || !rsdp.rsdt) {
            kprintf("*the root table at 0x%llx is not valid\n", acpi.root);
            return 1;
        }
        acpi.xsdt = false;                            /* a bad XSDT: the RSDT lists the same tables */
        acpi.root = rsdp.rsdt;
        t = map_table(acpi.root, &len, &ok);
        if(!ok || memcmp(t, "RSDT", 4)) {
            kprintf("*no valid XSDT or RSDT\n");
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

u32 acpi_isa_gsi(u8 irq) {
    u_int i;
    for(i = 0; i < acpi.nisos; i++) if(acpi.iso[i].bus == 0 && acpi.iso[i].source == irq) return acpi.iso[i].gsi;
    return irq;
}

void acpi_print(void) {
    u_int i;
    if(!acpi.present) {
        printf("    no ACPI tables were read (they are read with profile=laptop)\n");
        return;
    }
    printf("    ACPI %s, OEM \"%s\", %s at 0x%llx\n", acpi.revision >= 2 ? "2.0+" : "1.0", acpi.oem,
           acpi.xsdt ? "XSDT" : "RSDT", acpi.root);
    for(i = 0; i < acpi.ntables; i++)
        printf("    %-4s 0x%09llx %6u bytes%s\n", acpi.tables[i].sig, acpi.tables[i].addr, acpi.tables[i].len,
               acpi.tables[i].ok ? "" : "  (failed its checks, not used)");
    if(acpi.nskipped) printf("    %u more tables not listed\n", acpi.nskipped);
    if(acpi.fadt) {
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
