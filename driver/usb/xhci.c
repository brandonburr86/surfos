/*
SurfOS xHCI USB Host Controller Driver
--------------------
File: xhci.c    Date: 10/5/26 (roadmap M7: USB networking)
--------------------
The USB 3 host controller every current PC has (PCI class 0c, subclass 03,
interface 30), programmed from the xHCI 1.2 specification: a command ring, one
event ring, a transfer ring per endpoint and the device contexts, all in the 1:1
DMA heap so their addresses are physical. Its interrupt is not used: the event
ring is polled from a kernel timer on every tick, by whoever waits for a command
or a control transfer, and by the network driver when it wants a transmit
buffer back. That works the same whether or not the controller's interrupt
would reach the CPU (on the laptop the I/O APIC stays masked).

Taken over from the firmware with the USB legacy support handshake, then reset.
Root hub ports only: no hubs, no streams, no isochronous endpoints. The device
layer above it (enumeration, drivers, the usb thread) is driver/usb/usb.c.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/irq.h>
#include <surfos/task.h>
#include <surfos/timer.h>
#include <surfos/ktimer.h>
#include <surfos/sync.h>
#include <sys/pci.h>
#include <sys/usb.h>
#include <mm/paging.h>
#include <mm/kalloc.h>
#include <blibc_common.h>

/* capability registers (byte offsets) */
#define CAP_CAPLENGTH  0x00
#define CAP_HCSPARAMS1 0x04
#define CAP_HCSPARAMS2 0x08
#define CAP_HCCPARAMS1 0x10
#define CAP_DBOFF      0x14
#define CAP_RTSOFF     0x18
#define HCC_AC64       (1 << 0)
#define HCC_CSZ        (1 << 2)
#define HCC_PPC        (1 << 3)

/* operational registers (u32 indexes) */
#define OP_USBCMD      (0x00 / 4)
#define OP_USBSTS      (0x04 / 4)
#define OP_PAGESIZE    (0x08 / 4)
#define OP_CRCR_LO     (0x18 / 4)
#define OP_CRCR_HI     (0x1c / 4)
#define OP_DCBAAP_LO   (0x30 / 4)
#define OP_DCBAAP_HI   (0x34 / 4)
#define OP_CONFIG      (0x38 / 4)
#define OP_PORTSC(p)   ((0x400 + 0x10 * ((p) - 1)) / 4)
#define CMD_RS         (1 << 0)
#define CMD_HCRST      (1 << 1)
#define STS_HCH        (1 << 0)
#define STS_HSE        (1 << 2)
#define STS_EINT       (1 << 3)
#define STS_PCD        (1 << 4)
#define STS_CNR        (1 << 11)
#define STS_HCE        (1 << 12)

/* runtime registers: interrupter 0 (u32 indexes) */
#define RT_IMAN        (0x20 / 4)
#define RT_IMOD        (0x24 / 4)
#define RT_ERSTSZ      (0x28 / 4)
#define RT_ERSTBA_LO   (0x30 / 4)
#define RT_ERSTBA_HI   (0x34 / 4)
#define RT_ERDP_LO     (0x38 / 4)
#define RT_ERDP_HI     (0x3c / 4)
#define ERDP_EHB       (1 << 3)

/* PORTSC */
#define PS_CCS         (1u << 0)
#define PS_PED         (1u << 1)
#define PS_PR          (1u << 4)
#define PS_PLS(v)      (((v) >> 5) & 0xf)
#define PS_PP          (1u << 9)
#define PS_SPEED(v)    (((v) >> 10) & 0xf)
#define PS_CSC         (1u << 17)
#define PS_PEC         (1u << 18)
#define PS_WRC         (1u << 19)
#define PS_OCC         (1u << 20)
#define PS_PRC         (1u << 21)
#define PS_PLC         (1u << 22)
#define PS_CEC         (1u << 23)
#define PS_WPR         (1u << 31)
#define PS_CHANGES     (PS_CSC | PS_PEC | PS_WRC | PS_OCC | PS_PRC | PS_PLC | PS_CEC)
/* what a PORTSC write carries back: the read-only and read/write bits. Everything else is
   written as 0, so a write never disables the port (PED is write-1-to-clear) or
   acknowledges a change by accident. */
#define PS_KEEP        ((1u << 0) | (1u << 3) | (0xfu << 5) | (1u << 9) | (0xfu << 10) | (3u << 14) | (7u << 25) | (1u << 30))

/* extended capabilities */
#define XCAP_LEGACY    1
#define XCAP_PROTOCOL  2
#define LEGACY_BIOS_OWNED (1u << 16)
#define LEGACY_OS_OWNED   (1u << 24)
#define LEGACY_SMI_KEEP   ((7u << 1) | (0xffu << 5) | (7u << 17))   /* USBLEGCTLSTS: everything but the SMI enables */
#define LEGACY_SMI_EVENTS (7u << 29)                                /* write-1-to-clear SMI events */

/* TRBs */
#define TRB_C          (1u << 0)
#define TRB_TC         (1u << 1)
#define TRB_ISP        (1u << 2)
#define TRB_IOC        (1u << 5)
#define TRB_IDT        (1u << 6)
#define TRB_ED         (1u << 2)            /* in an event */
#define TRB_TYPE(t)    ((u32)(t) << 10)
#define TRB_GET_TYPE(c) (((c) >> 10) & 0x3f)
#define TRB_DIR_IN     (1u << 16)
#define TRB_TRT(t)     ((u32)(t) << 16)
#define TRB_EP(dci)    ((u32)(dci) << 16)
#define TRB_SLOT(s)    ((u32)(s) << 24)
enum {
    TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_LINK = 6,
    TRB_ENABLE_SLOT = 9, TRB_DISABLE_SLOT = 10, TRB_ADDRESS_DEVICE = 11, TRB_CONFIGURE_EP = 12,
    TRB_EVALUATE_CTX = 13, TRB_RESET_EP = 14, TRB_STOP_EP = 15, TRB_SET_TR_DEQ = 16,
    TRB_TRANSFER_EVENT = 32, TRB_CMD_COMPLETION = 33, TRB_PORT_STATUS = 34, TRB_HC_EVENT = 37
};

/* completion codes */
#define CC_SUCCESS       1
#define CC_DATA_BUFFER   2
#define CC_BABBLE        3
#define CC_TRANSACTION   4
#define CC_TRB           5
#define CC_STALL         6
#define CC_SHORT_PACKET  13
#define CC_STOPPED       26
#define CC_STOPPED_LEN   27
#define CC_STOPPED_SHORT 28
#define CC_SPLIT         36

/* endpoint context: types and states */
#define EPT_ISOC_OUT 1
#define EPT_BULK_OUT 2
#define EPT_INTR_OUT 3
#define EPT_CONTROL  4
#define EPT_ISOC_IN  5
#define EPT_BULK_IN  6
#define EPT_INTR_IN  7
#define EPS_RUNNING  1
#define EPS_HALTED   2
#define EPS_STOPPED  3
#define EPS_ERROR    4

