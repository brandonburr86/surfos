/*
SurfOS USB
----------------------
File: usb.h     Date: 10/5/26 (roadmap M7: USB networking)
----------------------
The USB stack: the xHCI host controller (driver/usb/xhci.c), the device layer
that enumerates what is plugged into the root hub ports and runs the "usb"
thread (driver/usb/usb.c), descriptor, CDC and NCM parsing that does no I/O
(driver/usb/usbdesc.c, unit-tested on the host) and the USB Ethernet class
driver (driver/usb/usbnet.c).

Transfers on endpoint 0 are synchronous and are made by the usb thread (or at
boot); bulk and interrupt transfers are queued with usb_submit() from any
context and finish in their done() callback, which runs from the controller's
event poll with interrupts off.
*/

#ifndef _SYS_USB_H
#define _SYS_USB_H

#include <surfos/types.h>
#include <surfos/sync.h>

/**** requests and descriptors (USB 2.0 and 3.2, chapter 9) ****/

#define USB_DIR_OUT         0x00
#define USB_DIR_IN          0x80
#define USB_TYPE_STANDARD   0x00
#define USB_TYPE_CLASS      0x20
#define USB_TYPE_VENDOR     0x40
#define USB_RECIP_DEVICE    0x00
#define USB_RECIP_INTERFACE 0x01
#define USB_RECIP_ENDPOINT  0x02

#define USB_REQ_GET_STATUS        0
#define USB_REQ_CLEAR_FEATURE     1
#define USB_REQ_SET_FEATURE       3
#define USB_REQ_SET_ADDRESS       5
#define USB_REQ_GET_DESCRIPTOR    6
#define USB_REQ_GET_CONFIGURATION 8
#define USB_REQ_SET_CONFIGURATION 9
#define USB_REQ_GET_INTERFACE     10
#define USB_REQ_SET_INTERFACE     11
#define USB_FEATURE_ENDPOINT_HALT 0

#define USB_DT_DEVICE       1
#define USB_DT_CONFIG       2
#define USB_DT_STRING       3
#define USB_DT_INTERFACE    4
#define USB_DT_ENDPOINT     5
#define USB_DT_CS_INTERFACE 0x24
#define USB_DT_SS_EP_COMP   0x30

#define USB_CLASS_PER_INTERFACE 0x00
#define USB_CLASS_AUDIO     0x01
#define USB_CLASS_COMM      0x02
#define USB_CLASS_HID       0x03
#define USB_CLASS_PRINTER   0x07
#define USB_CLASS_STORAGE   0x08
#define USB_CLASS_HUB       0x09
#define USB_CLASS_CDC_DATA  0x0a
#define USB_CLASS_VIDEO     0x0e
#define USB_CLASS_WIRELESS  0xe0
#define USB_CLASS_MISC      0xef
#define USB_CLASS_VENDOR    0xff

#define USB_EP_CONTROL 0
#define USB_EP_ISOC    1
#define USB_EP_BULK    2
#define USB_EP_INTR    3

struct usb_device_descriptor {
    u8 bLength, bDescriptorType;
    u16 bcdUSB;
    u8 bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    u16 idVendor, idProduct, bcdDevice;
    u8 iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} __attribute__((packed));

struct usb_config_descriptor {
    u8 bLength, bDescriptorType;
    u16 wTotalLength;
    u8 bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, bMaxPower;
} __attribute__((packed));

struct usb_interface_descriptor {
    u8 bLength, bDescriptorType;
    u8 bInterfaceNumber, bAlternateSetting, bNumEndpoints;
    u8 bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
} __attribute__((packed));

struct usb_endpoint_descriptor {
    u8 bLength, bDescriptorType;
    u8 bEndpointAddress, bmAttributes;
    u16 wMaxPacketSize;
    u8 bInterval;
} __attribute__((packed));

struct usb_ss_ep_comp_descriptor {
    u8 bLength, bDescriptorType;
    u8 bMaxBurst, bmAttributes;
    u16 wBytesPerInterval;
} __attribute__((packed));

