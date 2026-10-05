/*
SurfOS USB Ethernet Driver: CDC ECM and NCM
--------------------
File: usbnet.c  Date: 10/5/26 (roadmap M7: USB networking)
--------------------
USB to Ethernet adapters that speak a USB-IF class instead of a vendor protocol:
CDC ECM (one Ethernet frame per bulk transfer) and CDC NCM (frames packed into
transfer blocks). Realtek RTL8153 adapters, the commonest gigabit ones, offer a
CDC ECM configuration next to Realtek's own; QEMU's usb-net is ECM too.

Each adapter is a netdev (eth0, ...). Received frames go to netdev_rx() from the
controller's poll; a frame to send is copied into a free transmit buffer and
queued at once. The adapter's notification endpoint reports the cable's link
state. An adapter that is unplugged and plugged in again gets its interface
name back, and with the same MAC address its address configuration too.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/irq.h>
#include <surfos/timer.h>
#include <sys/usb.h>
#include <mm/kalloc.h>
#include <net/net.h>
#include <blibc_common.h>

#define UNITS       4
#define TX_BUFS     16
#define RX_ECM      16             /* ECM: a transfer holds one frame */
#define RX_NCM      4              /* NCM: a transfer holds a block of frames */
#define RX_MAX      16
#define FRAME_BUF    2048           /* a frame, or a one-frame NCM block */
#define NCM_IN_SIZE 16384          /* the receive block size asked of an NCM adapter */
#define ERR_LIMIT   16             /* failed transfers in a row before a direction gives up */

/* CDC requests and notifications (CDC 1.2, ECM 1.2, NCM 1.0) */
#define CDC_SET_ETHERNET_PACKET_FILTER 0x43
#define CDC_PACKET_TYPE_DIRECTED   (1 << 2)
#define CDC_PACKET_TYPE_BROADCAST  (1 << 3)
#define NCM_GET_NTB_PARAMETERS     0x80
#define NCM_SET_NTB_FORMAT         0x84
#define NCM_SET_NTB_INPUT_SIZE     0x86
#define NCM_NCAP_NTB_INPUT_SIZE    (1 << 5)
#define CDC_NOTIFY_NETWORK_CONNECTION 0x00
#define CDC_NOTIFY_SPEED_CHANGE    0x2a

#define CLASS_OUT (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE)
#define CLASS_IN  (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE)

struct usbnet {
    struct netdev dev;                 /* registered once; it stays when the adapter goes */
    struct usb_device *udev;           /* NULL while unplugged */
    int kind;                          /* CDC_ECM or CDC_NCM */
    struct usb_endpoint *in, *out, *notify;
    struct usb_xfer rx[RX_MAX], tx[TX_BUFS], irq;
    u_int nrx, rx_size;
    u8 *rx_bufs, *tx_bufs, *irq_buf;
    void *rx_raw, *tx_raw, *irq_raw;
    volatile bool tx_busy[TX_BUFS];
    volatile u_int rx_errors, irq_errors;
    volatile bool link_changed;
    bool link_reported, rx_stop_reported;
    volatile u32 speed;                /* bit/s, from the last speed notification */
    struct ncm_out_params ncm;
    u16 ntb_seq;
};

static struct usbnet *units[UNITS];

/* zeroed DMA memory aligned to align (a power of two), so no buffer crosses a 64 KB boundary */
static u8 *dma_block(u_int size, u_int align, void **raw) {
    u8 *p = (u8 *)kalloc_dma(size + align), *a;
    *raw = p;
    if(!p) return NULL;
    a = (u8 *)(((u_long)p + align - 1) & ~(u_long)(align - 1));
    memset(a, 0, size);
    return a;
}

static const char *kind_name(int kind) {
    return kind == CDC_NCM ? "cdc-ncm" : "cdc-ecm";
}

/**** data path: interrupts off, from the controller's poll or under irq_save ****/

static void ncm_frame(void *arg, const u8 *frame, u_int len) {
    struct usbnet *un = (struct usbnet *)arg;
    netdev_rx(&un->dev, frame, len);
}