#define CMD_TRBS     64
#define EVT_TRBS     256
#define EP0_TRBS     64
#define EP_TRBS      256
#define CMD_TIMEOUT  5000              /* ms; a command that takes longer means the controller is gone */

struct erst_entry {
    u32 base_lo, base_hi, size, rsvd;
};

struct xhci {
    struct usb_hc hc;                  /* first: the device layer's view */
    struct pci_dev *pci;
    volatile u8 *cap;
    volatile u32 *op, *rt, *db;
    u64 regs_phys;
    u_int max_slots, csz, page_size, nscratch;
    u16 version;
    u32 hcs1, hcs2, hcc1;
    u64 *dcbaa;
    void *dcbaa_raw;
    u64 *scratch;
    void *scratch_raw, *scratch_pages_raw;
    struct usb_ring cmd;
    struct xhci_trb *evt;
    void *evt_raw;
    u_int evt_deq;
    u32 evt_ccs;
    struct erst_entry *erst;
    void *erst_raw;
    struct usb_device *slot_dev[256];
    mutex_t cmd_lock;
    volatile bool cmd_busy;
    volatile int cmd_done;
    volatile u32 cmd_cc, cmd_slot, cmd_trb;
    struct ktimer poll;
    u_long events, hc_events;
};

#define XHC(h) ((struct xhci *)(h))

static inline u32 phys(const void *p) { return (u32)(u_long)p; }    /* the DMA heap is mapped 1:1 below 1 GB */
static inline u32 cap32(struct xhci *x, u_int off) { return *(volatile u32 *)(x->cap + off); }
static inline u32 portsc(struct xhci *x, u_int port) { return x->op[OP_PORTSC(port)]; }
static inline void portsc_write(struct xhci *x, u_int port, u32 v) { x->op[OP_PORTSC(port)] = v; }
#define barrier() asm volatile("" : : : "memory")

/* zeroed DMA heap memory aligned to `align` (a power of two); *raw is what kfree_dma() takes */
static void *dma_zalloc(u_int size, u_int align, void **raw) {
    u8 *p = (u8 *)kalloc_dma(size + align), *a;
    *raw = p;
    if(!p) return NULL;
    a = (u8 *)(((u_long)p + align - 1) & ~(u_long)(align - 1));
    memset(a, 0, size);
    return a;
}

/**** rings ****/

static int ring_init(struct usb_ring *r, u_int n) {
    struct xhci_trb *link;
    r->trbs = (struct xhci_trb *)dma_zalloc(n * sizeof(struct xhci_trb), n * sizeof(struct xhci_trb), &r->raw);
    r->owner = (struct usb_xfer **)kcalloc(n, sizeof(struct usb_xfer *));
    if(!r->trbs || !r->owner) {
        if(r->raw) kfree_dma(r->raw);
        if(r->owner) kfree(r->owner);
        r->trbs = NULL; r->raw = NULL; r->owner = NULL;
        return USB_ENOMEM;
    }
    r->n = n;
    r->enq = r->deq = r->used = 0;
    r->pcs = 1;
    link = &r->trbs[n - 1];
    link->lo = phys(r->trbs);
    link->hi = 0;
    link->status = 0;
    link->control = TRB_TYPE(TRB_LINK) | TRB_TC;   /* cycle 0: the controller does not own it yet */
    return 0;
}

static void ring_free(struct usb_ring *r) {
    if(r->raw) kfree_dma(r->raw);
    if(r->owner) kfree(r->owner);
    r->trbs = NULL; r->raw = NULL; r->owner = NULL;
    r->n = 0;
}

/* Queue one TRB; returns its index. hold writes the cycle bit inverted so the controller
   does not take the TRB yet: the first TRB of a multi-TRB transfer is handed over last. */
static u_int ring_put(struct usb_ring *r, u32 lo, u32 hi, u32 status, u32 control, bool hold) {
    u_int i = r->enq;
    struct xhci_trb *t = &r->trbs[i];
    t->lo = lo;
    t->hi = hi;
    t->status = status;
    barrier();
    t->control = (control & ~TRB_C) | (hold ? (r->pcs ^ 1) : r->pcs);
    if(++r->enq == r->n - 1) {          /* past the last usable TRB: give the link over and wrap */
        struct xhci_trb *l = &r->trbs[r->n - 1];
        l->control = (l->control & ~TRB_C) | r->pcs;
        r->enq = 0;
        r->pcs ^= 1;
    }
    return i;
}

static inline u_int ring_space(struct usb_ring *r) {
    return r->n - 2 - r->used;           /* one usable TRB stays free */
}

static inline void doorbell(struct xhci *x, u_int slot, u_int target) {
    barrier();
    x->db[slot] = target;
}

/**** contexts ****/

static inline u32 *in_ctrl(struct usb_device *d) { return (u32 *)d->in_ctx; }
static inline u32 *in_slot(struct usb_device *d) { return (u32 *)((u8 *)d->in_ctx + XHC(d->hc)->csz); }
static inline u32 *in_ep(struct usb_device *d, u_int dci) { return (u32 *)((u8 *)d->in_ctx + (dci + 1) * XHC(d->hc)->csz); }
static inline u32 *out_slot(struct usb_device *d) { return (u32 *)d->out_ctx; }
static inline u32 *out_ep(struct usb_device *d, u_int dci) { return (u32 *)((u8 *)d->out_ctx + dci * XHC(d->hc)->csz); }

/**** events ****/

static int cc_status(u_int cc) {
    switch(cc) {
    case CC_SUCCESS: case CC_SHORT_PACKET: return USB_OK;
    case CC_STALL: return USB_ESTALL;
    case CC_STOPPED: case CC_STOPPED_LEN: case CC_STOPPED_SHORT: return USB_ECANCEL;
    default: return USB_EIO;
    }
}

/* the completion codes after which the endpoint is Halted (xHCI 4.10.2) */
static bool halts(u_int cc) {
    return cc == CC_STALL || cc == CC_BABBLE || cc == CC_TRANSACTION || cc == CC_SPLIT;
}

/* the transfer is over: its TRBs are free, its owner hears about it */
static void xfer_finish(struct usb_endpoint *ep, struct usb_xfer *x, u_int cc) {
    struct usb_ring *r = &ep->ring;
    u_int i = x->first, k;
    for(k = 0; k < x->ntrbs; k++) {
        r->owner[i] = NULL;
        if(++i == r->n - 1) i = 0;
    }
    r->deq = i;
    r->used -= x->ntrbs;
    x->status = cc_status(cc);
    ep->xfers++;
    if(x->status < 0 && x->status != USB_ECANCEL) ep->errors++;
    if(halts(cc) && ep->dci != 1) {           /* endpoint 0 is handled by xhci_control() itself */
        ep->halted = true;
        ep->halt_status = x->status;
        usb_wake();                           /* the usb thread resets it */
    }
    if(x->done) x->done(x);
}

