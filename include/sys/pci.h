/*
SurfOS PCI Header
----------------------
File: pci.h Date: 7/1/04, rebuilt 10/2026 (roadmap D1)
----------------------
(C)2004 Brandon Burr

-Original by Martin McCormick
*/

#ifndef _SURFOS_PCI_H
#define _SURFOS_PCI_H

#include <surfos/types.h>

/* configuration space registers (type 0 header) */
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define  PCI_COMMAND_IO     0x1
#define  PCI_COMMAND_MEMORY 0x2
#define  PCI_COMMAND_MASTER 0x4
#define PCI_STATUS          0x06
#define PCI_REVISION_ID     0x08
#define PCI_CLASS_PROG      0x09
#define PCI_CLASS_SUB       0x0a
#define PCI_CLASS_BASE      0x0b
#define PCI_CACHE_LINE_SIZE 0x0c
#define PCI_LATENCY_TIMER   0x0d
#define PCI_HEADER_TYPE     0x0e
#define  PCI_HEADER_TYPE_NORMAL 0
#define  PCI_HEADER_TYPE_BRIDGE 1
#define  PCI_HEADER_TYPE_CARDBUS 2
#define PCI_BASE_ADDRESS_0  0x10
#define  PCI_BASE_ADDRESS_SPACE_IO 0x01
#define  PCI_BASE_ADDRESS_MEM_TYPE_MASK 0x06
#define  PCI_BASE_ADDRESS_MEM_TYPE_64   0x04
#define  PCI_BASE_ADDRESS_MEM_MASK (~0x0fUL)
#define  PCI_BASE_ADDRESS_IO_MASK  (~0x03UL)
#define PCI_SUBSYSTEM_VENDOR_ID 0x2c
#define PCI_SUBSYSTEM_ID    0x2e
#define PCI_INTERRUPT_LINE  0x3c
#define PCI_INTERRUPT_PIN   0x3d
/* type 1 (bridge) */
#define PCI_PRIMARY_BUS     0x18
#define PCI_SECONDARY_BUS   0x19

#define PCI_CLASS_BRIDGE    0x06
#define PCI_SUBCLASS_PCI_BRIDGE 0x04
#define PCI_CLASS_NETWORK   0x02
#define PCI_CLASS_STORAGE   0x01

struct pci_dev {
    u8 bus, slot, func;
    u16 vendor, device;
    u8 class_code, subclass, prog_if, revision, header_type;
    u8 irq, irq_pin;
    u32 bar[6];         /* base address with the type bits masked off */
    u32 bar_size[6];    /* 0 when the BAR is not implemented */
    bool bar_io[6];
    u16 subsys_vendor, subsys_id;
    struct pci_dev *next;
};

extern struct pci_dev *pci_devices;

/* configuration space */
u8 pci_read8(u8 bus, u8 slot, u8 func, u8 reg);
u16 pci_read16(u8 bus, u8 slot, u8 func, u8 reg);
u32 pci_read32(u8 bus, u8 slot, u8 func, u8 reg);
void pci_write8(u8 bus, u8 slot, u8 func, u8 reg, u8 val);
void pci_write16(u8 bus, u8 slot, u8 func, u8 reg, u16 val);
void pci_write32(u8 bus, u8 slot, u8 func, u8 reg, u32 val);
u8 pci_dev_read8(struct pci_dev *d, u8 reg);
u16 pci_dev_read16(struct pci_dev *d, u8 reg);
u32 pci_dev_read32(struct pci_dev *d, u8 reg);
void pci_dev_write8(struct pci_dev *d, u8 reg, u8 val);
void pci_dev_write16(struct pci_dev *d, u8 reg, u16 val);
void pci_dev_write32(struct pci_dev *d, u8 reg, u32 val);

/* devices */
int init_pci(void);                     /* 0 ok, 1 no PCI bus */
u_int pci_device_count(void);
struct pci_dev *pci_find_device(u16 vendor, u16 device, struct pci_dev *after);
struct pci_dev *pci_find_class(u8 class_code, u8 subclass, struct pci_dev *after);
void pci_enable_device(struct pci_dev *d);   /* turn on the decoders it uses and bus mastering */
void pci_print(void);                        /* lspci */

/* names from the 2004 PCIDATA.H table */
const char *pci_vendor_name(u16 vendor);
const char *pci_device_name(u16 vendor, u16 device, char *buf, size_t size);
const char *pci_class_name(u8 class_code, u8 subclass);

#endif