/* port speeds: the xHCI default protocol speed IDs */
#define USB_SPEED_FULL       1
#define USB_SPEED_LOW        2
#define USB_SPEED_HIGH       3
#define USB_SPEED_SUPER      4
#define USB_SPEED_SUPER_PLUS 5

/* transfer results: a byte count, or one of these */
#define USB_OK        0
#define USB_PENDING   1
#define USB_ESTALL   -1     /* the device stalled the endpoint (or does not know the request) */
#define USB_EIO      -2     /* transaction error, babble, a controller error */
#define USB_ETIMEOUT -3
#define USB_ENODEV   -4     /* the device is gone, or the controller stopped working */
#define USB_ECANCEL  -5     /* stopped before it finished */
#define USB_ENOMEM   -6
#define USB_EINVAL   -7

/**** devices, endpoints, transfers ****/

struct usb_device;
struct usb_endpoint;
struct usb_hc;

struct xhci_trb {
    u32 lo, hi, status, control;
};

/* an xHCI ring: one segment whose last TRB links back to the first */
struct usb_ring {
    struct xhci_trb *trbs;         /* DMA heap, aligned to its size */
    void *raw;                     /* what kfree_dma() takes */
    struct usb_xfer **owner;       /* per TRB: the transfer it belongs to, while queued */
    u_int n;                       /* TRBs, the link included */
    u_int enq, deq, used;          /* producer index, oldest TRB still queued, TRBs queued */
    u32 pcs;                       /* producer cycle state */
};

struct usb_xfer {
    struct usb_endpoint *ep;
    void *buf;                     /* DMA heap; must not cross a 64 KB boundary */
    u_int length;                  /* at most 64 KB */
    volatile u_int actual;
    volatile int status;           /* USB_PENDING while queued */
    void (*done)(struct usb_xfer *x);  /* from the controller's poll, interrupts off: be quick */
    void *ctx;
    /* the controller's bookkeeping */
    u_int first, last, ntrbs, data_trb;
};

struct usb_endpoint {
    struct usb_device *dev;
    u8 addr;                       /* bEndpointAddress: number | USB_DIR_IN */
    u8 type;                       /* USB_EP_* */
    u16 mps;                       /* wMaxPacketSize, the packet size bits */
    u8 interval;                   /* bInterval as the descriptor has it */
    u8 burst;                      /* SuperSpeed bMaxBurst, or a high-bandwidth multiplier */
    u8 dci;                        /* xHCI device context index: 1 for endpoint 0 */
    bool configured;               /* the controller knows it (Configure Endpoint done) */
    volatile bool halted;          /* stopped by an error; the usb thread resets it */
    volatile bool recovering;      /* someone is resetting it right now */
    bool stuck;                    /* the reset failed: transfers fail at once */
    volatile int halt_status;
    u_long xfers, errors;
    struct usb_ring ring;
};

#define USB_MAX_EP    32           /* by device context index */
#define USB_MAX_PORTS 255

struct usb_driver;

struct usb_device {
    struct usb_hc *hc;
    u8 port;                       /* root hub port, from 1 */
    u8 speed;                      /* USB_SPEED_* */
    u8 slot;                       /* xHCI device slot, 0 when none */
    u8 address;
    volatile bool gone;            /* unplugged: new transfers fail with USB_ENODEV */
    bool dma_live;                 /* the controller did not let go of it: keep its buffers */
    struct usb_device_descriptor desc;
    char manufacturer[32], product[48], serial[24];
    u16 langid;                    /* the language its strings are read in */
    u8 class_code;                 /* the device's class, else its first interface's */
    u8 *config;                    /* the configuration a driver chose (kalloc), else NULL */
    u_int config_len;
    u8 config_value;
    const struct usb_driver *driver;
    void *driver_data;
    char status[64];               /* what lsusb says about it: "eth0 (cdc-ecm)", "no driver" */
    struct usb_endpoint ep0;
    mutex_t ep0_lock;              /* one control transfer at a time */
    struct usb_endpoint *eps[USB_MAX_EP];
    void *out_ctx, *out_raw, *in_ctx, *in_raw;    /* xHCI device and input contexts */
};

