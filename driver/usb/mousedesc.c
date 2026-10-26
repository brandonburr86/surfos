/*
SurfOS USB Boot Mouse Parsing
--------------------
File: mousedesc.c Date: 10/10/26
--------------------
Pure boot mouse descriptor and report parsing.
10/26/26 (stage C8): the HID descriptor's report length, and whether the report descriptor puts
the Wheel at the fourth byte (driver/hidpointer.c parses it, into a layout allocated for it).
*/
#include <sys/usbmouse.h>
#include <sys/hidpointer.h>
#include <mm/kalloc.h>
#include <blibc_common.h>

int usb_mouse_find(const u8 *cfg, u_int len, struct usb_mouse_function *out) {
    struct usb_mouse_function found = { 0, 0, NULL, NULL, 0 };
    u_int off, total, endpoints = 0, expected = 0, report_len = 0;
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
            if(!found.in) report_len = 0;               /* 10/26/26: an interface's own HID descriptor */
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
                found.report_len = (u16)report_len;
            }
        } else if(p[1] == USB_DT_SS_EP_COMP) {
            if(n < 6) return -1;
            if(previous == (const u8 *)found.in && found.in) {
                if(p[2] > 15) return -1;
                found.comp = (const struct usb_ss_ep_comp_descriptor *)p;
            }
        } else if(p[1] == 0x21 && boot && !found.in && n >= 9 && p[6] == 0x22) {
            report_len = p[7] | ((u_int)p[8] << 8);     /* 10/26/26: HID's, its first class descriptor the report's */
        }
        previous = p;
        off += n;
    }
    if(!found.in || endpoints != expected) return -1;
    *out = found;
    return 0;
}

bool usb_mouse_decode_boot(const u8 *bytes, u_int len, bool wheel, struct usb_mouse_report *out) {
    if(!bytes || !out || len < 3) return false;
    out->buttons = bytes[0] & 7;
    out->dx = (int)(i8)bytes[1];
    out->dy = (int)(i8)bytes[2];
    /* Boot protocol defines exactly these three fields. 10/26/26 (C8): a fourth byte is the wheel's
       when the report descriptor puts it there (HID's wheel: + away from the user) */
    out->wheel = wheel && len >= 4 ? (int)(i8)bytes[3] : 0;
    return true;
}

/* 10/26/26 (C8): the report descriptor parsed (driver/hidpointer.c): its reports carry no IDs, and one of them has
   its Wheel as a signed 8-bit field at bit 24, after the buttons, X and Y bytes, where the boot report's fourth byte
   is. The layout (3 KB) is allocated, off the usb thread's stack, which the parser uses too: none, no wheel */
bool usb_mouse_wheel(const u8 *desc, u_int len) {
    struct hid_pointer_layout *layout = (struct hid_pointer_layout *)kalloc(sizeof(*layout));
    bool wheel = false;
    u_int i;
    if(!layout) return false;
    if(hid_pointer_parse(desc, len, layout) == HID_POINTER_OK && !layout->numbered)
        for(i = 0; i < layout->count && !wheel; i++) {
            const struct hid_pointer_field *w = &layout->report[i].wheel;
            wheel = w->bit_size == 8 && w->bit_offset == 24 && w->minimum < 0;
        }
    kfree(layout);
    return wheel;
}