static void transfer_event(struct xhci *x, struct xhci_trb *e) {
    u_int slot = e->control >> 24, dci = (e->control >> 16) & 0x1f;
    u_int cc = e->status >> 24, residual = e->status & 0xffffff, idx;
    struct usb_device *dev = x->slot_dev[slot];
    struct usb_endpoint *ep;
    struct usb_ring *r;
    struct usb_xfer *t;
    if(!dev || !(ep = dev->eps[dci]) || (e->control & TRB_ED)) return;
    r = &ep->ring;
    if(!r->trbs || e->hi || e->lo < phys(r->trbs) || e->lo >= phys(r->trbs + r->n - 1)) return;
    idx = (e->lo - phys(r->trbs)) / sizeof(struct xhci_trb);
    t = r->owner[idx];
    if(!t) return;
    if(idx != t->last) {
        /* a short packet in a control transfer's data stage (the status stage reports the end),
           or an error before the end, which is the end */
        if(cc == CC_SHORT_PACKET || cc == CC_SUCCESS) {
            if(idx == t->data_trb) t->actual = residual < t->length ? t->length - residual : 0;
            return;
        }
        xfer_finish(ep, t, cc);
        return;
    }
    /* a short packet says how much is missing; success means all of it (some controllers
       leave a stale count in a successful event, so it is not trusted there) */
    if(t->data_trb == ~0u) t->actual = cc == CC_SUCCESS ? t->length : (residual < t->length ? t->length - residual : 0);
    xfer_finish(ep, t, cc);
}

/* interrupts off */
void xhci_poll(struct usb_hc *h) {
    struct xhci *x = XHC(h);
    u_int n = 0;
    if(!x->evt || h->failed) return;
    if(x->op[OP_USBSTS] & (STS_HSE | STS_HCE)) {
        h->failed = true;
        h->failed_why = (x->op[OP_USBSTS] & STS_HCE) ? "host controller error" : "host system error";
        usb_wake();
        return;
    }
    for(;;) {
        struct xhci_trb *e = &x->evt[x->evt_deq];
        u32 c = e->control;
        if((c & TRB_C) != x->evt_ccs) break;
        barrier();
        switch(TRB_GET_TYPE(c)) {
        case TRB_TRANSFER_EVENT:
            transfer_event(x, e);
            break;
        case TRB_CMD_COMPLETION:
            if(x->cmd_busy && !x->cmd_done && e->lo == x->cmd_trb && !e->hi) {
                x->cmd_cc = e->status >> 24;
                x->cmd_slot = c >> 24;
                x->cmd_done = 1;
            }
            break;
        case TRB_PORT_STATUS:
            usb_wake();                     /* the usb thread looks at the ports */
            break;
        default:
            x->hc_events++;                 /* host controller events: an event ring that ran full */
            break;
        }
        x->events++;
        if(++x->evt_deq == EVT_TRBS) {
            x->evt_deq = 0;
            x->evt_ccs ^= 1;
        }
        /* during a long burst (a resubmitted receive can complete at once) the controller
           must see the ring drain, or it would think it full */
        if(++n % 64 == 0) {
            x->rt[RT_ERDP_LO] = phys(&x->evt[x->evt_deq]) | ERDP_EHB;
            x->rt[RT_ERDP_HI] = 0;
        }
    }
    if(n % 64) {
        x->rt[RT_ERDP_LO] = phys(&x->evt[x->evt_deq]) | ERDP_EHB;
        x->rt[RT_ERDP_HI] = 0;
    }
}

static void poll_timer(void *arg) {
    xhci_poll((struct usb_hc *)arg);
}

/* Poll until *v is no longer `pending`: at boot by busy-waiting, in a task by sleeping a
   tick between polls (the timer polls as well). false when ms passed first, the controller
   failed, or dev (when given) was unplugged. */
static bool wait_while(struct xhci *x, volatile int *v, int pending, struct usb_device *dev, u_long ms) {
    bool sleeping = usb_can_sleep();
    u_long start = getticks(), spins = 0;
    for(;;) {
        u_long f = irq_save();
        xhci_poll(&x->hc);
        irq_restore(f);
        if(*v != pending) return true;
        if(x->hc.failed || (dev && dev->gone)) return false;
        if(sleeping) {
            if((getticks() - start) * (1000 / HZ) >= ms) return false;
            sleep_ms(1);
        } else {
            if(spins++ >= ms * 20) return false;
            udelay(50);
        }
    }
}

static void controller_failed(struct xhci *x, const char *why) {
    if(!x->hc.failed) {
        x->hc.failed = true;
        x->hc.failed_why = why;
        x->op[OP_USBCMD] &= ~CMD_RS;
    }
}

/* one command at a time: queue it, ring the controller's doorbell, wait for its completion */
static int command(struct xhci *x, u32 lo, u32 hi, u32 control, u32 *slot) {
    u_long f;
    u_int i;
    bool ok;
    if(x->hc.failed) return USB_ENODEV;
    mutex_lock(&x->cmd_lock);
    f = irq_save();
    x->cmd_done = 0;
    x->cmd_busy = true;
    i = ring_put(&x->cmd, lo, hi, 0, control, false);
    x->cmd_trb = phys(&x->cmd.trbs[i]);
    doorbell(x, 0, 0);
    irq_restore(f);
    ok = wait_while(x, &x->cmd_done, 0, NULL, CMD_TIMEOUT);
    x->cmd_busy = false;
    mutex_unlock(&x->cmd_lock);
    if(!ok) {
        controller_failed(x, "a command was not completed");
        return USB_ENODEV;
    }
    if(slot) *slot = x->cmd_slot;
    return x->cmd_cc == CC_SUCCESS ? 0 : USB_EIO;
}

/**** ports ****/

u32 xhci_port_status(struct usb_hc *h, u_int port) {
    if(port < 1 || port > h->nports || h->failed) return 0;
    return portsc(XHC(h), port);
}

void xhci_port_ack(struct usb_hc *h, u_int port) {
    struct xhci *x = XHC(h);
    u32 v;
    if(port < 1 || port > h->nports || h->failed) return;
    v = portsc(x, port);
    if(v & PS_CHANGES) portsc_write(x, port, (v & PS_KEEP) | (v & PS_CHANGES));
}

/* wait until one of `bits` is set in PORTSC, up to ms */
static u32 port_wait(struct xhci *x, u_int port, u32 bits, u_long ms) {
    u_long t;
    u32 v = portsc(x, port);
    for(t = 0; !(v & bits) && t < ms; t += 10) {
        usb_msleep(10);
        v = portsc(x, port);
    }
    return v;
}

/* A connected port to the Enabled state: a USB 2 port is reset (a bus reset of the device);
   a USB 3 port trains its link by itself and is warm-reset only when that failed. The port's
   speed, or USB_ENODEV / USB_EIO. */
