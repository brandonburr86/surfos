/*
SurfOS USB Descriptor Parsing
--------------------
File: usbdesc.c Date: 10/5/26 (roadmap M7: USB networking)
--------------------
Walking a configuration descriptor, finding a CDC Ethernet (ECM) or NCM function
in it, reading the MAC address string, and NCM's 16-bit transfer blocks in both
directions. No I/O and no allocation: tests/host/test_usb.c runs this on the host
against hand-built descriptors and blocks. Everything a device sends is checked
against the length it came in, so a broken descriptor ends a walk instead of
reading past it.
*/

#include <surfos/types.h>
#include <sys/usb.h>
#include <blibc_common.h>

/* CDC (USB CDC 1.2 and its ECM and NCM subclass specifications) */
#define CDC_SUBCLASS_ECM     0x06
#define CDC_SUBCLASS_NCM     0x0d
#define CDC_FD_UNION         0x06
#define CDC_FD_ETHERNET      0x0f
#define CDC_FD_NCM           0x1a

#define NTH16_SIGNATURE      0x484d434e      /* "NCMH" */
#define NDP16_SIGNATURE      0x304d434e      /* "NCM0": no CRC */
#define NDP16_CRC_SIGNATURE  0x314d434e      /* "NCM1": each datagram carries a CRC-32 */
#define NTH16_LEN            12
#define NDP16_LEN_ONE        16              /* header, one datagram pointer, the terminator */

static u16 get16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 get32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, (u16)v); put16(p + 2, (u16)(v >> 16)); }

/* p == NULL starts with the configuration descriptor itself */
const u8 *usb_desc_next(const u8 *cfg, u_int len, const u8 *p) {
    const u8 *end = cfg + len;
    if(!p) p = cfg;
    else {
        if(p < cfg || p + 2 > end || p[0] < 2) return NULL;
        p += p[0];
    }
    if(p + 2 > end || p[0] < 2 || p + p[0] > end) return NULL;
    return p;
}

const struct usb_interface_descriptor *usb_find_interface(const u8 *cfg, u_int len, u8 number, u8 alt) {
    const u8 *p;
    for(p = usb_desc_next(cfg, len, NULL); p; p = usb_desc_next(cfg, len, p)) {
        const struct usb_interface_descriptor *i = (const struct usb_interface_descriptor *)p;
        if(p[1] == USB_DT_INTERFACE && p[0] >= 9 && i->bInterfaceNumber == number && i->bAlternateSetting == alt) return i;
    }
    return NULL;
}

u8 usb_config_class(const u8 *cfg, u_int len) {
    const u8 *p;
    for(p = usb_desc_next(cfg, len, NULL); p; p = usb_desc_next(cfg, len, p)) {
        if(p[1] == USB_DT_INTERFACE && p[0] >= 9) return ((const struct usb_interface_descriptor *)p)->bInterfaceClass;
    }
    return 0;
}

/* the SuperSpeed companion right after an endpoint descriptor, if there is one */
static const struct usb_ss_ep_comp_descriptor *companion(const u8 *cfg, u_int len, const u8 *ep) {
    const u8 *n = usb_desc_next(cfg, len, ep);
    if(n && n[1] == USB_DT_SS_EP_COMP && n[0] >= 6) return (const struct usb_ss_ep_comp_descriptor *)n;
    return NULL;
}