static void rx_done(struct usb_xfer *x) {
    struct usbnet *un = (struct usbnet *)x->ctx;
    if(x->status == USB_OK) {
        un->rx_errors = 0;
        if(un->kind == CDC_NCM) {
            if(x->actual && ncm_parse_ntb16((const u8 *)x->buf, x->actual, ncm_frame, un) < 0) un->dev.rx_dropped++;
        } else if(x->actual >= ETH_HLEN) {
            netdev_rx(&un->dev, x->buf, x->actual);
        }
        /* an empty transfer ends a frame that filled its last packet: nothing to pass on */
    } else if(x->status == USB_ENODEV) {
        return;
    } else if(x->status == USB_ECANCEL) {
        /* stopped while an error was being recovered from: the buffer goes back in */
    } else {
        un->dev.rx_dropped++;
        if(++un->rx_errors >= ERR_LIMIT) {
            un->link_changed = true;           /* the usb thread says so */
            usb_wake();
            return;
        }
    }
    usb_submit(x);                             /* the buffer goes straight back */
}

static void tx_done(struct usb_xfer *x) {
    struct usbnet *un = (struct usbnet *)x->ctx;
    un->tx_busy[x - un->tx] = false;
    if(x->status < 0 && x->status != USB_ENODEV) un->dev.tx_errors++;
}

static void irq_done(struct usb_xfer *x) {
    struct usbnet *un = (struct usbnet *)x->ctx;
    const u8 *p = (const u8 *)x->buf;
    if(x->status == USB_OK) {
        un->irq_errors = 0;
        if(x->actual >= 8 && p[0] == CLASS_IN) {
            if(p[1] == CDC_NOTIFY_NETWORK_CONNECTION) {
                bool up = (p[2] | p[3]) != 0;
                if(up != un->dev.link_up) {
                    un->dev.link_up = up;
                    un->link_changed = true;
                    usb_wake();
                }
            } else if(p[1] == CDC_NOTIFY_SPEED_CHANGE && x->actual >= 16) {
                un->speed = (u32)p[8] | ((u32)p[9] << 8) | ((u32)p[10] << 16) | ((u32)p[11] << 24);
            }
        }
    } else if(x->status == USB_ENODEV) {
        return;
    } else if(x->status != USB_ECANCEL && ++un->irq_errors >= ERR_LIMIT) {
        return;
    }
    usb_submit(x);
}

static int usbnet_send(struct netdev *nd, struct netbuf *nb) {
    struct usbnet *un = (struct usbnet *)nd->priv;
    u_int i, n = 0;
    u_long f;
    int r = -1;
    if(nb->len < ETH_HLEN || nb->len > ETH_HLEN + ETH_MTU + 4) return -1;
    f = irq_save();
    if(un->udev && !un->udev->gone && un->out) {
        for(i = 0; i < TX_BUFS && un->tx_busy[i]; i++);
        if(i == TX_BUFS) {                     /* all in flight: collect the finished ones now */
            usb_poll();
            for(i = 0; i < TX_BUFS && un->tx_busy[i]; i++);
        }
        if(i < TX_BUFS) {
            u8 *b = un->tx_bufs + i * FRAME_BUF;
            if(un->kind == CDC_NCM) {
                n = ncm_build_ntb16(b, FRAME_BUF, un->ntb_seq, netbuf_data(nb), nb->len, &un->ncm);
                un->ntb_seq++;
            } else {
                memcpy(b, netbuf_data(nb), nb->len);
                n = nb->len;
                if(n % un->out->mps == 0) b[n++] = 0;   /* end in a short packet: one byte of padding, as Linux does */
            }
            if(n) {
                un->tx[i].length = n;
                un->tx_busy[i] = true;
                r = usb_submit(&un->tx[i]);
                if(r) un->tx_busy[i] = false;
            }
        }
    }
    irq_restore(f);
    return r ? -1 : 0;
}