int xhci_port_enable(struct usb_hc *h, u_int port) {
    struct xhci *x = XHC(h);
    u32 v;
    if(port < 1 || port > h->nports || h->failed) return USB_EINVAL;
    v = portsc(x, port);
    if(!(v & PS_CCS)) return USB_ENODEV;
    if(h->port_proto[port] == 3 || (h->port_proto[port] == 0 && PS_SPEED(v) >= USB_SPEED_SUPER)) {
        v = port_wait(x, port, PS_PED, 1000);         /* link training */
        if(!(v & PS_PED) && (v & PS_CCS)) {
            portsc_write(x, port, (v & PS_KEEP) | PS_WPR);
            port_wait(x, port, PS_PRC | PS_WRC, 500);
            v = port_wait(x, port, PS_PED, 500);
        }
    } else {
        portsc_write(x, port, (v & PS_KEEP) | PS_PR);
        usb_msleep(10);                               /* TDRST is at least 10 ms; the controller times it */
        v = port_wait(x, port, PS_PRC, 500);
    }
    xhci_port_ack(h, port);
    v = portsc(x, port);
    if(!(v & PS_CCS)) return USB_ENODEV;
    if(!(v & PS_PED)) return USB_EIO;
    usb_msleep(10);                                   /* reset recovery (USB 2.0 7.1.7.5) */
    return (int)PS_SPEED(v);
}

/**** device slots ****/

int xhci_enable_slot(struct usb_device *dev) {
    struct xhci *x = XHC(dev->hc);
    u32 slot = 0;
    u_long f;
    int r = command(x, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot);
    if(r) return r;
    if(!slot || slot > x->max_slots) return USB_EIO;
    /* contexts must not cross a page boundary (xHCI table 6-1): page-aligned */
    dev->out_ctx = dma_zalloc(32 * x->csz, x->page_size, &dev->out_raw);
    dev->in_ctx = dma_zalloc(33 * x->csz, x->page_size, &dev->in_raw);
    dev->slot = (u8)slot;
    f = irq_save();
    x->slot_dev[slot] = dev;
    irq_restore(f);
    if(!dev->out_ctx || !dev->in_ctx) return USB_ENOMEM;
    x->dcbaa[slot] = phys(dev->out_ctx);
    return 0;
}

int xhci_endpoint_init(struct usb_endpoint *ep) {
    return ring_init(&ep->ring, ep->dci == 1 ? EP0_TRBS : EP_TRBS);
}

/* Address Device: the slot and endpoint 0 contexts, and the controller sends SET_ADDRESS */
int xhci_address_device(struct usb_device *dev) {
    struct xhci *x = XHC(dev->hc);
    u32 *slot, *ep0;
    int r;
    if(!dev->ep0.ring.trbs && (r = xhci_endpoint_init(&dev->ep0)) != 0) return r;
    memset(dev->in_ctx, 0, 33 * x->csz);
    slot = in_slot(dev);
    ep0 = in_ep(dev, 1);
    in_ctrl(dev)[1] = 3;                                          /* add the slot and endpoint 0 */
    slot[0] = ((u32)dev->speed << 20) | (1u << 27);               /* one context entry */
    slot[1] = (u32)dev->port << 16;
    ep0[1] = (3 << 1) | (EPT_CONTROL << 3) | ((u32)dev->ep0.mps << 16);
    ep0[2] = phys(dev->ep0.ring.trbs) | 1;
    ep0[3] = 0;
    ep0[4] = 8;                                                   /* average TRB length */
    r = command(x, phys(dev->in_ctx), 0, TRB_TYPE(TRB_ADDRESS_DEVICE) | TRB_SLOT(dev->slot), NULL);
    if(r == 0) {
        dev->address = out_slot(dev)[3] & 0xff;
        dev->ep0.configured = true;
    }
    return r;
}

/* the device descriptor said endpoint 0 takes other packets than the speed's default */
int xhci_set_ep0_mps(struct usb_device *dev, u_int mps) {
    struct xhci *x = XHC(dev->hc);
    u32 *ep0;
    int r;
    memset(dev->in_ctx, 0, 33 * x->csz);
    ep0 = in_ep(dev, 1);
    in_ctrl(dev)[1] = 2;
    ep0[1] = (3 << 1) | (EPT_CONTROL << 3) | ((u32)mps << 16);
    r = command(x, phys(dev->in_ctx), 0, TRB_TYPE(TRB_EVALUATE_CTX) | TRB_SLOT(dev->slot), NULL);
    if(r == 0) dev->ep0.mps = (u16)mps;
    return r;
}

/* the endpoint context's interval: a power of two in 125 us units */
static u32 ep_interval(u_int speed, u_int bInterval) {
    u_int units, e = 0;
    if(speed == USB_SPEED_HIGH || speed >= USB_SPEED_SUPER) {
        e = bInterval ? bInterval - 1 : 0;
        return e > 15 ? 15 : e;
    }
    units = (bInterval ? bInterval : 1) * 8;           /* full and low speed count 1 ms frames */
    while((2u << e) <= units) e++;
    if(e < 3) e = 3;
    if(e > 10) e = 10;
    return e;
}

static void fill_ep_context(struct usb_device *dev, struct usb_endpoint *ep, u32 *c) {
    bool in = (ep->addr & USB_DIR_IN) != 0;
    u32 type, interval = 0, esit = 0, avg = 1024;
    if(ep->type == USB_EP_INTR) {
        type = in ? EPT_INTR_IN : EPT_INTR_OUT;
        interval = ep_interval(dev->speed, ep->interval);
        esit = (u32)ep->mps * (ep->burst + 1);
        avg = esit;
    } else if(ep->type == USB_EP_ISOC) {
        type = in ? EPT_ISOC_IN : EPT_ISOC_OUT;      /* not used by any driver */
    } else {
        type = in ? EPT_BULK_IN : EPT_BULK_OUT;
    }
    c[0] = (interval << 16) | ((esit >> 16) << 24);
    c[1] = (3 << 1) | (type << 3) | ((u32)ep->burst << 8) | ((u32)ep->mps << 16);
    c[2] = phys(&ep->ring.trbs[ep->ring.enq]) | ep->ring.pcs;    /* a new ring: its start, cycle 1 */
    c[3] = 0;
    c[4] = (avg & 0xffff) | ((esit & 0xffff) << 16);
}

/* Configure Endpoint for every endpoint added since the last time */
int xhci_configure_endpoints(struct usb_device *dev) {
    struct xhci *x = XHC(dev->hc);
    u32 add = 0, *slot;
    u_int dci, last = 1;
    int r;
    memset(dev->in_ctx, 0, 33 * x->csz);
    for(dci = 2; dci < USB_MAX_EP; dci++) {
        struct usb_endpoint *ep = dev->eps[dci];
        if(!ep) continue;
        last = dci;
        if(ep->configured) continue;
        add |= 1u << dci;
        fill_ep_context(dev, ep, in_ep(dev, dci));
    }
    if(!add) return 0;
    in_ctrl(dev)[1] = add | 1;                         /* and the slot context, for Context Entries */
    slot = in_slot(dev);
    memcpy(slot, out_slot(dev), 16);
    slot[0] = (slot[0] & ~(0x1fu << 27)) | ((u32)last << 27);
    slot[3] = 0;
    r = command(x, phys(dev->in_ctx), 0, TRB_TYPE(TRB_CONFIGURE_EP) | TRB_SLOT(dev->slot), NULL);
    if(r == 0) {
        for(dci = 2; dci < USB_MAX_EP; dci++) if(add & (1u << dci)) dev->eps[dci]->configured = true;
    }
    return r;
}