/* the data interface: an alternate setting of `number` with a bulk IN and a bulk OUT endpoint */
static bool find_data_interface(const u8 *cfg, u_int len, u8 number, struct cdc_net_function *f) {
    const u8 *p;
    for(p = usb_desc_next(cfg, len, NULL); p; p = usb_desc_next(cfg, len, p)) {
        const struct usb_interface_descriptor *i = (const struct usb_interface_descriptor *)p;
        const u8 *q;
        if(p[1] != USB_DT_INTERFACE || p[0] < 9 || i->bInterfaceNumber != number) continue;
        if(i->bInterfaceClass != USB_CLASS_CDC_DATA || i->bNumEndpoints < 2) continue;
        f->in = f->out = NULL;
        f->in_comp = f->out_comp = NULL;
        for(q = usb_desc_next(cfg, len, p); q && q[1] != USB_DT_INTERFACE; q = usb_desc_next(cfg, len, q)) {
            const struct usb_endpoint_descriptor *e = (const struct usb_endpoint_descriptor *)q;
            if(q[1] != USB_DT_ENDPOINT || q[0] < 7 || (e->bmAttributes & 3) != USB_EP_BULK) continue;
            if(e->bEndpointAddress & USB_DIR_IN) { if(!f->in) { f->in = e; f->in_comp = companion(cfg, len, q); } }
            else if(!f->out) { f->out = e; f->out_comp = companion(cfg, len, q); }
        }
        if(f->in && f->out) {
            f->data_alt = i->bAlternateSetting;
            return true;
        }
    }
    f->in = f->out = NULL;
    return false;
}

static int find_kind(const u8 *cfg, u_int len, int kind, struct cdc_net_function *f) {
    u8 subclass = kind == CDC_ECM ? CDC_SUBCLASS_ECM : CDC_SUBCLASS_NCM;
    const u8 *p, *q;
    for(p = usb_desc_next(cfg, len, NULL); p; p = usb_desc_next(cfg, len, p)) {
        const struct usb_interface_descriptor *i = (const struct usb_interface_descriptor *)p;
        if(p[1] != USB_DT_INTERFACE || p[0] < 9 || i->bAlternateSetting != 0) continue;
        if(i->bInterfaceClass != USB_CLASS_COMM || i->bInterfaceSubClass != subclass) continue;
        memset(f, 0, sizeof(*f));
        f->kind = kind;
        f->comm_iface = i->bInterfaceNumber;
        f->data_iface = i->bInterfaceNumber + 1;      /* when there is no union descriptor */
        f->max_segment = 1514;
        /* its functional descriptors and the notification endpoint, up to the next interface */
        for(q = usb_desc_next(cfg, len, p); q && q[1] != USB_DT_INTERFACE; q = usb_desc_next(cfg, len, q)) {
            if(q[1] == USB_DT_CS_INTERFACE && q[0] >= 3) {
                if(q[2] == CDC_FD_UNION && q[0] >= 5) f->data_iface = q[4];
                else if(q[2] == CDC_FD_ETHERNET && q[0] >= 13) {
                    f->mac_string = q[3];
                    f->max_segment = get16(q + 8);
                } else if(q[2] == CDC_FD_NCM && q[0] >= 6) f->ncm_caps = q[5];
            } else if(q[1] == USB_DT_ENDPOINT && q[0] >= 7 && !f->notify) {
                const struct usb_endpoint_descriptor *e = (const struct usb_endpoint_descriptor *)q;
                if((e->bmAttributes & 3) == USB_EP_INTR && (e->bEndpointAddress & USB_DIR_IN)) {
                    f->notify = e;
                    f->notify_comp = companion(cfg, len, q);
                }
            }
        }
        if(find_data_interface(cfg, len, f->data_iface, f)) return 0;
    }
    return -1;
}

/* ECM first: it is the simpler protocol, and the one QEMU tests */
int cdc_find_net_function(const u8 *cfg, u_int len, struct cdc_net_function *f) {
    if(len < 9 || cfg[1] != USB_DT_CONFIG) return -1;
    if(find_kind(cfg, len, CDC_ECM, f) == 0) return 0;
    if(find_kind(cfg, len, CDC_NCM, f) == 0) return 0;
    memset(f, 0, sizeof(*f));
    return -1;
}