/* the part of a host controller the device layer uses */
struct usb_hc {
    char name[24];                 /* "xHCI 00:14.0" */
    u_int nports;
    u8 port_proto[USB_MAX_PORTS + 1];             /* 2 or 3: the USB version of a root port */
    struct usb_device *port_dev[USB_MAX_PORTS + 1];
    u8 port_tries[USB_MAX_PORTS + 1];             /* enumeration attempts since it was plugged in */
    volatile bool failed;          /* stopped working: everything on it is gone */
    const char *failed_why;
    struct usb_hc *next;
};

struct usb_driver {
    const char *name;
    bool (*match)(struct usb_device *dev, const u8 *cfg, u_int len);  /* would it drive this configuration? */
    int (*attach)(struct usb_device *dev);     /* dev->config is the one it matched; 0 when it took the device */
    void (*quiesce)(struct usb_device *dev);   /* unplugged (dev->gone set): wait until no task is inside the driver */
    void (*detach)(struct usb_device *dev);    /* unplugged; the controller has let go of it already */
    void (*service)(struct usb_device *dev);   /* from the usb thread, at least once a second */
};

extern const struct usb_driver usbnet_driver;
extern const struct usb_driver msc_driver;

/* device layer (driver/usb/usb.c) */
int init_usb(void);                            /* the driver table entry: controllers, then the ports */
int usb_control(struct usb_device *dev, u8 type, u8 req, u16 value, u16 index, void *data, u16 len);
int usb_get_descriptor(struct usb_device *dev, u8 type, u8 index, u16 lang, void *buf, u16 len);
int usb_get_string(struct usb_device *dev, u8 index, char *buf, size_t size);
int usb_set_configuration(struct usb_device *dev, u8 value);
int usb_set_interface(struct usb_device *dev, u8 iface, u8 alt);
int usb_clear_halt(struct usb_device *dev, u8 ep_addr);
struct usb_endpoint *usb_endpoint_add(struct usb_device *dev, const struct usb_endpoint_descriptor *d,
                                      const struct usb_ss_ep_comp_descriptor *comp);
int usb_configure_endpoints(struct usb_device *dev);   /* tell the controller about the endpoints added */
int usb_submit(struct usb_xfer *x);            /* bulk or interrupt; any context */
int usb_bulk_sync(struct usb_endpoint *ep, void *buf, u_int len, u_int timeout_ms);   /* bytes or USB_E*; a task */
int usb_endpoint_recover(struct usb_endpoint *ep, u_int timeout_ms);   /* after a halt: 0 when it runs again */
int usb_endpoint_reset(struct usb_endpoint *ep);    /* toggle/sequence to 0 on both sides (BOT reset recovery) */
void usb_poll(void);                           /* run the controllers' event rings now; any context */
void usb_wake(void);                           /* the usb thread has work; any context */
void usb_msleep(u_long ms);                    /* sleeps in a task, busy-waits during boot */
bool usb_can_sleep(void);
void usb_print(void);                          /* lsusb */
const char *usb_describe(char *buf, size_t size);
/* 10/7/26: what is plugged in, copied under the lock so that the caller prints after it (wifiinfo) */
struct usb_info {
    u8 port;                       /* root hub port, from 1 */
    u8 class_code;
    u16 vendor, product;
    char name[82];                 /* manufacturer and product, or "(no name)" */
    char status[64];               /* what drives it, as lsusb says */
};
u_int usb_snapshot(struct usb_info *out, u_int max);   /* how many, at most max; 0 without USB */
const char *usb_strerror(int err);
const char *usb_speed_name(u_int speed);
const char *usb_class_name(u8 class_code);
int usb_stall_test(void);                      /* selftest: endpoint 0 after a stall */