/* Disable the slot (the controller lets go of the contexts and rings), then free them.
   If a running controller refuses, it may still use the device's memory: the rings and
   contexts are leaked rather than freed, dev->dma_live tells the driver to keep its
   buffers too, and the result is USB_EIO. */
int xhci_release_device(struct usb_device *dev) {
    struct xhci *x = XHC(dev->hc);
    u_int dci;
    u_long f;
    int r = 0;
    if(dev->slot) {
        r = command(x, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(dev->slot), NULL);
        if(x->hc.failed) r = 0;                      /* halted: it touches nothing any more */
        f = irq_save();
        x->slot_dev[dev->slot] = NULL;
        if(r == 0) x->dcbaa[dev->slot] = 0;
        irq_restore(f);
        dev->slot = 0;
    }
    if(r) {
        dev->dma_live = true;
        kprintf("+USB: port %u: the controller did not release the device; its memory is kept\n", dev->port);
    }
    for(dci = 1; dci < USB_MAX_EP; dci++) {
        struct usb_endpoint *ep = dev->eps[dci];
        if(!ep) continue;
        dev->eps[dci] = NULL;
        if(r) continue;
        ring_free(&ep->ring);
        if(ep != &dev->ep0) kfree(ep);
    }
    if(r == 0) {
        if(dev->out_raw) kfree_dma(dev->out_raw);
        if(dev->in_raw) kfree_dma(dev->in_raw);
    }
    dev->out_ctx = dev->in_ctx = dev->out_raw = dev->in_raw = NULL;
    return r;
}

/**** transfers ****/

static u32 ep_state(struct usb_endpoint *ep) {
    return out_ep(ep->dev, ep->dci)[0] & 7;
}

static int ep_command(struct usb_endpoint *ep, u_int type) {
    return command(XHC(ep->dev->hc), 0, 0, TRB_TYPE(type) | TRB_EP(ep->dci) | TRB_SLOT(ep->dev->slot), NULL);
}

/* Bring an endpoint to the Stopped state from wherever an error or a timeout left it:
   Halted needs Reset Endpoint, Running needs Stop Endpoint. The state in the output context
   can lag, so when one command is refused the other is tried. */
static int ep_stop(struct usb_endpoint *ep) {
    u32 s = ep_state(ep);
    if(s == EPS_STOPPED || s == EPS_ERROR) return 0;
    if(s == EPS_HALTED) return ep_command(ep, TRB_RESET_EP) == 0 || ep_command(ep, TRB_STOP_EP) == 0 ? 0 : USB_EIO;
    return ep_command(ep, TRB_STOP_EP) == 0 || ep_command(ep, TRB_RESET_EP) == 0 ? 0 : USB_EIO;
}

/* point the controller past everything that is finished: at the oldest transfer still
   queued, or where the next one will go. The endpoint must be Stopped. */
static int set_dequeue(struct usb_endpoint *ep) {
    struct usb_ring *r = &ep->ring;
    u_long f = irq_save();
    u_int i = r->used ? r->deq : r->enq;
    u32 cycle = r->used ? (r->trbs[i].control & TRB_C) : r->pcs;
    u32 ptr = phys(&r->trbs[i]) | cycle;
    irq_restore(f);
    return command(XHC(ep->dev->hc), ptr, 0, TRB_TYPE(TRB_SET_TR_DEQ) | TRB_EP(ep->dci) | TRB_SLOT(ep->dev->slot), NULL);
}

/* A control transfer: setup, optional data and status stages, then wait. The bytes moved,
   or USB_E*. After a stall (a request the device does not support), an error or a timeout,
   endpoint 0 is stopped and moved past the transfer, ready for the next request. */
static int control_locked(struct usb_device *dev, const u8 *setup, void *data, u_int len, bool in, u_int timeout_ms);

int xhci_control(struct usb_device *dev, const u8 *setup, void *data, u_int timeout_ms) {
    struct xhci *x = XHC(dev->hc);
    struct usb_endpoint *ep = &dev->ep0;
    u_int len = setup[6] | (setup[7] << 8);
    bool in = (setup[0] & USB_DIR_IN) != 0;
    int status;

    if(dev->gone || x->hc.failed || !dev->slot) return USB_ENODEV;
    if(ep->stuck) return USB_EIO;
    if(len > 4096) return USB_EINVAL;
    mutex_lock(&dev->ep0_lock);                        /* one request at a time per device */
    status = control_locked(dev, setup, data, len, in, timeout_ms);
    mutex_unlock(&dev->ep0_lock);
    return status;
}

static int control_locked(struct usb_device *dev, const u8 *setup, void *data, u_int len, bool in, u_int timeout_ms) {
    struct xhci *x = XHC(dev->hc);
    struct usb_endpoint *ep = &dev->ep0;
    struct usb_ring *r = &ep->ring;
    struct usb_xfer t;
    u_int first;
    u8 *buf = NULL;
    void *raw = NULL;
    u_long f;
    int status;
    u_int align;
    bool timed_out, lost = false;

    if(dev->gone || x->hc.failed || !dev->slot) return USB_ENODEV;
    if(ep->stuck) return USB_EIO;
    if(len) {
        /* aligned to its size, so the data stage cannot cross a 64 KB boundary */
        for(align = 64; align < len; align <<= 1);
        buf = (u8 *)dma_zalloc(len, align, &raw);
        if(!buf) return USB_ENOMEM;
        if(!in) memcpy(buf, data, len);
    }
    memset(&t, 0, sizeof(t));
    t.ep = ep;
    t.length = len;
    t.actual = len;                                    /* unless the data stage ends short */
    t.data_trb = ~0u;
    t.status = USB_PENDING;

    f = irq_save();
    if(ring_space(r) < 3) {
        irq_restore(f);
        if(raw) kfree_dma(raw);
        return USB_EIO;
    }
    first = ring_put(r, setup[0] | (setup[1] << 8) | (setup[2] << 16) | ((u32)setup[3] << 24),
                     setup[4] | (setup[5] << 8) | (setup[6] << 16) | ((u32)setup[7] << 24), 8,
                     TRB_TYPE(TRB_SETUP) | TRB_IDT | TRB_TRT(len ? (in ? 3 : 2) : 0), true);
    t.first = first;
    r->owner[first] = &t;
    t.ntrbs = 1;
    if(len) {
        t.data_trb = ring_put(r, phys(buf), 0, len, TRB_TYPE(TRB_DATA) | (in ? (TRB_DIR_IN | TRB_ISP) : 0), false);
        r->owner[t.data_trb] = &t;
        t.ntrbs++;
    }
    t.last = ring_put(r, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | ((len && in) ? 0 : TRB_DIR_IN), false);
    r->owner[t.last] = &t;
    t.ntrbs++;
    r->used += t.ntrbs;
    barrier();
    r->trbs[first].control ^= TRB_C;                   /* now the controller may have it */
    doorbell(x, dev->slot, 1);
    irq_restore(f);

    timed_out = !wait_while(x, &t.status, USB_PENDING, dev, timeout_ms) && t.status == USB_PENDING;
    if(t.status != USB_OK && !x->hc.failed && !dev->gone) {
        /* a stall, an error or a timeout: stop endpoint 0 and skip the rest of the transfer.
           When the controller refuses, the transfer may still run: its buffer is never freed
           and endpoint 0 takes no more requests. */
        int rc = ep_stop(ep);
        f = irq_save();
        if(t.status == USB_PENDING) {                  /* timed out: the TRBs are given up */
            u_int i = t.first, k;
            for(k = 0; k < t.ntrbs; k++) {
                r->owner[i] = NULL;
                if(++i == r->n - 1) i = 0;
            }
            r->used -= t.ntrbs;
            r->deq = r->enq;
            t.status = USB_ETIMEOUT;
        }
        irq_restore(f);
        if(rc == 0) rc = set_dequeue(ep);
        if(rc) {
            ep->stuck = true;
            lost = true;
        }
    }
    f = irq_save();
    if(t.status == USB_PENDING) {                      /* the device or the controller went away */
        u_int i = t.first, k;
        for(k = 0; k < t.ntrbs; k++) {
            r->owner[i] = NULL;
            if(++i == r->n - 1) i = 0;
        }
        r->used -= t.ntrbs;
        t.status = USB_ENODEV;
    }
    irq_restore(f);
    status = t.status;
    if(timed_out && status == USB_ECANCEL) status = USB_ETIMEOUT;    /* Stop Endpoint ended it */
    if(status == USB_OK) {
        u_int n = t.actual <= len ? t.actual : len;
        if(in && n) memcpy(data, buf, n);
        status = (int)n;
    }
    if(lost) {
        kprintf("+USB: port %u: endpoint 0 could not be stopped; the device takes no more requests\n", dev->port);
        return USB_EIO;
    }
    if(raw) kfree_dma(raw);
    return status;
}