static int hexval(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* twelve hex digits; a group (multicast) address or all zeros is not a station's */
bool cdc_parse_mac(const char *hex, u8 *mac) {
    u8 m[6];
    int i;
    for(i = 0; i < 12; i++) {
        int v = hexval(hex[i]);
        if(v < 0) return false;
        if(i & 1) m[i / 2] |= (u8)v;
        else m[i / 2] = (u8)(v << 4);
    }
    if(hex[12] != 0) return false;
    if(m[0] & 1) return false;
    if(!(m[0] | m[1] | m[2] | m[3] | m[4] | m[5])) return false;
    memcpy(mac, m, 6);
    return true;
}

/* One frame in a block: the NTH16 header, one NDP16 (aligned to p->align), the datagram
   where (offset % div) == rem. A block that would fill its last packet exactly gets one
   byte more, so it ends in a short packet (as Linux's cdc_ncm does). 0 when it does not fit. */
u_int ncm_build_ntb16(u8 *out, u_int cap, u16 seq, const u8 *frame, u_int len, const struct ncm_out_params *p) {
    u_int align = p->align >= 4 ? p->align : 4, div = p->div ? p->div : 1, rem = p->rem % (p->div ? p->div : 1);
    u_int ndp, dgram, block;
    if(align & (align - 1)) align = 4;
    ndp = (NTH16_LEN + align - 1) & ~(align - 1);
    dgram = ndp + NDP16_LEN_ONE;
    dgram += (rem + div - dgram % div) % div;
    block = dgram + len;
    if(p->mps && block % p->mps == 0) block++;
    if(!len || block > cap || (p->max && block > p->max) || block > 0xffff) return 0;
    memset(out, 0, dgram);
    put32(out, NTH16_SIGNATURE);
    put16(out + 4, NTH16_LEN);
    put16(out + 6, seq);
    put16(out + 8, (u16)block);
    put16(out + 10, (u16)ndp);
    put32(out + ndp, NDP16_SIGNATURE);
    put16(out + ndp + 4, NDP16_LEN_ONE);
    put16(out + ndp + 6, 0);                       /* no further NDP */
    put16(out + ndp + 8, (u16)dgram);
    put16(out + ndp + 10, (u16)len);
    put32(out + ndp + 12, 0);                      /* the terminating entry */
    memcpy(out + dgram, frame, len);
    if(block > dgram + len) out[block - 1] = 0;
    return block;
}

/* Every datagram of a received block goes to fn. The number handed over, or -1 when the
   header is not an NTB16 one. A bad pointer table ends the walk, and so does one that does
   not lie after the last (a loop would hand the same frames on again); a datagram outside
   the block is skipped. */
int ncm_parse_ntb16(const u8 *b, u_int len, void (*fn)(void *arg, const u8 *frame, u_int len), void *arg) {
    u_int block, ndp, last = 0;
    int n = 0;
    if(len < NTH16_LEN || get32(b) != NTH16_SIGNATURE || get16(b + 4) != NTH16_LEN) return -1;
    block = get16(b + 8);
    if(block < NTH16_LEN || block > len) return -1;
    ndp = get16(b + 10);
    while(ndp) {
        u_int sig, ndplen, i;
        if(ndp <= last || ndp < NTH16_LEN || (ndp & 3) || ndp + 8 > block) break;
        last = ndp;
        sig = get32(b + ndp);
        ndplen = get16(b + ndp + 4);
        if((sig != NDP16_SIGNATURE && sig != NDP16_CRC_SIGNATURE) || ndplen < NDP16_LEN_ONE || (ndplen & 3) ||
           ndp + ndplen > block) break;
        for(i = ndp + 8; i + 4 <= ndp + ndplen; i += 4) {
            u_int off = get16(b + i), dlen = get16(b + i + 2);
            if(!off || !dlen) break;
            if(sig == NDP16_CRC_SIGNATURE) {
                if(dlen < 4) continue;
                dlen -= 4;
            }
            if(off < NTH16_LEN || off + dlen > block) continue;
            fn(arg, b + off, dlen);
            n++;
        }
        ndp = get16(b + ndp + 6);
    }
    return n;
}