/* the xHCI controller (driver/usb/xhci.c), used by the device layer */
struct usb_hc *xhci_probe(void);               /* takes over and starts every xHCI controller; the list */
u32 xhci_port_status(struct usb_hc *hc, u_int port);
void xhci_port_ack(struct usb_hc *hc, u_int port);   /* clear the port's change bits */
int xhci_port_enable(struct usb_hc *hc, u_int port); /* reset as needed; the speed, or USB_E* */
int xhci_enable_slot(struct usb_device *dev);
int xhci_address_device(struct usb_device *dev);
int xhci_set_ep0_mps(struct usb_device *dev, u_int mps);
int xhci_release_device(struct usb_device *dev);     /* disable the slot, free contexts and rings */
int xhci_control(struct usb_device *dev, const u8 *setup, void *data, u_int timeout_ms);   /* at most 4 KB */
int xhci_endpoint_init(struct usb_endpoint *ep);     /* its transfer ring */
int xhci_configure_endpoints(struct usb_device *dev);
int xhci_submit(struct usb_xfer *x);
int xhci_bulk_sync(struct usb_endpoint *ep, void *buf, u_int len, u_int timeout_ms);
int xhci_recover(struct usb_endpoint *ep);
int xhci_endpoint_reset(struct usb_endpoint *ep);
void xhci_poll(struct usb_hc *hc);             /* interrupts off */
void xhci_service(struct usb_hc *hc);          /* the usb thread: reset halted endpoints */
void xhci_describe(struct usb_hc *hc, char *buf, size_t size);
#define USB_PS_CONNECTED   0x00000001          /* xhci_port_status() bits used outside xhci.c */
#define USB_PS_ENABLED     0x00000002
#define USB_PS_CONNECT_CHANGE 0x00020000
#define USB_PS_CHANGES     0x00fe0000          /* every change bit */

/* parsing, no I/O (driver/usb/usbdesc.c) */
const u8 *usb_desc_next(const u8 *cfg, u_int len, const u8 *p);   /* the descriptor after p, NULL at the end */
const struct usb_interface_descriptor *usb_find_interface(const u8 *cfg, u_int len, u8 number, u8 alt);
u8 usb_config_class(const u8 *cfg, u_int len);  /* the first interface's class */

#define CDC_ECM 1
#define CDC_NCM 2
struct cdc_net_function {
    int kind;                      /* CDC_ECM or CDC_NCM */
    u8 comm_iface, data_iface, data_alt;
    u8 mac_string;                 /* iMACAddress, 0 when there is none */
    u16 max_segment;               /* wMaxSegmentSize */
    u8 ncm_caps;                   /* bmNetworkCapabilities (NCM) */
    const struct usb_endpoint_descriptor *notify, *in, *out;
    const struct usb_ss_ep_comp_descriptor *notify_comp, *in_comp, *out_comp;
};
int cdc_find_net_function(const u8 *cfg, u_int len, struct cdc_net_function *f);   /* 0 found (ECM before NCM), -1 none */
bool cdc_parse_mac(const char *hex, u8 *mac);   /* "0050B6123456" */

/* NCM 16-bit transfer blocks (USB CDC NCM 1.0, section 3) */
struct ncm_out_params {
    u_int max;                     /* dwNtbOutMaxSize */
    u_int align;                   /* wNdpOutAlignment */
    u_int div, rem;                /* wNdpOutDivisor, wNdpOutPayloadRemainder */
    u_int mps;                     /* the bulk OUT packet size: a block must end in a short packet */
};
u_int ncm_build_ntb16(u8 *out, u_int cap, u16 seq, const u8 *frame, u_int len, const struct ncm_out_params *p);
int ncm_parse_ntb16(const u8 *ntb, u_int len, void (*fn)(void *arg, const u8 *frame, u_int len), void *arg);

#endif