/* a bulk or interrupt transfer: one TRB, interrupt on completion (and on a short packet) */
int xhci_submit(struct usb_xfer *t) {
    struct usb_endpoint *ep = t->ep;
    struct usb_device *dev = ep->dev;
    struct xhci *x = XHC(dev->hc);
    struct usb_ring *r = &ep->ring;
    bool in = (ep->addr & USB_DIR_IN) != 0;
    u_long f;
    u_int i;
    if(t->length > 0x10000 || ((phys(t->buf) ^ (phys(t->buf) + t->length - 1)) & ~0xffffu)) return USB_EINVAL;
    f = irq_save();
    if(dev->gone || x->hc.failed || !r->trbs || !ep->configured) { irq_restore(f); return USB_ENODEV; }
    if(ep->stuck) { irq_restore(f); return USB_EIO; }
    if(ring_space(r) < 1) { irq_restore(f); return USB_ENOMEM; }
    t->status = USB_PENDING;
    t->actual = 0;
    t->data_trb = ~0u;
    i = ring_put(r, phys(t->buf), 0, t->length, TRB_TYPE(TRB_NORMAL) | TRB_IOC | (in ? TRB_ISP : 0), false);
    r->owner[i] = t;
    t->first = t->last = i;
    t->ntrbs = 1;
    r->used++;
    if(!ep->halted) doorbell(x, dev->slot, ep->dci);
    irq_restore(f);
    return 0;
}

/* the usb thread: an endpoint halted by an error is stopped (Reset Endpoint), moved past the
   failed transfer (Set TR Dequeue Pointer), and the device's end is reset too with
   CLEAR_FEATURE(ENDPOINT_HALT): Reset Endpoint puts the controller's data toggle (or
   sequence number) back to 0, and only that request does the same on the device, stall or
   not. Then whatever is still queued runs again. */
static void recover_endpoint(struct usb_endpoint *ep) {
    struct usb_device *dev = ep->dev;
    struct xhci *x = XHC(dev->hc);
    int r = ep_stop(ep);
    u_long f;
    if(r == 0) r = set_dequeue(ep);
    if(r == 0) usb_clear_halt(dev, ep->addr);
    f = irq_save();
    ep->halted = false;
    if(r) ep->stuck = true;
    else if(ep->ring.used) doorbell(x, dev->slot, ep->dci);
    irq_restore(f);
    if(r) kprintf("+USB: port %u: endpoint %02x could not be reset after an error\n", dev->port, ep->addr);
}

/* Recover a halted endpoint now, from whichever task needs it (the usb thread for the
   asynchronous transfers, a driver waiting for its own transfer); one at a time. 0 when it
   runs again (or never halted), USB_PENDING while another task is at it, USB_EIO when the
   reset failed. */
int xhci_recover(struct usb_endpoint *ep) {
    u_long f = irq_save();
    if(!ep->halted || ep->stuck) {
        irq_restore(f);
        return ep->stuck ? USB_EIO : 0;
    }
    if(ep->recovering) {
        irq_restore(f);
        return USB_PENDING;
    }
    ep->recovering = true;
    irq_restore(f);
    recover_endpoint(ep);
    ep->recovering = false;
    return ep->stuck ? USB_EIO : 0;
}

void xhci_service(struct usb_hc *h) {
    struct xhci *x = XHC(h);
    u_int s, dci;
    if(h->failed) return;
    for(s = 1; s <= x->max_slots; s++) {
        struct usb_device *dev = x->slot_dev[s];
        if(!dev || dev->gone) continue;
        for(dci = 2; dci < USB_MAX_EP; dci++) {
            struct usb_endpoint *ep = dev->eps[dci];
            if(ep && ep->halted && !ep->stuck) xhci_recover(ep);
        }
    }
}

/* Take a transfer back off its ring (it timed out): stop the endpoint, forget the TRBs,
   move the controller past them. The endpoint is stuck if the controller refuses. */
static void abort_xfer(struct usb_endpoint *ep, struct usb_xfer *t) {
    struct usb_ring *r = &ep->ring;
    int rc = ep_stop(ep);
    u_long f = irq_save();
    if(t->status == USB_PENDING) {
        u_int i = t->first, k;
        for(k = 0; k < t->ntrbs; k++) {
            r->owner[i] = NULL;
            if(++i == r->n - 1) i = 0;
        }
        r->used -= t->ntrbs;
        if(r->deq == t->first) r->deq = r->used ? i : r->enq;
        t->status = USB_ETIMEOUT;
    }
    irq_restore(f);
    if(rc == 0) rc = set_dequeue(ep);
    if(rc) ep->stuck = true;
}

/* A bulk transfer the caller waits for (a disk command): the event ring is polled in a
   tight loop for the first few milliseconds, because a USB stick answers within microseconds
   to a few milliseconds and a tick is 10 ms, then a tick at a time. buf must be DMA heap
   memory within one 64 KB block. The bytes moved, or USB_E*. An error that halts the endpoint
   is recovered by xhci_recover() (the caller's choice, or the usb thread). */