static const struct netdev_ops usbnet_ops = { usbnet_send, NULL };

/**** attaching (the usb thread, or boot) ****/

static bool usbnet_match(struct usb_device *dev, const u8 *cfg, u_int len) {
    struct cdc_net_function fn;
    return cdc_find_net_function(cfg, len, &fn) == 0;
}

/* NCM: the transfer block sizes and alignment rules (with the data interface still in its
   alternate setting 0), 16-bit blocks, and receive blocks of at most NCM_IN_SIZE */
static int ncm_setup(struct usb_device *udev, const struct cdc_net_function *fn, struct ncm_out_params *ncm, u_int *rx_size) {
    u8 p[28], v[8];
    u32 in_max, size;
    u16 formats;
    int r = usb_control(udev, CLASS_IN, NCM_GET_NTB_PARAMETERS, 0, fn->comm_iface, p, sizeof(p));
    if(r < 0) return r;
    if(r < (int)sizeof(p)) return USB_EIO;
    formats = (u16)(p[2] | (p[3] << 8));
    in_max = (u32)p[4] | ((u32)p[5] << 8) | ((u32)p[6] << 16) | ((u32)p[7] << 24);
    ncm->max = (u32)p[16] | ((u32)p[17] << 8) | ((u32)p[18] << 16) | ((u32)p[19] << 24);
    ncm->div = p[20] | (p[21] << 8);
    ncm->rem = p[22] | (p[23] << 8);
    ncm->align = p[24] | (p[25] << 8);
    if(!(formats & 1) || in_max < 2048) return USB_EINVAL;
    if(formats & 2) usb_control(udev, CLASS_OUT, NCM_SET_NTB_FORMAT, 0, fn->comm_iface, NULL, 0);   /* 16-bit blocks */
    size = in_max > NCM_IN_SIZE ? NCM_IN_SIZE : in_max;
    if(size != in_max) {
        memset(v, 0, sizeof(v));
        v[0] = (u8)size; v[1] = (u8)(size >> 8); v[2] = (u8)(size >> 16);
        if(usb_control(udev, CLASS_OUT, NCM_SET_NTB_INPUT_SIZE, 0, fn->comm_iface, v,
                       (fn->ncm_caps & NCM_NCAP_NTB_INPUT_SIZE) ? 8 : 4) < 0)
            size = in_max > 65536 ? 65536 : in_max;          /* it keeps its own size */
    }
    for(*rx_size = 2048; *rx_size < size; *rx_size <<= 1);
    return 0;
}

/* this adapter's unit: the one it had before (same MAC), else a free one, else a new one */
static struct usbnet *unit_for(const u8 *mac) {
    struct usbnet *un = NULL;
    int i;
    for(i = 0; i < UNITS; i++) if(units[i] && !units[i]->udev && !memcmp(units[i]->dev.mac, mac, ETH_ALEN)) return units[i];
    for(i = 0; i < UNITS; i++) if(units[i] && !units[i]->udev) { un = units[i]; break; }
    if(un) {
        /* another adapter takes the name: the old address configuration was not for it */
        mutex_lock(&net_lock);
        un->dev.ip = un->dev.netmask = un->dev.gateway = un->dev.dns = 0;
        memcpy(un->dev.mac, mac, ETH_ALEN);
        mutex_unlock(&net_lock);
        return un;
    }
    for(i = 0; i < UNITS && units[i]; i++);
    if(i == UNITS) return NULL;
    un = (struct usbnet *)kcalloc(1, sizeof(struct usbnet));
    if(!un) return NULL;
    un->tx_bufs = dma_block(TX_BUFS * FRAME_BUF, FRAME_BUF, &un->tx_raw);
    un->irq_buf = dma_block(64, 64, &un->irq_raw);
    if(!un->tx_bufs || !un->irq_buf) {
        if(un->tx_raw) kfree_dma(un->tx_raw);
        if(un->irq_raw) kfree_dma(un->irq_raw);
        kfree(un);
        return NULL;
    }
    memcpy(un->dev.mac, mac, ETH_ALEN);
    un->dev.ops = &usbnet_ops;
    un->dev.priv = un;
    un->dev.mtu = ETH_MTU;
    netdev_register(&un->dev);
    units[i] = un;
    return un;
}

