/*
SurfOS USB Devices
--------------------
File: usb.c     Date: 10/5/26 (roadmap M7: USB networking)
--------------------
What happens when something is plugged into a root hub port: the port is reset,
the device gets a slot and an address, its descriptors and names are read, and
the first configuration a class driver wants is selected. A device may offer
several: an RTL8153 adapter's first configuration is Realtek's own protocol, its
second is CDC Ethernet. That is done for every connected port at boot, and from
then on by the "usb" thread whenever a port reports a change, which is also how
unplugging is noticed. Hubs are not supported: a device behind one is not seen.

usb=off on the kernel command line leaves the controllers alone.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/irq.h>
#include <surfos/task.h>
#include <surfos/timer.h>
#include <surfos/sync.h>
#include <surfos/interrupt.h>
#include <surfos/multiboot.h>
#include <sys/driver.h>
#include <sys/usb.h>
#include <mm/kalloc.h>
#include <blibc_common.h>

#define CONTROL_TIMEOUT 1000       /* ms for one control transfer */
#define MAX_TRIES 3                /* enumeration attempts before a port is left alone until replugged */

/* the class drivers, asked in order; the list ends with NULL */
static const struct usb_driver *const usb_drivers[] = { &usbnet_driver, &msc_driver, NULL };

static struct usb_hc *hcs;
static event_t usb_event;
static mutex_t usb_lock = MUTEX_INIT("usb");
static bool usb_off, booting;

/**** context ****/

bool usb_can_sleep(void) {
    return curTask && curTask != idle_task && !in_interrupt() && irqs_enabled();
}

void usb_msleep(u_long ms) {
    if(usb_can_sleep()) sleep_ms(ms);
    else mdelay(ms);
}

void usb_wake(void) {
    event_set(&usb_event);
}

void usb_poll(void) {
    struct usb_hc *hc;
    u_long f = irq_save();
    for(hc = hcs; hc; hc = hc->next) xhci_poll(hc);
    irq_restore(f);
}

/**** names ****/

const char *usb_strerror(int err) {
    switch(err) {
    case USB_OK: return "ok";
    case USB_PENDING: return "pending";
    case USB_ESTALL: return "stalled";
    case USB_EIO: return "I/O error";
    case USB_ETIMEOUT: return "timed out";
    case USB_ENODEV: return "device gone";
    case USB_ECANCEL: return "cancelled";
    case USB_ENOMEM: return "out of memory";
    case USB_EINVAL: return "invalid";
    default: return err > 0 ? "short" : "error";
    }
}

const char *usb_speed_name(u_int speed) {
    switch(speed) {
    case USB_SPEED_LOW: return "1.5M";
    case USB_SPEED_FULL: return "12M";
    case USB_SPEED_HIGH: return "480M";
    case USB_SPEED_SUPER: return "5G";
    case USB_SPEED_SUPER_PLUS: return "10G";
    default: return "?";
    }
}

const char *usb_class_name(u8 class_code) {
    switch(class_code) {
    case USB_CLASS_PER_INTERFACE: return "-";
    case USB_CLASS_AUDIO: return "audio";
    case USB_CLASS_COMM: return "comm";
    case USB_CLASS_HID: return "hid";
    case USB_CLASS_PRINTER: return "printer";
    case USB_CLASS_STORAGE: return "storage";
    case USB_CLASS_HUB: return "hub";
    case USB_CLASS_CDC_DATA: return "cdc-data";
    case USB_CLASS_VIDEO: return "video";
    case USB_CLASS_WIRELESS: return "wireless";
    case USB_CLASS_MISC: return "misc";
    case USB_CLASS_VENDOR: return "vendor";
    default: return "other";
    }
}

/**** requests ****/

