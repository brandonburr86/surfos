/*
SurfOS PCI names
--------------------
File: pci_names.c   Date: 10/3/26 (roadmap D1)
--------------------
The vendor, device and class tables Brandon added on 7/1/04 (driver/PCIDATA.H, from
pcidatabase.com) finally get used: lspci prints names instead of numbers.
*/

#include <surfos/types.h>
#include <sys/pci.h>
#include <blibc_common.h>

#include "PCIDATA.H"

const char *pci_vendor_name(u16 vendor) {
    u_int i;
    for(i = 0; i < PCI_VENTABLE_LEN; i++) {
        if(PciVenTable[i].VenId == vendor) return PciVenTable[i].VenFull;
    }
    return "Unknown vendor";
}

/* "82540EM Gigabit Ethernet Controller": the chip name and its description, whichever exist */
const char *pci_device_name(u16 vendor, u16 device, char *buf, size_t size) {
    u_int i;
    for(i = 0; i < PCI_DEVTABLE_LEN; i++) {
        const char *chip, *desc;
        if(PciDevTable[i].VenId != vendor || PciDevTable[i].DevId != device) continue;
        chip = PciDevTable[i].Chip ? PciDevTable[i].Chip : "";
        desc = PciDevTable[i].ChipDesc ? PciDevTable[i].ChipDesc : "";
        if(!strcmp(chip, "???")) chip = "";
        if(!strcmp(desc, "???")) desc = "";
        if(!chip[0] && !desc[0]) continue;
        snprintf(buf, size, "%s%s%s", chip, (chip[0] && desc[0]) ? " " : "", desc);
        return buf;
    }
    snprintf(buf, size, "unknown device");
    return buf;
}

const char *pci_class_name(u8 class_code, u8 subclass) {
    u_int i;
    const char *base = NULL;
    if(class_code == 0x03 && subclass == 0x00) return "VGA"; /* the table says "PC Compatible" */
    for(i = 0; i < PCI_CLASSCODETABLE_LEN; i++) {
        if(PciClassCodeTable[i].BaseClass != class_code) continue;
        if(!base) base = PciClassCodeTable[i].BaseDesc;
        if(PciClassCodeTable[i].SubClass == subclass && PciClassCodeTable[i].SubDesc && PciClassCodeTable[i].SubDesc[0])
            return PciClassCodeTable[i].SubDesc;
    }
    return base ? base : "Unknown class";
}