static int usbnet_attach(struct usb_device *udev) {
    struct cdc_net_function fn;
    struct usb_endpoint *in, *out, *notify = NULL;
    struct usbnet *un;
    struct ncm_out_params ncm;
    const char *stage;
    char hex[16], m[18];
    u8 mac[ETH_ALEN];
    u_int i, rx_size;
    u_long f;
    int r;

    if(cdc_find_net_function(udev->config, udev->config_len, &fn) != 0) return USB_EINVAL;
    /* xHCI wants the endpoints before SET_CONFIGURATION */
    stage = "setting up its endpoints";
    in = usb_endpoint_add(udev, fn.in, fn.in_comp);
    out = usb_endpoint_add(udev, fn.out, fn.out_comp);
    if(fn.notify) notify = usb_endpoint_add(udev, fn.notify, fn.notify_comp);
    r = USB_ENOMEM;
    if(!in || !out) goto fail;
    if((r = usb_configure_endpoints(udev)) != 0) goto fail;
    stage = "SET_CONFIGURATION";
    if((r = usb_set_configuration(udev, udev->config_value)) < 0) goto fail;

    memset(&ncm, 0, sizeof(ncm));
    rx_size = FRAME_BUF;
    if(fn.kind == CDC_NCM) {
        /* the data interface on and off again resets the function (some NCM devices do not
           start cleanly otherwise; Linux's cdc_ncm does the same), then its parameters */
        usb_msleep(10);
        if(fn.data_alt) usb_set_interface(udev, fn.data_iface, fn.data_alt);
        usb_set_interface(udev, fn.data_iface, 0);
        stage = "reading the NCM parameters";
        if((r = ncm_setup(udev, &fn, &ncm, &rx_size)) < 0) goto fail;
    }
    /* the data endpoints live in an alternate setting; a device that has them in the default
       one is there already (and may stall the request) */
    stage = "SET_INTERFACE";
    if(fn.data_alt && (r = usb_set_interface(udev, fn.data_iface, fn.data_alt)) < 0) goto fail;

    /* the adapter's MAC address is the one to use: it only passes frames sent to it */
    if(!fn.mac_string || usb_get_string(udev, fn.mac_string, hex, sizeof(hex)) != 12 || !cdc_parse_mac(hex, mac)) {
        u32 t = (u32)getticks();
        mac[0] = 0x02; mac[1] = 0x53; mac[2] = 0x46;                /* locally administered, "SF" */
        mac[3] = udev->port; mac[4] = (u8)(t >> 8); mac[5] = (u8)t;
    }
    /* directed and broadcast frames; a device that does not know the request sends them anyway */
    usb_control(udev, CLASS_OUT, CDC_SET_ETHERNET_PACKET_FILTER, CDC_PACKET_TYPE_DIRECTED | CDC_PACKET_TYPE_BROADCAST,
                fn.comm_iface, NULL, 0);

    stage = "allocating its buffers";
    r = USB_ENOMEM;
    un = unit_for(mac);
    if(!un || !un->tx_bufs || !un->irq_buf) goto fail;
    un->ncm = ncm;
    un->rx_size = rx_size;
    un->kind = fn.kind;
    un->nrx = fn.kind == CDC_NCM ? RX_NCM : RX_ECM;
    un->rx_bufs = dma_block(un->nrx * un->rx_size, un->rx_size, &un->rx_raw);
    if(!un->rx_bufs) goto fail;
    un->ncm.mps = out->mps;
    un->in = in;
    un->out = out;
    un->notify = notify;
    un->rx_errors = un->irq_errors = 0;
    un->speed = 0;
    un->ntb_seq = 0;
    un->link_reported = un->rx_stop_reported = false;
    un->dev.mtu = (fn.max_segment > ETH_HLEN && fn.max_segment - ETH_HLEN < ETH_MTU) ? fn.max_segment - ETH_HLEN : ETH_MTU;
    for(i = 0; i < TX_BUFS; i++) {
        memset(&un->tx[i], 0, sizeof(struct usb_xfer));
        un->tx[i].ep = out;
        un->tx[i].buf = un->tx_bufs + i * FRAME_BUF;
        un->tx[i].done = tx_done;
        un->tx[i].ctx = un;
        un->tx_busy[i] = false;
    }
    for(i = 0; i < un->nrx; i++) {
        memset(&un->rx[i], 0, sizeof(struct usb_xfer));
        un->rx[i].ep = in;
        un->rx[i].buf = un->rx_bufs + i * un->rx_size;
        un->rx[i].length = un->rx_size;
        un->rx[i].done = rx_done;
        un->rx[i].ctx = un;
    }
    memset(&un->irq, 0, sizeof(struct usb_xfer));
    un->irq.ep = notify;
    un->irq.buf = un->irq_buf;
    un->irq.length = notify ? (notify->mps < 64 ? notify->mps : 64) : 0;
    un->irq.done = irq_done;
    un->irq.ctx = un;

    f = irq_save();
    un->udev = udev;
    un->dev.link_up = notify ? false : true;   /* with no notifications, assume a cable */
    irq_restore(f);
    udev->driver_data = un;
    for(i = 0; i < un->nrx; i++) usb_submit(&un->rx[i]);
    if(notify) usb_submit(&un->irq);
    snprintf(udev->status, sizeof(udev->status), "%s, %s, MAC %s", un->dev.name, kind_name(un->kind), macfmt(un->dev.mac, m));
    return 0;

fail:
    kprintf("+USB: port %u: network adapter: %s failed (%s)\n", udev->port, stage, usb_strerror(r));
    return r < 0 ? r : USB_EIO;
}

