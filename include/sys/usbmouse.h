/*
SurfOS USB Boot Mouse
--------------------
File: usbmouse.h Date: 10/10/26
--------------------
Boot-protocol descriptors and reports; no device-specific report formats.
10/26/26 (stage C8 of docs/superpowers/plans/2026-10-25-app-windows.md): the boot protocol names three
bytes and leaves the rest to the device (HID 1.11 B.2), so a fourth is taken as the wheel only for a
mouse whose report descriptor puts its Wheel there: an 8-bit relative field at bit 24, with no Report
IDs (usb_mouse_wheel()).
*/
#ifndef _SYS_USBMOUSE_H
#define _SYS_USBMOUSE_H

#include <sys/usb.h>

struct usb_mouse_function {
    u8 iface, alt;
    const struct usb_endpoint_descriptor *in;
    const struct usb_ss_ep_comp_descriptor *comp;
    u16 report_len;                 /* 10/26/26: its HID descriptor's report descriptor length, 0: none named */
};

struct usb_mouse_report { int dx, dy, wheel; unsigned buttons; };   /* wheel: notches, + away from the user */

/* Pure parsers. Failure leaves the output unchanged. Returned descriptors belong to cfg. */
int usb_mouse_find(const u8 *cfg, u_int len, struct usb_mouse_function *out);
/* 10/26/26: wheel: the fourth byte is the wheel's (usb_mouse_wheel() said so) */
bool usb_mouse_decode_boot(const u8 *bytes, u_int len, bool wheel, struct usb_mouse_report *out);
/* 10/26/26: does the report descriptor put the Wheel at the boot report's fourth byte? */
bool usb_mouse_wheel(const u8 *desc, u_int len);

#endif
