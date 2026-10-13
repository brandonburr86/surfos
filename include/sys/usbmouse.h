/*
SurfOS USB Boot Mouse
--------------------
File: usbmouse.h Date: 10/10/26
--------------------
Boot-protocol descriptors and reports; no device-specific report formats.
*/
#ifndef _SYS_USBMOUSE_H
#define _SYS_USBMOUSE_H

#include <sys/usb.h>

struct usb_mouse_function {
    u8 iface, alt;
    const struct usb_endpoint_descriptor *in;
    const struct usb_ss_ep_comp_descriptor *comp;
};

struct usb_mouse_report { int dx, dy; unsigned buttons; };

/* Pure parsers. Failure leaves the output unchanged. Returned descriptors belong to cfg. */
int usb_mouse_find(const u8 *cfg, u_int len, struct usb_mouse_function *out);
bool usb_mouse_decode_boot(const u8 *bytes, u_int len, struct usb_mouse_report *out);

#endif