/* unplugged; the controller has already let go of the buffers */
static void usbnet_detach(struct usb_device *udev) {
    struct usbnet *un = (struct usbnet *)udev->driver_data;
    u_long f;
    u_int i;
    if(!un) return;
    f = irq_save();
    un->udev = NULL;
    un->in = un->out = un->notify = NULL;
    un->dev.link_up = false;
    for(i = 0; i < TX_BUFS; i++) un->tx_busy[i] = false;
    irq_restore(f);
    if(un->rx_raw && !udev->dma_live) kfree_dma(un->rx_raw);     /* else the controller may still write there */
    un->rx_raw = NULL;
    un->rx_bufs = NULL;
    if(udev->dma_live) {                       /* nor may the transmit buffers be used again */
        un->tx_raw = NULL;
        un->tx_bufs = dma_block(TX_BUFS * FRAME_BUF, FRAME_BUF, &un->tx_raw);
        un->irq_raw = NULL;
        un->irq_buf = dma_block(64, 64, &un->irq_raw);
    }
    udev->driver_data = NULL;
}

/* the usb thread: say what the notifications changed */
static void usbnet_service(struct usb_device *udev) {
    struct usbnet *un = (struct usbnet *)udev->driver_data;
    if(!un || !un->link_changed) return;
    un->link_changed = false;
    if(un->dev.link_up != un->link_reported) {
        un->link_reported = un->dev.link_up;
        if(un->dev.link_up && un->speed >= 1000000) kprintf("%s: link up, %u Mbit/s\n", un->dev.name, un->speed / 1000000);
        else kprintf("%s: link %s\n", un->dev.name, un->dev.link_up ? "up" : "down");
    }
    if(un->rx_errors >= ERR_LIMIT && !un->rx_stop_reported) {
        un->rx_stop_reported = true;
        kprintf("%s: %u receive errors in a row; receiving stopped (unplug and replug the adapter)\n", un->dev.name, un->rx_errors);
    }
}

const struct usb_driver usbnet_driver = { "usbnet", usbnet_match, usbnet_attach, usbnet_detach, usbnet_service };
