/*
SurfOS USB Boot Mouse
--------------------
File: mouse.c Date: 10/10/26
--------------------
HID boot mouse interrupt input, with one independent pointer source per device.
*/
#include <surfos/console.h>
#include <surfos/irq.h>
#include <sys/usbmouse.h>
#include <sys/pointer.h>
#include <mm/kalloc.h>
#include <blibc_common.h>

#define HID_SET_PROTOCOL 11
#define MOUSE_ERRORS 16
#define MOUSE_DMA_SIZE 1024

struct usb_mouse {
    struct usb_device *dev;
    struct usb_endpoint *in;
    struct usb_xfer rx;
    void *raw;
    unsigned source;
    volatile unsigned errors;
    volatile bool stopping, queued;
    bool reported;
};

static struct usb_mouse *mice[POINTER_USB_COUNT];

/* Called with interrupts off. The USB thread owns task-side driver work, so
   quiescing only has to exclude the timer's completion callback. */
static void mouse_stop(struct usb_mouse *m) {
    m->stopping = true;
    pointer_device(m->source, false);
}

static int mouse_queue(struct usb_mouse *m) {
    int r = usb_submit(&m->rx);
    m->queued = r == 0;
    if(r) {
        if(++m->errors >= MOUSE_ERRORS || r == USB_ENODEV) mouse_stop(m);
        usb_wake();
    }
    return r;
}

static void mouse_done(struct usb_xfer *x) {
    struct usb_mouse *m = (struct usb_mouse *)x->ctx;
    struct usb_mouse_report report;
    m->queued = false;
    if(m->stopping || m->dev->gone) return;
    if(x->status == USB_OK && x->actual <= x->length &&
       usb_mouse_decode_boot((const u8 *)x->buf, x->actual, &report)) {
        m->errors = 0;
        pointer_report(m->source, report.dx, report.dy, 0, report.buttons);
    } else if(x->status == USB_ENODEV) {
        mouse_stop(m);
        return;
    } else if(x->status != USB_ECANCEL && ++m->errors >= MOUSE_ERRORS) {
        mouse_stop(m);
        usb_wake();
        return;
    }
    /* xHCI can queue while halted; its USB-thread service resets the endpoint
       and rings its doorbell. No blocking control request runs in this callback. */
    mouse_queue(m);
}

static bool mouse_match(struct usb_device *d, const u8 *cfg, u_int len) {
    struct usb_mouse_function fn;
    (void)d;
    return usb_mouse_find(cfg, len, &fn) == 0;
}

static void mouse_free(struct usb_mouse *m) {
    u_long flags = irq_save();
    mice[m->source - POINTER_USB_FIRST] = NULL;
    irq_restore(flags);
    /* xHCI has removed this slot from callback lookup before detach. Only the
       DMA buffer can still be touched if disabling the hardware slot failed. */
    if(m->raw && !m->dev->dma_live) kfree_dma(m->raw);
    kfree(m);
}

static int mouse_attach(struct usb_device *d) {
    struct usb_mouse_function fn;
    struct usb_mouse *m;
    unsigned slot;
    u_long flags;
    int r = USB_ENOMEM;
    const char *stage = "allocating input state";
    if(usb_mouse_find(d->config, d->config_len, &fn) != 0) return USB_EINVAL;
    m = (struct usb_mouse *)kcalloc(1, sizeof(*m));
    if(!m) return USB_ENOMEM;
    flags = irq_save();
    for(slot = 0; slot < POINTER_USB_COUNT && mice[slot]; slot++) {}
    if(slot == POINTER_USB_COUNT) { irq_restore(flags); kfree(m); return USB_ENOMEM; }
    mice[slot] = m;
    m->source = POINTER_USB_FIRST + slot;
    m->dev = d;
    irq_restore(flags);

    stage = "selecting configuration";
    if((r = usb_set_configuration(d, d->config_value)) != 0) goto fail;
    if(fn.alt) {
        stage = "selecting interface";
        if((r = usb_set_interface(d, fn.iface, fn.alt)) != 0) goto fail;
    }
    stage = "selecting boot protocol";
    r = usb_control(d, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                    HID_SET_PROTOCOL, 0, fn.iface, NULL, 0);
    if(r != 0) goto fail;                  /* never decode report mode as boot mode */
    stage = "configuring interrupt endpoint";
    m->in = usb_endpoint_add(d, fn.in, fn.comp);
    if(!m->in) { r = USB_ENOMEM; goto fail; }
    if((r = usb_configure_endpoints(d)) != 0) goto fail;

    /* One maximum-size interrupt packet, aligned so it cannot cross 64 KB. */
    stage = "allocating receive buffer";
    m->raw = kalloc_dma(MOUSE_DMA_SIZE * 2);
    if(!m->raw) { r = USB_ENOMEM; goto fail; }
    m->rx.buf = (void *)(((u_long)m->raw + MOUSE_DMA_SIZE - 1) & ~(u_long)(MOUSE_DMA_SIZE - 1));
    m->rx.length = m->in->mps;
    m->rx.ep = m->in;
    m->rx.done = mouse_done;
    m->rx.ctx = m;
    memset(m->rx.buf, 0, m->rx.length);

    stage = "starting interrupt input";
    flags = irq_save();
    d->driver_data = m;
    pointer_device(m->source, true);
    r = mouse_queue(m);
    if(r) {
        mouse_stop(m);
        d->driver_data = NULL;
    }
    irq_restore(flags);
    if(r) goto fail;                      /* no submitted transfer owns this buffer */
    snprintf(d->status, sizeof(d->status), "boot mouse, pointer %u", m->source);
    return 0;
fail:
    kprintf("+USB: port %u: mouse: %s failed (%d)\n", d->port, stage, r);
    mouse_free(m);
    return r < 0 ? r : USB_EIO;
}

static void mouse_quiesce(struct usb_device *d) {
    struct usb_mouse *m = (struct usb_mouse *)d->driver_data;
    u_long flags;
    if(!m) return;
    flags = irq_save();
    mouse_stop(m);
    irq_restore(flags);
}

static void mouse_detach(struct usb_device *d) {
    struct usb_mouse *m = (struct usb_mouse *)d->driver_data;
    if(!m) return;
    mouse_quiesce(d);
    d->driver_data = NULL;
    mouse_free(m);                        /* endpoint memory was already released */
}

static void mouse_service(struct usb_device *d) {
    struct usb_mouse *m = (struct usb_mouse *)d->driver_data;
    bool report;
    u_long flags;
    if(!m) return;
    flags = irq_save();
    if(d->gone || m->in->stuck) mouse_stop(m);
    if(!m->stopping && !m->queued) mouse_queue(m);
    report = m->stopping && !m->reported && !d->gone;
    if(report) m->reported = true;
    irq_restore(flags);
    if(report) {
        strlcpy(d->status, "boot mouse: input stopped (replug)", sizeof(d->status));
        kprintf("+USB: port %u: mouse input stopped; unplug and replug it\n", d->port);
    }
}
const struct usb_driver mouse_driver = { "usbmouse", mouse_match, mouse_attach, mouse_quiesce, mouse_detach, mouse_service };