#define SYNC_SPINS 500                         /* x 10 us */
int xhci_bulk_sync(struct usb_endpoint *ep, void *buf, u_int len, u_int timeout_ms) {
    struct usb_device *dev = ep->dev;
    struct xhci *x = XHC(dev->hc);
    struct usb_xfer t;
    bool sleeping = usb_can_sleep();
    u_long start, spins = 0;
    int r;
    memset(&t, 0, sizeof(t));
    t.ep = ep;
    t.buf = buf;
    t.length = len;
    r = xhci_submit(&t);
    if(r) return r;
    start = getticks();
    for(;;) {
        u_long f = irq_save();
        xhci_poll(&x->hc);
        irq_restore(f);
        if(t.status != USB_PENDING) break;
        if(x->hc.failed || dev->gone) {
            /* the controller is letting go of the device: no more commands for this endpoint,
               but no late event may find a pointer into this stack frame either */
            struct usb_ring *rg = &ep->ring;
            f = irq_save();
            if(t.status == USB_PENDING) {
                u_int i = t.first, k;
                for(k = 0; k < t.ntrbs; k++) {
                    rg->owner[i] = NULL;
                    if(++i == rg->n - 1) i = 0;
                }
                rg->used -= t.ntrbs;
                t.status = USB_ENODEV;
            }
            irq_restore(f);
            return USB_ENODEV;
        }
        if(spins < SYNC_SPINS || !sleeping) {
            if(spins++ >= SYNC_SPINS + (u_long)timeout_ms * 100) break;
            udelay(10);
            continue;
        }
        if((getticks() - start) * (1000 / HZ) >= timeout_ms) break;
        sleep_ms(1);
    }
    if(t.status == USB_PENDING) {
        abort_xfer(ep, &t);
        if(t.status != USB_OK) return USB_ETIMEOUT;   /* it may have finished while being stopped */
    }
    return t.status < 0 ? t.status : (int)t.actual;
}

/* Reset the controller's side of an endpoint without a halt: drop and add it in one
   Configure Endpoint, which restarts its data toggle (USB 2) or sequence number (USB 3) at 0,
   as CLEAR_FEATURE(ENDPOINT_HALT) does on the device. Nothing may be queued on it. */
int xhci_endpoint_reset(struct usb_endpoint *ep) {
    struct usb_device *dev = ep->dev;
    struct xhci *x = XHC(dev->hc);
    struct usb_ring *r = &ep->ring;
    u32 *slot;
    u_long f;
    u_int i;
    int rc;
    if(dev->gone || x->hc.failed) return USB_ENODEV;
    for(i = 0; ; i++) {                                /* claim it; someone else may be resetting it */
        bool mine;
        f = irq_save();
        mine = !ep->recovering;
        if(mine) ep->recovering = true;
        irq_restore(f);
        if(mine) break;
        if(i >= 500) return USB_EIO;
        usb_msleep(10);
    }
    ep_stop(ep);
    f = irq_save();
    for(i = 0; i < r->n; i++) {
        struct usb_xfer *t = r->owner[i];
        if(t && t->status == USB_PENDING) t->status = USB_ECANCEL;
        r->owner[i] = NULL;
    }
    r->used = 0;
    r->deq = r->enq;
    irq_restore(f);
    memset(dev->in_ctx, 0, 33 * x->csz);
    in_ctrl(dev)[0] = 1u << ep->dci;                   /* drop it ... */
    in_ctrl(dev)[1] = (1u << ep->dci) | 1;             /* ... and add it again */
    slot = in_slot(dev);
    memcpy(slot, out_slot(dev), 16);
    slot[3] = 0;
    fill_ep_context(dev, ep, in_ep(dev, ep->dci));
    rc = command(x, phys(dev->in_ctx), 0, TRB_TYPE(TRB_CONFIGURE_EP) | TRB_SLOT(dev->slot), NULL);
    f = irq_save();
    ep->halted = false;
    ep->stuck = rc != 0;
    ep->recovering = false;
    irq_restore(f);
    return rc;
}

void xhci_describe(struct usb_hc *h, char *buf, size_t size) {
    struct xhci *x = XHC(h);
    u_int p, n2 = 0, n3 = 0;
    for(p = 1; p <= h->nports; p++) {
        if(h->port_proto[p] == 3) n3++;
        else n2++;
    }
    snprintf(buf, size, "%s %04x:%04x, xHCI %x.%02x, %u USB 2 + %u USB 3 ports, %u slots, %s%s",
             h->name, x->pci->vendor, x->pci->device, x->version >> 8, x->version & 0xff, n2, n3, x->max_slots,
             x->csz == 64 ? "64-byte contexts" : "32-byte contexts", h->failed ? ", FAILED" : "");
}

/**** taking the controller over ****/

static void legacy_handoff(struct xhci *x, volatile u32 *c) {
    u_int i;
    if(c[0] & LEGACY_BIOS_OWNED) {
        c[0] = c[0] | LEGACY_OS_OWNED;
        for(i = 0; i < 1000 && (c[0] & LEGACY_BIOS_OWNED); i++) mdelay(1);
        if(c[0] & LEGACY_BIOS_OWNED) {
            kprintf("+xHCI: the firmware did not let go of the controller; taking it anyway\n");
            c[0] = c[0] & ~LEGACY_BIOS_OWNED;
        }
    }
    c[1] = (c[1] & LEGACY_SMI_KEEP) | LEGACY_SMI_EVENTS;    /* no SMIs on USB events */
}

static void extended_caps(struct xhci *x, u_long mapped) {
    u_long off = (x->hcc1 >> 16) << 2;
    u_int guard;
    for(guard = 0; off && off + 16 <= mapped && guard < 64; guard++) {
        volatile u32 *c = (volatile u32 *)(x->cap + off);
        u32 v = c[0];
        u_int next = (v >> 8) & 0xff;
        if((v & 0xff) == XCAP_LEGACY) legacy_handoff(x, c);
        else if((v & 0xff) == XCAP_PROTOCOL) {
            u_int major = v >> 24, first = c[2] & 0xff, count = (c[2] >> 8) & 0xff, p;
            for(p = first; p < first + count && p <= x->hc.nports; p++) if(p) x->hc.port_proto[p] = (u8)major;
        }
        if(!next) break;
        off += next << 2;
    }
}

static bool halt_and_reset(struct xhci *x) {
    u_int i;
    if(!(x->op[OP_USBSTS] & STS_HCH)) {
        x->op[OP_USBCMD] &= ~CMD_RS;
        for(i = 0; i < 200 && !(x->op[OP_USBSTS] & STS_HCH); i++) udelay(100);
        if(!(x->op[OP_USBSTS] & STS_HCH)) return false;
    }
    x->op[OP_USBCMD] = CMD_HCRST;
    mdelay(1);                     /* some Intel controllers must not be touched right after a reset */
    for(i = 0; i < 2000 && ((x->op[OP_USBCMD] & CMD_HCRST) || (x->op[OP_USBSTS] & STS_CNR)); i++) mdelay(1);
    return !(x->op[OP_USBCMD] & CMD_HCRST) && !(x->op[OP_USBSTS] & STS_CNR);
}

static struct usb_hc *controllers;

