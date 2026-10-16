/*
SurfOS USB Boot Mouse Parsing
--------------------
File: mousedesc.c Date: 10/10/26
--------------------
Pure boot mouse descriptor and report parsing.
*/
#include <sys/usbmouse.h>
#include <blibc_common.h>

int usb_mouse_find(const u8 *cfg, u_int len, struct usb_mouse_function *out) {
    struct usb_mouse_function found = { 0, 0, NULL, NULL };
    u_int off, total, endpoints = 0, expected = 0;
    u8 iface = 0, alt = 0;
    bool have_iface = false, boot = false;
    const u8 *previous = NULL;
    if(!cfg || !out || len < 9 || cfg[0] < 9 || cfg[1] != USB_DT_CONFIG) return -1;
    total = cfg[2] | ((u_int)cfg[3] << 8);
    if(total < cfg[0] || total > len) return -1;

    /* Validate the whole declared configuration, including the tail after a match.
       Endpoint counts belong to each alternate setting, not to the next interface. */
    for(off = 0; off < total;) {
        const u8 *p = cfg + off;
        u_int n;
        if(total - off < 2 || (n = p[0]) < 2 || n > total - off) return -1;
        if(p[1] == USB_DT_CONFIG && off != 0) return -1;
        if(p[1] == USB_DT_INTERFACE) {
            if(n < 9 || (have_iface && endpoints != expected)) return -1;
            have_iface = true;
            endpoints = 0;
            expected = p[4];
            iface = p[2]; alt = p[3];
            boot = p[5] == USB_CLASS_HID && p[6] == 1 && p[7] == 2;
        } else if(p[1] == USB_DT_ENDPOINT) {
            u_int raw, mps;
            if(n < 7 || !have_iface || ++endpoints > expected) return -1;
            raw = p[4] | ((u_int)p[5] << 8);
            mps = raw & 0x7ff;
            if(boot && !found.in && (p[2] & 0x8f) == p[2] && (p[2] & 0x80) && (p[2] & 15) &&
               (p[3] & 3) == USB_EP_INTR && mps >= 3 && mps <= 1024 &&
               !(raw & 0xe000) && (raw & 0x1800) != 0x1800 && p[6]) {
                found.iface = iface; found.alt = alt;
                found.in = (const struct usb_endpoint_descriptor *)p;
            }
        } else if(p[1] == USB_DT_SS_EP_COMP) {
            if(n < 6) return -1;
            if(previous == (const u8 *)found.in && found.in) {
                if(p[2] > 15) return -1;
                found.comp = (const struct usb_ss_ep_comp_descriptor *)p;
            }
        }
        previous = p;
        off += n;
    }
    if(!found.in || endpoints != expected) return -1;
    *out = found;
    return 0;
}

bool usb_mouse_decode_boot(const u8 *bytes, u_int len, struct usb_mouse_report *out) {
    if(!bytes || !out || len < 3) return false;
    out->buttons = bytes[0] & 7;
    out->dx = (int)(i8)bytes[1];
    out->dy = (int)(i8)bytes[2];
    /* Boot protocol defines exactly these three fields. Extra bytes, including
       any report-protocol wheel byte, have no meaning in the selected protocol. */
    return true;
}
