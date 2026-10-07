/*
SurfOS PCI Configuration Space, Described
----------------------
File: pciinfo.h Date: 10/7/26
----------------------
What lspci -v and wifiinfo print about a PCI function, read from its 256 bytes of
configuration space (what configuration mechanism 1 reaches): the ids with the
subsystem, command and status, the interrupt pin, a bridge's buses, and the capability
list (power management and the power state, MSI, MSI-X, PCI Express and its link).
Also what a known wireless chip is, and what a known Bluetooth half of a Wi-Fi card
says about the card, for a Wi-Fi driver (docs/WIFI-PLANNING.md). No I/O: the caller
reads the bytes (pci_read_config()), and tests/host/test_pciinfo.c runs this on the
host. Each line goes to a callback, indented 8 columns as lspci's BAR lines are.
*/

#ifndef _SURFOS_PCIINFO_H
#define _SURFOS_PCIINFO_H

#include <surfos/types.h>

#define PCI_CONFIG_SIZE 256

typedef void (*pci_line_fn)(void *ctx, const char *line);

void pciinfo_details(const u8 *cfg, pci_line_fn out, void *ctx);   /* ids, command and status, interrupt, buses, capabilities */
void pciinfo_dump(const u8 *cfg, pci_line_fn out, void *ctx);      /* the 256 bytes, 16 to a line */
bool pciinfo_is_wireless(const u8 *cfg);                           /* a wireless network controller, or a known Wi-Fi chip */
const char *pciinfo_chip(u16 vendor, u16 device);                  /* what a known wireless chip probably is, else NULL */
const char *pciinfo_bluetooth(u16 vendor, u16 product, u8 usb_class);  /* a USB device: what it says about a Wi-Fi card, else NULL */

#endif