static int xhci_attach(struct pci_dev *pci) {
    struct xhci *x = (struct xhci *)kcalloc(1, sizeof(struct xhci));
    u_long size = pci->bar_size[0] ? pci->bar_size[0] : 0x10000;
    const char *why = NULL;
    u_int caplen, p, i;
    u32 pagesize;

    if(!x) return -1;
    snprintf(x->hc.name, sizeof(x->hc.name), "xHCI %02x:%02x.%x", pci->bus, pci->slot, pci->func);
    x->pci = pci;
    x->regs_phys = ((u64)pci->bar_hi[0] << 32) | pci->bar[0];
    if(pci->bar_io[0] || !x->regs_phys) {
        kprintf("+%s: no register BAR\n", x->hc.name);
        kfree(x);
        return -1;
    }
    pci_enable_device(pci);
    pci_dev_write16(pci, PCI_COMMAND, pci_dev_read16(pci, PCI_COMMAND) | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER |
                    PCI_COMMAND_INTX_DISABLE);       /* polled: its interrupt line stays quiet */
    x->cap = (volatile u8 *)ioremap64(x->regs_phys, size, &why);
    if(!x->cap) {
        kprintf("+%s: registers at 0x%x%08x cannot be mapped: %s\n", x->hc.name, pci->bar_hi[0], pci->bar[0], why);
        kfree(x);
        return -1;
    }
    caplen = x->cap[CAP_CAPLENGTH];
    x->version = (u16)(cap32(x, CAP_CAPLENGTH) >> 16);  /* a dword read: QEMU ignores a 16-bit one at offset 2 */
    x->hcs1 = cap32(x, CAP_HCSPARAMS1);
    x->hcs2 = cap32(x, CAP_HCSPARAMS2);
    x->hcc1 = cap32(x, CAP_HCCPARAMS1);
    if(caplen < 0x20 || caplen == 0xff || x->hcs1 == 0xffffffff) {
        kprintf("+%s: the registers do not answer\n", x->hc.name);
        kfree(x);
        return -1;
    }
    x->op = (volatile u32 *)(x->cap + caplen);
    x->rt = (volatile u32 *)(x->cap + (cap32(x, CAP_RTSOFF) & ~0x1fu));
    x->db = (volatile u32 *)(x->cap + (cap32(x, CAP_DBOFF) & ~0x3u));
    x->max_slots = x->hcs1 & 0xff;
    x->hc.nports = (x->hcs1 >> 24) & 0xff;
    x->csz = (x->hcc1 & HCC_CSZ) ? 64 : 32;
    x->nscratch = ((x->hcs2 >> 27) & 0x1f) | (((x->hcs2 >> 21) & 0x1f) << 5);
    mutex_init(&x->cmd_lock, "xhci command");

    extended_caps(x, size);
    if(!halt_and_reset(x)) {
        kprintf("+%s: the controller did not reset\n", x->hc.name);
        return -1;
    }
    pagesize = x->op[OP_PAGESIZE] & 0xffff;
    for(i = 0; i < 16 && !(pagesize & (1u << i)); i++);
    x->page_size = 4096u << (i < 16 ? i : 0);

    /* the device context base address array, the scratchpad the controller asked for,
       the command ring and the event ring with its one-segment table */
    x->dcbaa = (u64 *)dma_zalloc((x->max_slots + 1) * 8, x->page_size, &x->dcbaa_raw);     /* within one page */
    if(x->nscratch) {
        u8 *pages;
        x->scratch = (u64 *)dma_zalloc(x->nscratch * 8, x->page_size, &x->scratch_raw);
        pages = (u8 *)dma_zalloc(x->nscratch * x->page_size, x->page_size, &x->scratch_pages_raw);
        if(!x->scratch || !pages) goto nomem;
        for(i = 0; i < x->nscratch; i++) x->scratch[i] = phys(pages + i * x->page_size);
        if(x->dcbaa) x->dcbaa[0] = phys(x->scratch);
    }
    x->evt = (struct xhci_trb *)dma_zalloc(EVT_TRBS * sizeof(struct xhci_trb), EVT_TRBS * sizeof(struct xhci_trb), &x->evt_raw);
    x->erst = (struct erst_entry *)dma_zalloc(sizeof(struct erst_entry), 64, &x->erst_raw);
    if(!x->dcbaa || !x->evt || !x->erst || ring_init(&x->cmd, CMD_TRBS) != 0) goto nomem;
    x->evt_ccs = 1;
    x->erst[0].base_lo = phys(x->evt);
    x->erst[0].size = EVT_TRBS;

    x->op[OP_CONFIG] = (x->op[OP_CONFIG] & ~0xffu) | x->max_slots;
    x->op[OP_DCBAAP_LO] = phys(x->dcbaa);
    x->op[OP_DCBAAP_HI] = 0;
    x->op[OP_CRCR_LO] = phys(x->cmd.trbs) | 1;          /* ring cycle state 1 */
    x->op[OP_CRCR_HI] = 0;
    x->rt[RT_ERSTSZ] = 1;
    x->rt[RT_ERDP_LO] = phys(x->evt);
    x->rt[RT_ERDP_HI] = 0;
    x->rt[RT_ERSTBA_LO] = phys(x->erst);
    x->rt[RT_ERSTBA_HI] = 0;
    x->rt[RT_IMOD] = 0;
    x->rt[RT_IMAN] = 1;                                 /* clear a pending interrupt, interrupts off */
    x->op[OP_USBSTS] = STS_HSE | STS_EINT | STS_PCD;
    x->op[OP_USBCMD] = CMD_RS;                          /* run, without INTE */
    for(i = 0; i < 200 && (x->op[OP_USBSTS] & STS_HCH); i++) udelay(100);
    if(x->op[OP_USBSTS] & STS_HCH) {
        kprintf("+%s: the controller did not start\n", x->hc.name);
        return -1;
    }
    if(x->hcc1 & HCC_PPC) {                             /* ports with power switches: on */
        for(p = 1; p <= x->hc.nports; p++) {
            u32 v = portsc(x, p);
            if(!(v & PS_PP)) portsc_write(x, p, (v & PS_KEEP) | PS_PP);
        }
        mdelay(20);
    }
    ktimer_init(&x->poll, poll_timer, &x->hc);
    ktimer_start(&x->poll, 10, true);

    {
        char d[128];
        xhci_describe(&x->hc, d, sizeof(d));
        kprintf("+%s, registers at 0x%x%08x\n", d, pci->bar_hi[0], pci->bar[0]);
    }
    x->hc.next = NULL;
    {
        struct usb_hc **pp;
        for(pp = &controllers; *pp; pp = &(*pp)->next);
        *pp = &x->hc;
    }
    return 0;

nomem:
    kprintf("+%s: out of DMA memory\n", x->hc.name);
    return -1;
}

/* every xHCI controller on the PCI bus; returns the list */
struct usb_hc *xhci_probe(void) {
    struct pci_dev *d = NULL;
    u_int started = 0;
    while((d = pci_find_class(PCI_CLASS_SERIAL, PCI_SUBCLASS_USB, d)) != NULL) {
        if(d->prog_if != PCI_PROGIF_XHCI) {
            kprintf("+USB: %02x:%02x.%x is a USB 1/2 controller (interface %02x), not supported\n",
                    d->bus, d->slot, d->func, d->prog_if);
            continue;
        }
        if(xhci_attach(d) == 0) started++;
    }
    if(started) mdelay(100);              /* the ports see their devices again after the reset */
    return controllers;
}
