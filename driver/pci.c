/*
SurfOS PCI Bus Driver
--------------------
File: pci.c Date: 7/1/04
--------------------
(C)2004 Brandon Burr
*/
//Rebuilt 9/28/2026 - not in the printout, see REBUILD-NOTES.md

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <asm/io.h>
#include <mm/kalloc.h>
#include <sys/pci.h>
#include <blibc_common.h>

/* PCI configuration mechanism #1 */
#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

//dev is PCI_DEVFN(slot,func), with the bus number above it (only bus 0 for now)
#define PCI_CONFIG_CMD(dev,reg) (0x80000000 | ((dev) << 8) | ((reg) & ~3))

#define PCI_MAX_SLOTS 32 //slots on bus 0

struct pci_dev *pci[PCI_MAX_SLOTS]; //one entry per slot, vendor 0xFFFF when the slot is empty

/**** Configuration space access ****/

u_char pci_config_read_byte(int dev, int reg) {
    u_char ret;
    KCRIT_ENTER
    outl(PCI_CONFIG_ADDR, PCI_CONFIG_CMD(dev,reg));
    ret = inb(PCI_CONFIG_DATA + (reg & 3));
    KCRIT_LEAVE
    return ret;
}

u_short pci_config_read_word(int dev, int reg) {
    u_short ret;
    KCRIT_ENTER
    outl(PCI_CONFIG_ADDR, PCI_CONFIG_CMD(dev,reg));
    ret = inw(PCI_CONFIG_DATA + (reg & 2));
    KCRIT_LEAVE
    return ret;
}

u_long pci_config_read_long(int dev, int reg) {
    u_long ret;
    KCRIT_ENTER
    outl(PCI_CONFIG_ADDR, PCI_CONFIG_CMD(dev,reg));
    ret = inl(PCI_CONFIG_DATA);
    KCRIT_LEAVE
    return ret;
}

void pci_config_write_byte(int dev, int reg, u_char value) {
    KCRIT_ENTER
    outl(PCI_CONFIG_ADDR, PCI_CONFIG_CMD(dev,reg));
    outb(PCI_CONFIG_DATA + (reg & 3), value);
    KCRIT_LEAVE
}

void pci_config_write_word(int dev, int reg, u_short value) {
    KCRIT_ENTER
    outl(PCI_CONFIG_ADDR, PCI_CONFIG_CMD(dev,reg));
    outw(PCI_CONFIG_DATA + (reg & 2), value);
    KCRIT_LEAVE
}

void pci_config_write_long(int dev, int reg, u_long value) {
    KCRIT_ENTER
    outl(PCI_CONFIG_ADDR, PCI_CONFIG_CMD(dev,reg));
    outl(PCI_CONFIG_DATA, value);
    KCRIT_LEAVE
}

/**** Devices ****/

//turn on the decoders the device uses, and let it be a bus master
void pci_enable_device(struct pci_dev *dev) {
    u_short cmd;
    int i;

    if(!dev || dev->vendor == 0xFFFF) return;

    cmd = pci_config_read_word(dev->dev, PCI_COMMAND);
    for(i=0;i<6;i++) {
        if(!dev->end[i]) continue; //base address not used
        if(pci_config_read_long(dev->dev, PCI_BASE_ADDRESS_0 + (i*4)) & PCI_BASE_ADDRESS_SPACE_IO)
            cmd |= PCI_COMMAND_IO;
        else
            cmd |= PCI_COMMAND_MEMORY;
    }
    cmd |= PCI_COMMAND_MASTER; //the 3c905b DMAs its own rings
    pci_config_write_word(dev->dev, PCI_COMMAND, cmd);

    dev->current_state = 0; //D0, fully on
}

//the size of a base address, from the bits that stuck after writing all 1's
static u_long pci_size(u_long base, u_long mask) {
    u_long size = mask & base; //the bits the device decodes
    size = size & ~(size-1); //the lowest one of them is the size
    return size-1; //extent = size - 1
}

//read one slot on bus 0 (function 0). Empty slots come back with vendor 0xFFFF
static struct pci_dev *pci_add_device(int slot) {
    struct pci_dev *dev;
    u_long l,sz;
    int i,reg,bars;

    dev = (struct pci_dev *)kalloc(sizeof(struct pci_dev));
    if(!dev) return 0;
    memset(dev,0,sizeof(struct pci_dev));

    dev->dev = PCI_DEVFN(slot,0);
    dev->vendor = pci_config_read_word(dev->dev, PCI_VENDOR_ID);
    if(dev->vendor == 0xFFFF) return dev; //nothing here

    dev->device = pci_config_read_word(dev->dev, PCI_DEVICE_ID);
    dev->class = pci_config_read_word(dev->dev, PCI_CLASS_DEVICE); //base class and sub class
    dev->revision = pci_config_read_byte(dev->dev, PCI_REVISION_ID);
    dev->irq = pci_config_read_byte(dev->dev, PCI_INTERRUPT_LINE);

    switch(pci_config_read_byte(dev->dev, PCI_HEADER_TYPE) & 0x7F) {
    case PCI_HEADER_TYPE_BRIDGE:
        bars = 2;
        break;
    case PCI_HEADER_TYPE_CARDBUS:
        bars = 1;
        break;
    default:
        bars = 6;
        break;
    }

    for(i=0;i<bars;i++) {
        reg = PCI_BASE_ADDRESS_0 + (i*4);

        KCRIT_ENTER //size it: write all 1's, see what sticks, put it back
        l = pci_config_read_long(dev->dev, reg);
        pci_config_write_long(dev->dev, reg, ~0);
        sz = pci_config_read_long(dev->dev, reg);
        pci_config_write_long(dev->dev, reg, l);
        KCRIT_LEAVE

        if(!sz || sz == 0xFFFFFFFF) continue; //not implemented

        if(l & PCI_BASE_ADDRESS_SPACE_IO) {
            dev->begin[i] = l & PCI_BASE_ADDRESS_IO_MASK;
            dev->end[i] = dev->begin[i] + pci_size(sz, PCI_BASE_ADDRESS_IO_MASK & 0xFFFF);
        } else {
            dev->begin[i] = l & PCI_BASE_ADDRESS_MEM_MASK;
            dev->end[i] = dev->begin[i] + pci_size(sz, PCI_BASE_ADDRESS_MEM_MASK);
        }
    }

    kprintf("+PCI: slot %i: vendor 0x%x device 0x%x class 0x%x irq %i\n",slot,dev->vendor,dev->device,dev->class,dev->irq);
    return dev;
}

//scan bus 0 and fill in pci[], returns how many devices were found
u_int pci_find_devices(void) {
    int slot;
    u_int count=0;

    for(slot=0;slot<PCI_MAX_SLOTS;slot++) {
        pci[slot] = pci_add_device(slot);
        if(pci[slot] && pci[slot]->vendor != 0xFFFF) {
            pci_enable_device(pci[slot]);
            count++;
        }
    }
    return count;
}

u_long init_pci(void) {
    u_long save,check,count;

    kprintf("\nInitializing PCI Bus\n");

    //see if configuration mechanism #1 is there
    KCRIT_ENTER
    save = inl(PCI_CONFIG_ADDR);
    outl(PCI_CONFIG_ADDR, 0x80000000);
    check = inl(PCI_CONFIG_ADDR);
    outl(PCI_CONFIG_ADDR, save);
    KCRIT_LEAVE

    if(check != 0x80000000) {
        kprintf("*No PCI Bus Detected\n");
        return 0;
    }

    count = pci_find_devices();
    kprintf("*%i PCI Devices Found\n",count);
    kprintf("*DONE\n");
    return count;
}
