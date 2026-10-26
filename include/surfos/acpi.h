/*
SurfOS ACPI Tables
----------------------
File: acpi.h    Date: 10/5/26 (USB boot)
----------------------
What the laptop profile needs from the firmware's ACPI tables, read once at boot:
the FADT (are there legacy devices or an 8042, the PM timer port, how to reset), the
MADT (local APIC address, whether there are 8259s, the I/O APICs and the ISA
interrupt overrides) and the HPET table. Every table is checked (signature, length,
checksum) before anything in it is used. No AML: SurfOS never switches the machine
into ACPI mode, so the firmware keeps handling power events itself.
10/25/26 (robustness, L20, L21): poweroff, the one exception. Soft off (S5) takes the
FADT's PM1 control blocks (or the sleep control register), \_S5_'s SLP_TYP values read
from the DSDT without running AML (driver/amlfield.c), and, when the machine is not in
ACPI mode yet, the switch through SMI_CMD, just before the sleep is entered. The legacy
profile reads the tables only then, and quietly: its boot stays as it was.
*/

#ifndef _SURFOS_ACPI_H
#define _SURFOS_ACPI_H

#include <surfos/types.h>

#define ACPI_MAX_TABLES  64
#define ACPI_MAX_IOAPICS 4
#define ACPI_MAX_ISOS    16

/* FADT IAPC_BOOT_ARCH */
#define ACPI_BOOT_LEGACY_DEVICES 0x0001
#define ACPI_BOOT_8042           0x0002
#define ACPI_BOOT_NO_VGA         0x0004
#define ACPI_BOOT_NO_CMOS_RTC    0x0020

/* FADT flags */
#define ACPI_FADT_TMR_VAL_EXT    (1 << 8)
#define ACPI_FADT_RESET_REG_SUP  (1 << 10)
#define ACPI_FADT_HW_REDUCED     (1 << 20)
#define ACPI_FADT_LOW_POWER_S0   (1 << 21)

/* MADT flags */
#define ACPI_MADT_PCAT_COMPAT    0x1     /* the machine also has dual 8259s */

/* generic address structure: space 0 = memory, 1 = I/O ports */
struct acpi_gas {
    u8 space, width, offset, access;
    u64 addr;
};

struct acpi_table_ref {
    char sig[5];
    u64 addr;
    u32 len;
    bool ok;                    /* checksum and length good */
};

struct acpi_info {
    bool present;               /* a valid root table was read */
    u8 revision;
    char oem[7];
    bool xsdt;                  /* the root is the XSDT (else the RSDT) */
    u64 root;
    u_int ntables, nskipped;
    struct acpi_table_ref tables[ACPI_MAX_TABLES];

    bool fadt;
    u64 dsdt_addr;             /* X_DSDT when nonzero, else DSDT, from a checked FADT */
    u8 fadt_revision;
    u16 boot_arch;
    u32 fadt_flags;
    u16 sci_irq;
    u16 pm_timer_port;          /* 0: none */
    bool pm_timer_32;
    struct acpi_gas reset_reg;
    u8 reset_value;
    bool reset_ok;
    u16 smi_cmd;                /* 10/25/26 (L21): the port that switches the machine to ACPI mode (0: none) */
    u8 acpi_enable;             /* the value written there */
    u16 pm1a_cnt, pm1b_cnt;     /* the PM1 control blocks (I/O ports; 0: none) */
    struct acpi_gas sleep_ctl;  /* hardware-reduced ACPI: the sleep control register */

    bool madt;
    u32 lapic_addr;
    u32 madt_flags;
    u_int ncpus, ncpus_enabled;
    u_int nioapics;
    struct { u8 id; u32 addr; u32 gsi_base; } ioapic[ACPI_MAX_IOAPICS];
    u_int nisos;
    struct { u8 bus, source; u32 gsi; u16 flags; } iso[ACPI_MAX_ISOS];

    bool hpet;
    u64 hpet_addr;
};

extern struct acpi_info acpi;

int init_acpi(void);            /* 0, or 1 when there are no usable tables */
void acpi_print(void);
u32 acpi_isa_gsi(u8 irq);       /* where ISA IRQ n arrives on the I/O APICs (interrupt overrides) */

/* 10/25/26 (robustness, L20, L21): soft off */
struct acpi_s5 {
    bool ok;                    /* all that S5 needs was found; else why says what is missing */
    const char *why;
    bool hw_reduced;            /* the sleep control register, not the PM1 control blocks */
    u8 typa, typb;              /* SLP_TYP for PM1a and PM1b, from \_S5_ in the DSDT */
};
int acpi_s5_find(struct acpi_s5 *s);        /* task context (it reads the DSDT); 0, or -1 and s->why */
int acpi_s5_enter(const struct acpi_s5 *s); /* interrupts off; it returns (-1) only if the machine is still on 3 s later */
void acpi_s5_print(const struct acpi_s5 *s);  /* what S5 would write, and where (poweroff -n) */

enum acpi_copy_result {
    ACPI_COPY_OK = 0,
    ACPI_COPY_REQUEST = -1,    /* unsupported table, range or arguments */
    ACPI_COPY_ABSENT = -2,
    ACPI_COPY_INVALID = -3,    /* bad/unmappable table or it changed while being copied */
    ACPI_COPY_NOMEM = -4
};
#define ACPI_AML_MAX_BYTES (1024U * 1024U)
#define ACPI_NVS_MAX_BYTES 256U
/* Task context. On success the caller owns *data and frees it with kfree().
   The returned bytes never alias the shared firmware mapping window. */
int acpi_aml_copy(const char *signature, u_int instance, u8 **data, u32 *length);
/* Read at most 256 bytes wholly within one loader-reported ACPI NVS range.
   Laptop profile and valid ACPI are required; overlapping non-NVS is refused.
   The caller supplies length bytes. Failure leaves the buffer unchanged. */
int acpi_nvs_copy(u64 address, u32 length, u8 *data);

#endif