int usb_control(struct usb_device *dev, u8 type, u8 req, u16 value, u16 index, void *data, u16 len) {
    u8 setup[8];
    setup[0] = type;
    setup[1] = req;
    setup[2] = (u8)value;
    setup[3] = (u8)(value >> 8);
    setup[4] = (u8)index;
    setup[5] = (u8)(index >> 8);
    setup[6] = (u8)len;
    setup[7] = (u8)(len >> 8);
    return xhci_control(dev, setup, data, CONTROL_TIMEOUT);
}

int usb_get_descriptor(struct usb_device *dev, u8 type, u8 index, u16 lang, void *buf, u16 len) {
    return usb_control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR,
                       (u16)((type << 8) | index), lang, buf, len);
}

/* a string descriptor as ASCII (anything else becomes '?'); its length, or USB_E* */
int usb_get_string(struct usb_device *dev, u8 index, char *buf, size_t size) {
    u8 raw[255];
    int n, i, o = 0;
    if(!size) return USB_EINVAL;
    buf[0] = 0;
    if(!index) return USB_EINVAL;
    if(!dev->langid) {
        n = usb_get_descriptor(dev, USB_DT_STRING, 0, 0, raw, sizeof(raw));
        dev->langid = (n >= 4 && raw[1] == USB_DT_STRING) ? (u16)(raw[2] | (raw[3] << 8)) : 0x0409;
    }
    n = usb_get_descriptor(dev, USB_DT_STRING, index, dev->langid, raw, sizeof(raw));
    if(n < 0) return n;
    if(n < 2 || raw[1] != USB_DT_STRING) return USB_EIO;
    if(raw[0] < n) n = raw[0];
    for(i = 2; i + 1 < n && o + 1 < (int)size; i += 2) {
        u16 c = (u16)(raw[i] | (raw[i + 1] << 8));
        buf[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    while(o > 0 && buf[o - 1] == ' ') o--;
    buf[o] = 0;
    return o;
}

int usb_set_configuration(struct usb_device *dev, u8 value) {
    return usb_control(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_CONFIGURATION, value, 0, NULL, 0);
}

int usb_set_interface(struct usb_device *dev, u8 iface, u8 alt) {
    return usb_control(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE, USB_REQ_SET_INTERFACE, alt, iface, NULL, 0);
}

int usb_clear_halt(struct usb_device *dev, u8 ep_addr) {
    return usb_control(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT, USB_REQ_CLEAR_FEATURE,
                       USB_FEATURE_ENDPOINT_HALT, ep_addr, NULL, 0);
}

/**** endpoints and transfers ****/

struct usb_endpoint *usb_endpoint_add(struct usb_device *dev, const struct usb_endpoint_descriptor *d,
                                      const struct usb_ss_ep_comp_descriptor *comp) {
    u_int dci = (d->bEndpointAddress & 0x0f) * 2 + ((d->bEndpointAddress & USB_DIR_IN) ? 1 : 0);
    struct usb_endpoint *ep;
    if(dci < 2 || dci >= USB_MAX_EP) return NULL;
    if(dev->eps[dci]) return dev->eps[dci];
    ep = (struct usb_endpoint *)kcalloc(1, sizeof(struct usb_endpoint));
    if(!ep) return NULL;
    ep->dev = dev;
    ep->addr = d->bEndpointAddress;
    ep->type = d->bmAttributes & 3;
    ep->mps = d->wMaxPacketSize & 0x7ff;
    ep->interval = d->bInterval;
    ep->dci = (u8)dci;
    if(dev->speed >= USB_SPEED_SUPER) ep->burst = comp ? comp->bMaxBurst : 0;
    else if(dev->speed == USB_SPEED_HIGH && (ep->type == USB_EP_INTR || ep->type == USB_EP_ISOC))
        ep->burst = (d->wMaxPacketSize >> 11) & 3;
    if(!ep->mps || xhci_endpoint_init(ep) != 0) {
        kfree(ep);
        return NULL;
    }
    dev->eps[dci] = ep;
    return ep;
}

int usb_configure_endpoints(struct usb_device *dev) {
    return xhci_configure_endpoints(dev);
}

int usb_submit(struct usb_xfer *x) {
    return xhci_submit(x);
}

int usb_bulk_sync(struct usb_endpoint *ep, void *buf, u_int len, u_int timeout_ms) {
    return xhci_bulk_sync(ep, buf, len, timeout_ms);
}

/* after a transfer halted ep: reset it now (or wait while another task does) */
int usb_endpoint_recover(struct usb_endpoint *ep, u_int timeout_ms) {
    u_long start = getticks();
    for(;;) {
        int r;
        if(ep->dev->gone) return USB_ENODEV;
        r = xhci_recover(ep);
        if(r != USB_PENDING) return r;
        if((getticks() - start) * (1000 / HZ) >= timeout_ms) return USB_ETIMEOUT;
        usb_msleep(10);
    }
}

/* both ends of an endpoint back to data toggle / sequence number 0 */
int usb_endpoint_reset(struct usb_endpoint *ep) {
    int r = xhci_endpoint_reset(ep);
    if(r == 0) r = usb_clear_halt(ep->dev, ep->addr);
    return r < 0 ? r : 0;
}

/**** enumeration ****/

/* the configurations, in order; the first one a driver matches is used */
static void choose_driver(struct usb_device *dev) {
    u_int i, d, n = dev->desc.bNumConfigurations;
    u8 hdr[9];
    strlcpy(dev->status, "no driver", sizeof(dev->status));
    if(n > 8) n = 8;
    for(i = 0; i < n; i++) {
        u_int total;
        u8 *cfg;
        int r = usb_get_descriptor(dev, USB_DT_CONFIG, (u8)i, 0, hdr, sizeof(hdr));
        if(r < 9 || hdr[1] != USB_DT_CONFIG) continue;
        total = hdr[2] | (hdr[3] << 8);
        if(total < 9 || total > 4096) continue;
        cfg = (u8 *)kalloc(total);
        if(!cfg) return;
        r = usb_get_descriptor(dev, USB_DT_CONFIG, (u8)i, 0, cfg, (u16)total);
        if(r != (int)total) {
            kfree(cfg);
            continue;
        }
        if(i == 0 && !dev->class_code) dev->class_code = usb_config_class(cfg, total);
        for(d = 0; usb_drivers[d]; d++) {
            if(!usb_drivers[d]->match(dev, cfg, total)) continue;
            dev->config = cfg;
            dev->config_len = total;
            dev->config_value = cfg[5];
            dev->driver = usb_drivers[d];
            if(dev->driver->attach(dev) == 0) return;
            snprintf(dev->status, sizeof(dev->status), "%s did not start", dev->driver->name);
            dev->driver = NULL;
            dev->config = NULL;
            break;
        }
        kfree(cfg);
        if(usb_drivers[d]) return;             /* a driver matched but did not start */
    }
    if(dev->class_code == USB_CLASS_HUB) strlcpy(dev->status, "hub: not supported", sizeof(dev->status));
}

/* a slot, an address, the descriptors, a driver; the device, or NULL after a message */
static struct usb_device *enumerate(struct usb_hc *hc, u_int port, int speed) {
    struct usb_device *dev = (struct usb_device *)kcalloc(1, sizeof(struct usb_device));
    const char *stage;
    u8 d8[8];
    int r;
    if(!dev) return NULL;
    dev->hc = hc;
    dev->port = (u8)port;
    dev->speed = (u8)speed;
    dev->ep0.dev = dev;
    dev->ep0.dci = 1;
    dev->ep0.type = USB_EP_CONTROL;
    dev->ep0.mps = speed >= USB_SPEED_SUPER ? 512 : (speed == USB_SPEED_LOW ? 8 : 64);
    dev->eps[1] = &dev->ep0;
    mutex_init(&dev->ep0_lock, "usb ep0");

    stage = "getting a device slot";
    if((r = xhci_enable_slot(dev)) != 0) goto fail;
    stage = "addressing";
    if((r = xhci_address_device(dev)) != 0) goto fail;
    usb_msleep(10);                        /* SET_ADDRESS recovery: some devices need it */
    stage = "reading the device descriptor";
    if((r = usb_get_descriptor(dev, USB_DT_DEVICE, 0, 0, d8, sizeof(d8))) < 8) goto fail;
    {
        /* endpoint 0's packet size: full speed devices may use 8, 16, 32 or 64 */
        u_int mps = speed >= USB_SPEED_SUPER ? (d8[7] < 16 ? 1u << d8[7] : 512) : d8[7];
        if(mps != dev->ep0.mps && (mps == 8 || mps == 16 || mps == 32 || mps == 64 || mps == 512)) {
            stage = "setting endpoint 0's packet size";
            if((r = xhci_set_ep0_mps(dev, mps)) != 0) goto fail;
        }
    }
    stage = "reading the device descriptor";
    if((r = usb_get_descriptor(dev, USB_DT_DEVICE, 0, 0, &dev->desc, sizeof(dev->desc))) < (int)sizeof(dev->desc)) goto fail;
    if(dev->desc.iManufacturer) usb_get_string(dev, dev->desc.iManufacturer, dev->manufacturer, sizeof(dev->manufacturer));
    if(dev->desc.iProduct) usb_get_string(dev, dev->desc.iProduct, dev->product, sizeof(dev->product));
    if(dev->desc.iSerialNumber) usb_get_string(dev, dev->desc.iSerialNumber, dev->serial, sizeof(dev->serial));
    dev->class_code = dev->desc.bDeviceClass;
    choose_driver(dev);
    return dev;

fail:
    kprintf("+USB: port %u: %s failed (%s)\n", port, stage, usb_strerror(r >= 0 ? USB_EIO : r));
    xhci_release_device(dev);
    kfree(dev);
    return NULL;
}

static void port_connect(struct usb_hc *hc, u_int port) {
    struct usb_device *dev;
    int speed;
    if(hc->port_tries[port] >= MAX_TRIES) return;
    hc->port_tries[port]++;
    speed = xhci_port_enable(hc, port);
    if(speed < 0) {
        if(speed != USB_ENODEV) kprintf("+USB: port %u: the port did not come up (%s)\n", port, usb_strerror(speed));
        return;
    }
    dev = enumerate(hc, port, speed);
    if(!dev) return;
    hc->port_dev[port] = dev;
    kprintf("+USB: port %u: %04x:%04x %s%s%s, %s: %s\n", port, dev->desc.idVendor, dev->desc.idProduct,
            dev->manufacturer, dev->manufacturer[0] ? " " : "", dev->product[0] ? dev->product : "(no name)",
            usb_speed_name(dev->speed), dev->status);
}

static void port_disconnect(struct usb_hc *hc, u_int port) {
    struct usb_device *dev = hc->port_dev[port];
    u_long f;
    if(!dev) return;
    f = irq_save();
    dev->gone = true;
    irq_restore(f);
    if(dev->driver && dev->driver->quiesce) dev->driver->quiesce(dev);   /* no task inside it any more */
    xhci_release_device(dev);              /* the controller lets go first: no more callbacks */
    if(dev->driver) dev->driver->detach(dev);
    kprintf("+USB: port %u: %04x:%04x %s unplugged (%s)\n", port, dev->desc.idVendor, dev->desc.idProduct,
            dev->product[0] ? dev->product : "(no name)", dev->status);
    hc->port_dev[port] = NULL;
    if(dev->config) kfree(dev->config);
    kfree(dev);
}

/* What is on the port now against what we know. Every change bit is acknowledged (while
   one stays set the controller reports no further changes). A device whose port is no longer
   enabled (the controller disabled it after an error, or a USB 3 link dropped) is dropped
   and set up again, up to MAX_TRIES times until it is unplugged. */
static void port_check(struct usb_hc *hc, u_int port) {
    u32 v = xhci_port_status(hc, port);
    bool connected = (v & USB_PS_CONNECTED) != 0, changed = (v & USB_PS_CONNECT_CHANGE) != 0;
    if(v & USB_PS_CHANGES) xhci_port_ack(hc, port);
    if(hc->port_dev[port] && (!connected || changed || !(v & USB_PS_ENABLED))) {
        if(connected && !changed) kprintf("+USB: port %u: the port was disabled\n", port);
        port_disconnect(hc, port);
    }
    if(!connected) {
        hc->port_tries[port] = 0;
        return;
    }
    if(hc->port_dev[port]) return;
    if(changed) hc->port_tries[port] = 0;
    if(hc->port_tries[port] >= MAX_TRIES) return;
    if(!booting) {
        usb_msleep(100);                   /* let the plug settle (USB 2.0 TATTDB) */
        if(!(xhci_port_status(hc, port) & USB_PS_CONNECTED)) return;
    }
    port_connect(hc, port);
}

/* a controller that stopped: forget what was on it */
static void controller_gone(struct usb_hc *hc) {
    u_int p;
    bool any = false;
    for(p = 1; p <= hc->nports; p++) if(hc->port_dev[p]) any = true;
    if(!any) return;
    kprintf("+USB: %s stopped working (%s)\n", hc->name, hc->failed_why ? hc->failed_why : "?");
    for(p = 1; p <= hc->nports; p++) port_disconnect(hc, p);
}

static void usb_thread(void *arg) {
    struct usb_hc *hc;
    for(;;) {
        event_wait_timeout(&usb_event, 1000);
        event_clear(&usb_event);
        mutex_lock(&usb_lock);
        for(hc = hcs; hc; hc = hc->next) {
            u_int p;
            if(hc->failed) {
                controller_gone(hc);
                continue;
            }
            xhci_service(hc);
            for(p = 1; p <= hc->nports; p++) port_check(hc, p);
            for(p = 1; p <= hc->nports; p++) {
                struct usb_device *dev = hc->port_dev[p];
                if(dev && dev->driver && dev->driver->service) dev->driver->service(dev);
            }
        }
        mutex_unlock(&usb_lock);
    }
}

/* the driver table entry: the controllers, what is plugged in now, then the thread */
int init_usb(void) {
    struct usb_hc *hc;
    u_int p;
    event_init(&usb_event);
    if(bootinfo_has_arg(&bootinfo, "usb=off")) {
        usb_off = true;
        kprintf("+USB: off (usb=off on the command line)\n");
        return DRV_SKIPPED;
    }
    hcs = xhci_probe();
    if(!hcs) return 1;
    booting = true;
    for(hc = hcs; hc; hc = hc->next) {
        for(p = 1; p <= hc->nports; p++) port_check(hc, p);
    }
    booting = false;
    kthread_create("usb", usb_thread, NULL, PL_NORMAL, TF_KTHREAD | TF_DETACHED);
    return 0;
}

/* For selftest: a request no device supports (GET_STATUS of interface 0xee, a request
   error the device must stall) and then the device descriptor again, which shows that
   endpoint 0 was stopped and moved past the stalled transfer. 0 when that worked,
   USB_ENODEV when no device stalled the request, else what went wrong. */
int usb_stall_test(void) {
    struct usb_hc *hc;
    struct usb_device_descriptor d;
    u_int p;
    u8 st[2];
    int r = USB_ENODEV;
    mutex_lock(&usb_lock);
    for(hc = hcs; hc && r == USB_ENODEV; hc = hc->next) {
        for(p = 1; p <= hc->nports && r == USB_ENODEV; p++) {
            struct usb_device *dev = hc->port_dev[p];
            if(!dev || dev->gone) continue;
            if(usb_control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE, USB_REQ_GET_STATUS, 0, 0xee, st, 2) != USB_ESTALL)
                continue;                          /* it answered: no use for this test */
            r = usb_get_descriptor(dev, USB_DT_DEVICE, 0, 0, &d, sizeof(d));
            if(r == (int)sizeof(d) && !memcmp(&d, &dev->desc, sizeof(d))) r = 0;
            else if(r >= 0) r = USB_EIO;
        }
    }
    mutex_unlock(&usb_lock);
    return r;
}

/**** reports ****/

void usb_print(void) {
    struct usb_hc *hc;
    if(usb_off) { printf("    USB is off (usb=off on the kernel command line)\n"); return; }
    if(!hcs) { printf("    no USB host controller\n"); return; }
    mutex_lock(&usb_lock);
    for(hc = hcs; hc; hc = hc->next) {
        char d[128];
        u_int p, n = 0;
        xhci_describe(hc, d, sizeof(d));
        printf("    %s\n", d);
        if(hc->failed) printf("    stopped: %s\n", hc->failed_why ? hc->failed_why : "?");
        printf("    port  USB  speed  id         class     device\n");
        for(p = 1; p <= hc->nports; p++) {
            struct usb_device *dev = hc->port_dev[p];
            if(!dev) {
                if(xhci_port_status(hc, p) & USB_PS_CONNECTED)
                    printf("    %4u  %u    -      (connected, not set up)\n", p, hc->port_proto[p] ? hc->port_proto[p] : 2);
                continue;
            }
            n++;
            printf("    %4u  %u    %-5s  %04x:%04x  %-8s  %s%s%s\n", p, hc->port_proto[p] ? hc->port_proto[p] : 2,
                   usb_speed_name(dev->speed), dev->desc.idVendor, dev->desc.idProduct, usb_class_name(dev->class_code),
                   dev->manufacturer, dev->manufacturer[0] ? " " : "", dev->product[0] ? dev->product : "(no name)");
            printf("                                          -> %s\n", dev->status);
        }
        if(!n) printf("    (nothing plugged in)\n");
    }
    mutex_unlock(&usb_lock);
}

/* 10/7/26: the devices plugged in, at most max, copied while the lock is held: the caller prints
   them afterwards, so a pager's question never holds the lock (wifiinfo) */
u_int usb_snapshot(struct usb_info *out, u_int max) {
    struct usb_hc *hc;
    u_int n = 0, p;
    if(usb_off || !hcs) return 0;
    mutex_lock(&usb_lock);
    for(hc = hcs; hc && n < max; hc = hc->next) {
        for(p = 1; p <= hc->nports && n < max; p++) {
            struct usb_device *dev = hc->port_dev[p];
            if(!dev) continue;
            out[n].port = (u8)p;
            out[n].class_code = dev->class_code;
            out[n].vendor = dev->desc.idVendor;
            out[n].product = dev->desc.idProduct;
            snprintf(out[n].name, sizeof(out[n].name), "%s%s%s", dev->manufacturer, dev->manufacturer[0] ? " " : "",
                     dev->product[0] ? dev->product : "(no name)");
            strlcpy(out[n].status, dev->status, sizeof(out[n].status));
            n++;
        }
    }
    mutex_unlock(&usb_lock);
    return n;
}

const char *usb_describe(char *buf, size_t size) {
    struct usb_hc *hc;
    u_int nhc = 0, ndev = 0, p;
    if(usb_off) { snprintf(buf, size, "off (usb=off)"); return buf; }
    for(hc = hcs; hc; hc = hc->next) {
        nhc++;
        for(p = 1; p <= hc->nports; p++) if(hc->port_dev[p]) ndev++;
    }
    if(!nhc) snprintf(buf, size, "no xHCI controller");
    else snprintf(buf, size, "%u xHCI controller%s, %u device%s (lsusb)", nhc, nhc == 1 ? "" : "s", ndev, ndev == 1 ? "" : "s");
    return buf;
}
