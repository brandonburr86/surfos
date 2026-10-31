/* Boot mouse descriptor/report tests. Hand-built packets catch endpoint/interface
   mixups, out-of-bounds descriptor reads, and treating report-mode bytes as a wheel.
   10/26/26 (stage C8): the fourth byte is the wheel's only for a mouse whose report descriptor
   puts its Wheel there (QEMU's usb-mouse's does); a mouse without one has no false wheel. */
#include <stdio.h>
#include <string.h>
#include <sys/usbmouse.h>
#include <sys/pointer.h>

/* Exercise the actual driver with only hardware, allocation and pointer sinks
   replaced. The production build still uses the real interrupt helpers. */
#define _SURFOS_IRQ_H
static u_long irq_save(void) { return 0; }
static void irq_restore(u_long flags) { (void)flags; }
#include "../../driver/usb/mouse.c"

void exit(int status);
static int checks, failures;
static int kalloc_fails;                                /* 10/26/26: kalloc returns NULL while set */
static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

static u8 cfg[512];
static u_int used;
static void add(const u8 *p, u_int n) { memcpy(cfg + used, p, n); used += n; }
#define ADD(...) do { const u8 bytes[] = { __VA_ARGS__ }; add(bytes, sizeof(bytes)); } while(0)
static void begin(void) {
    used = 0;
    memset(cfg, 0, sizeof(cfg));
    ADD(9, USB_DT_CONFIG, 0, 0, 1, 1, 0, 0x80, 50);
}
static void iface(u8 number, u8 alt, u8 subclass, u8 protocol) {
    ADD(9, USB_DT_INTERFACE, number, alt, 1, USB_CLASS_HID, subclass, protocol, 0);
}
static void endpoint(u8 address, u8 type, u8 mps) {
    ADD(7, USB_DT_ENDPOINT, address, type, mps, 0, 10);
}
static u_int end(void) { cfg[2] = (u8)used; cfg[3] = (u8)(used >> 8); return used; }
static u_int mouse(void) {
    begin();
    iface(3, 0, 1, 2);
    ADD(9, 0x21, 0x11, 1, 0, 1, 0x22, 52, 0);          /* 10/26/26: its report descriptor, wheel_desc's 52 bytes */
    endpoint(0x82, USB_EP_INTR, 8);
    return end();
}

static void test_reports(void) {
    const u8 positive[] = { 1, 42, 17 };
    const u8 negative[] = { 0xff, 0x80, 0xff, 0x7f };
    const u8 away[] = { 0, 0, 0, 1 }, toward[] = { 1, 3, 0, 0xff };
    const u8 extremes[] = { 2, 0x7f, 0x80 };
    struct usb_mouse_report r = { 99, 98, 97 };
    check(usb_mouse_decode_boot(positive, 3, true, &r) && r.dx == 42 && r.dy == 17 && r.buttons == 1 && r.wheel == 0,
          "three-byte report preserves positive movement and left button, and has no wheel");
    check(usb_mouse_decode_boot(negative, 4, false, &r) && r.dx == -128 && r.dy == -1 && r.buttons == 7 && r.wheel == 0,
          "signed axes, three buttons only; additional bytes do not change a boot report without a wheel");
    check(usb_mouse_decode_boot(negative, 4, true, &r) && r.dx == -128 && r.wheel == 127,
          "10/26/26: a mouse whose report descriptor puts its wheel there: the fourth byte is the wheel's");
    check(usb_mouse_decode_boot(away, 4, true, &r) && r.wheel == 1 && !r.dx && !r.buttons, "one notch away from the user");
    check(usb_mouse_decode_boot(toward, 8, true, &r) && r.wheel == -1 && r.dx == 3 && r.buttons == 1,
          "one notch toward the user; bytes past the fourth change nothing");
    check(usb_mouse_decode_boot(extremes, 3, true, &r) && r.dx == 127 && r.dy == -128 && r.buttons == 2,
          "axis extrema and right button");
    check(!usb_mouse_decode_boot(positive, 2, true, &r) && r.dx == 127 && r.dy == -128 && r.buttons == 2,
          "truncated packet does not release buttons or overwrite state");
    check(!usb_mouse_decode_boot(NULL, 3, true, &r), "NULL packet rejected");
    check(!usb_mouse_decode_boot(positive, 3, true, NULL), "NULL report rejected");
}

/* 10/26/26 (C8): QEMU's usb-mouse's report descriptor (hw/usb/dev-hid.c): five buttons and padding, then X, Y and the
   Wheel as signed bytes at bits 8, 16 and 24 */
static const u8 wheel_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xc0, 0xc0,
};
/* the same without the Wheel */
static const u8 plain_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0xc0, 0xc0,
};

static void test_wheel_layout(void) {
    u8 d[sizeof(wheel_desc) + 2];
    check(usb_mouse_wheel(wheel_desc, sizeof(wheel_desc)), "a Wheel byte at bit 24: the fourth byte is the wheel's");
    check(!usb_mouse_wheel(plain_desc, sizeof(plain_desc)), "no Wheel: no false wheel");
    memcpy(d, wheel_desc, sizeof(wheel_desc));
    d[45] = 0x10;                                       /* sixteen bits each: the Wheel at bit 40 */
    check(!usb_mouse_wheel(d, sizeof(wheel_desc)), "a Wheel elsewhere is not the fourth byte");
    memcpy(d, wheel_desc, 6);                           /* a Report ID before the first collection's fields */
    d[6] = 0x85; d[7] = 0x01;
    memcpy(d + 8, wheel_desc + 6, sizeof(wheel_desc) - 6);
    check(!usb_mouse_wheel(d, sizeof(wheel_desc) + 2), "numbered reports: no wheel byte at a fixed place");
    check(!usb_mouse_wheel(wheel_desc, sizeof(wheel_desc) - 1), "a truncated descriptor: no wheel");
    check(!usb_mouse_wheel(NULL, 10), "no descriptor: no wheel");
    kalloc_fails = 1;                                   /* the parser's layout is allocated: none, no wheel */
    check(!usb_mouse_wheel(wheel_desc, sizeof(wheel_desc)), "no memory for the layout: no wheel");
    kalloc_fails = 0;
}

static void test_descriptors(void) {
    struct usb_mouse_function f;
    u_int n = mouse(), i;
    memset(&f, 0, sizeof(f));
    check(usb_mouse_find(cfg, n, &f) == 0 && f.iface == 3 && f.alt == 0 && f.in == (void *)(cfg + 27) && !f.comp,
          "boot mouse uses its interrupt IN endpoint and interface number");
    check(f.report_len == 52, "10/26/26: its HID descriptor's report descriptor length");
    begin(); iface(3, 0, 1, 2); endpoint(0x82, USB_EP_INTR, 8); n = end();
    check(usb_mouse_find(cfg, n, &f) == 0 && f.report_len == 0, "without a HID descriptor: no report length");
    n = mouse();
    for(i = 0; i < n; i++) check(usb_mouse_find(cfg, i, &f) < 0, "every truncated configuration rejected");
    check(usb_mouse_find(NULL, n, &f) < 0, "NULL configuration rejected");
    check(usb_mouse_find(cfg, n, NULL) < 0, "NULL function rejected");
    cfg[2] = 8; check(usb_mouse_find(cfg, n, &f) < 0, "total length smaller than header rejected");
    n = mouse(); cfg[0] = 8; check(usb_mouse_find(cfg, n, &f) < 0, "short configuration header rejected");
    n = mouse(); cfg[1] = USB_DT_DEVICE; check(usb_mouse_find(cfg, n, &f) < 0, "non-configuration rejected");
    n = mouse(); cfg[15] = 0; check(usb_mouse_find(cfg, n, &f) < 0, "report-only HID not claimed");
    n = mouse(); cfg[16] = 1; check(usb_mouse_find(cfg, n, &f) < 0, "boot keyboard not claimed");
    n = mouse(); cfg[14] = USB_CLASS_VENDOR; check(usb_mouse_find(cfg, n, &f) < 0, "vendor protocol not claimed");
    n = mouse(); cfg[29] = 0x02; check(usb_mouse_find(cfg, n, &f) < 0, "OUT endpoint not used for reports");
    n = mouse(); cfg[29] = 0x80; check(usb_mouse_find(cfg, n, &f) < 0, "endpoint zero rejected");
    n = mouse(); cfg[29] = 0x92; check(usb_mouse_find(cfg, n, &f) < 0, "reserved endpoint address bits rejected");
    n = mouse(); cfg[30] = USB_EP_BULK; check(usb_mouse_find(cfg, n, &f) < 0, "bulk endpoint rejected");
    n = mouse(); cfg[31] = 2; check(usb_mouse_find(cfg, n, &f) < 0, "packet size cannot hold boot report");
    n = mouse(); cfg[32] = 0x18; check(usb_mouse_find(cfg, n, &f) < 0, "reserved high-bandwidth multiplier rejected");
    n = mouse(); cfg[33] = 0; check(usb_mouse_find(cfg, n, &f) < 0, "zero interrupt interval rejected");
    n = mouse(); cfg[9] = 0; check(usb_mouse_find(cfg, n, &f) < 0, "zero-length descriptor terminates safely");
    n = mouse(); cfg[9] = 1; check(usb_mouse_find(cfg, n, &f) < 0, "one-byte descriptor rejected");
    n = mouse(); cfg[9] = 8; check(usb_mouse_find(cfg, n, &f) < 0, "short interface rejected");
    n = mouse(); cfg[27] = 6; check(usb_mouse_find(cfg, n, &f) < 0, "short endpoint rejected");
    n = mouse(); cfg[13] = 0; check(usb_mouse_find(cfg, n, &f) < 0, "undeclared endpoint rejected");
    n = mouse(); cfg[13] = 2; check(usb_mouse_find(cfg, n, &f) < 0, "missing declared endpoint rejected");

    begin(); iface(0, 0, 1, 2); cfg[13] = 0;
    iface(1, 0, 0, 2); endpoint(0x81, USB_EP_INTR, 8); n = end();
    check(usb_mouse_find(cfg, n, &f) < 0, "next interface's endpoint does not belong to boot mouse");
    begin(); iface(0, 0, 1, 1); endpoint(0x81, USB_EP_INTR, 8);
    iface(4, 1, 1, 2); endpoint(0x83, USB_EP_INTR, 8); n = end();
    check(usb_mouse_find(cfg, n, &f) == 0 && f.iface == 4 && f.alt == 1 && f.in->bEndpointAddress == 0x83,
          "composite device picks mouse interface and its alternate setting");

    n = mouse(); ADD(6, USB_DT_SS_EP_COMP, 0, 0, 8, 0); n = end();
    check(usb_mouse_find(cfg, n, &f) == 0 && f.comp == (void *)(cfg + 34), "adjacent SuperSpeed companion matched");
    cfg[34] = 5; check(usb_mouse_find(cfg, n, &f) < 0, "short companion rejected");
    n = mouse(); ADD(2, 0xff); ADD(6, USB_DT_SS_EP_COMP, 0, 0, 8, 0); n = end();
    check(usb_mouse_find(cfg, n, &f) == 0 && !f.comp, "non-adjacent companion not borrowed");
    n = mouse(); ADD(0, 0xff); n = end();
    check(usb_mouse_find(cfg, n, &f) < 0, "malformed tail rejected even after valid mouse endpoint");
    n = mouse(); ADD(9, USB_DT_INTERFACE, 1, 0, 1, USB_CLASS_HID, 1, 2, 0); endpoint(0x83, USB_EP_INTR, 8);
    check(usb_mouse_find(cfg, used, &f) == 0 && f.iface == 3,
          "bytes beyond declared configuration length are ignored");
}

void *malloc(size_t size);
void free(void *ptr);
static int control_error, submit_error, configure_error;
static unsigned devices, reports, submissions, dma_allocs, dma_frees;
static unsigned report_source, report_buttons;
static int report_dx, report_dy, report_wheel;
static u8 protocol_iface, interface_alt;
static bool boot_selected;
static struct usb_xfer *last_xfer;
static void *retained_dma;

void *kcalloc(size_t n, size_t size) {
    void *p = malloc(n * size);
    if(p) memset(p, 0, n * size);
    return p;
}
void kfree(void *p) { free(p); }
void *kalloc(size_t n) { return kalloc_fails ? NULL : malloc(n); }  /* 10/26/26: the report descriptor's buffer,
                                                                       usb_mouse_wheel()'s layout */
void *kalloc_dma(size_t n) { dma_allocs++; retained_dma = malloc(n); return retained_dma; }
void kfree_dma(void *p) { dma_frees++; free(p); }
void kprintf(const char *format, ...) { (void)format; }
void usb_wake(void) {}
void pointer_device(unsigned source, bool present) {
    if(present) devices |= 1u << source;
    else devices &= ~(1u << source);
}
void pointer_report(unsigned source, int dx, int dy, int wheel, unsigned buttons) {
    reports++; report_source = source; report_dx = dx; report_dy = dy;
    report_wheel = wheel; report_buttons = buttons;
}
int usb_set_configuration(struct usb_device *d, u8 value) { (void)d; (void)value; return 0; }
int usb_set_interface(struct usb_device *d, u8 iface_num, u8 alt) {
    (void)d; protocol_iface = iface_num; interface_alt = alt; return 0;
}
/* 10/26/26: the report descriptor the device gives (GET_DESCRIPTOR, type 0x22, on the interface) */
static const u8 *report_desc = wheel_desc;
static u_int report_desc_len = sizeof(wheel_desc);
int usb_control(struct usb_device *d, u8 type, u8 req, u16 value, u16 index, void *data, u16 len) {
    (void)d;
    if(type == 0x21 && req == 11 && value == 0 && len == 0) { boot_selected = true; protocol_iface = index; }
    if(type == 0x81 && req == 6 && value == 0x2200 && !control_error) {
        u_int n = report_desc_len < len ? report_desc_len : len;
        memcpy(data, report_desc, n);
        return (int)n;
    }
    return control_error;
}
struct usb_endpoint *usb_endpoint_add(struct usb_device *d, const struct usb_endpoint_descriptor *e,
                                     const struct usb_ss_ep_comp_descriptor *comp) {
    struct usb_endpoint *ep = kcalloc(1, sizeof(*ep));
    (void)comp;
    if(!ep) return NULL;
    ep->dev = d; ep->addr = e->bEndpointAddress; ep->mps = e->wMaxPacketSize & 0x7ff;
    ep->dci = (ep->addr & 15) * 2 + 1;
    d->eps[ep->dci] = ep;
    return ep;
}
int usb_configure_endpoints(struct usb_device *d) { (void)d; return configure_error; }
int usb_submit(struct usb_xfer *x) {
    if(submit_error) return submit_error;
    last_xfer = x; submissions++; x->status = USB_PENDING; return 0;
}

static void setup_device(struct usb_device *d) {
    memset(d, 0, sizeof(*d)); d->config_len = mouse(); d->config = cfg; d->config_value = 1;
}
static void remove_device(struct usb_device *d) {
    unsigned i;
    d->gone = true;
    mouse_driver.quiesce(d);
    /* xHCI releases endpoint memory before invoking the driver's detach. */
    for(i = 0; i < USB_MAX_EP; i++) { free(d->eps[i]); d->eps[i] = NULL; }
    mouse_driver.detach(d);
}
static void test_driver(void) {
    struct usb_device d[POINTER_USB_COUNT + 1];
    struct usb_xfer *x;
    unsigned i, before, source;
    setup_device(&d[0]);
    check(mouse_driver.attach(&d[0]) == 0 && boot_selected && protocol_iface == 3,
          "attach selects boot protocol on the matched interface");
    x = last_xfer;
    check(x && x->length >= 3 && devices == (1u << POINTER_USB_FIRST), "attach queues one DMA report and marks source present");
    if(!x) return;
    before = submissions; mouse_driver.service(&d[0]);
    check(submissions == before, "service does not duplicate an outstanding receive");
    ((u8 *)x->buf)[0] = 5; ((u8 *)x->buf)[1] = 0xfe; ((u8 *)x->buf)[2] = 12; ((u8 *)x->buf)[3] = 0xff;
    x->status = USB_OK; x->actual = 4; before = submissions; x->done(x);
    source = report_source;
    check(reports == 1 && source == POINTER_USB_FIRST && report_dx == -2 && report_dy == 12 &&
          report_buttons == 5 && report_wheel == -1 && submissions == before + 1,
          "completion publishes motion and the wheel (10/26/26: the fourth byte) and immediately requeues");
    check(strstr(d[0].status, "with a wheel") != NULL, "10/26/26: lsusb says it has a wheel");
    x->status = USB_OK; x->actual = 2; before = reports; x->done(x);
    check(reports == before, "short interrupt report is not published");
    x->status = USB_ESTALL; before = submissions; x->done(x);
    check(submissions == before + 1, "halted endpoint keeps a receive queued for xHCI recovery");
    x->status = USB_OK; x->actual = 3; submit_error = USB_ENOMEM; x->done(x); submit_error = 0;
    before = submissions; mouse_driver.service(&d[0]);
    check(submissions == before + 1, "failed resubmission is retried by USB service");
    d[0].gone = true; mouse_driver.quiesce(&d[0]); before = submissions;
    x->status = USB_OK; x->actual = 3; x->done(x);
    check(devices == 0 && submissions == before, "quiesce releases source and prevents late callback requeue");
    before = dma_frees; remove_device(&d[0]);
    check(!d[0].driver_data && dma_frees == before + 1, "detach frees owned DMA after controller release");

    /* 10/26/26: a mouse whose report descriptor has no Wheel: its fourth byte is no wheel */
    setup_device(&d[0]); report_desc = plain_desc; report_desc_len = sizeof(plain_desc);
    check(mouse_driver.attach(&d[0]) == 0 && !strstr(d[0].status, "wheel"), "a mouse without a Wheel attaches as before");
    x = last_xfer;
    ((u8 *)x->buf)[0] = 1; ((u8 *)x->buf)[1] = 3; ((u8 *)x->buf)[2] = 0; ((u8 *)x->buf)[3] = 0xff;
    x->status = USB_OK; x->actual = 4; before = reports; x->done(x);
    check(reports == before + 1 && report_dx == 3 && report_wheel == 0, "without false wheel");
    report_desc = wheel_desc; report_desc_len = sizeof(wheel_desc);
    remove_device(&d[0]);

    setup_device(&d[0]); control_error = USB_ESTALL;
    check(mouse_driver.attach(&d[0]) < 0 && !d[0].driver_data && devices == 0,
          "SET_PROTOCOL rejection never claims a usable mouse");
    control_error = 0; remove_device(&d[0]);
    setup_device(&d[0]); configure_error = USB_EIO; before = dma_allocs;
    check(mouse_driver.attach(&d[0]) < 0 && !d[0].driver_data && devices == 0 && dma_allocs == before,
          "endpoint setup failure leaves no claimed input source or DMA receive buffer");
    configure_error = 0; remove_device(&d[0]);
    setup_device(&d[0]); submit_error = USB_EIO;
    check(mouse_driver.attach(&d[0]) < 0 && !d[0].driver_data && devices == 0,
          "initial queue failure releases source and driver state");
    submit_error = 0; remove_device(&d[0]);

    setup_device(&d[0]); cfg[12] = 1; interface_alt = 0;
    check(mouse_driver.attach(&d[0]) == 0 && interface_alt == 1 && protocol_iface == 3,
          "alternate mouse interface selected before boot input starts");
    x = last_xfer;
    for(i = 0; i < 16; i++) { x->status = USB_OK; x->actual = 1; x->done(x); }
    before = submissions; mouse_driver.service(&d[0]);
    check(devices == 0 && submissions == before,
          "repeated malformed reports stop input and release buttons after bounded retries");
    remove_device(&d[0]);

    setup_device(&d[0]); check(mouse_driver.attach(&d[0]) == 0, "attach before endpoint recovery failure");
    last_xfer->ep->stuck = true; before = submissions; mouse_driver.service(&d[0]);
    check(devices == 0 && submissions == before,
          "unrecoverable endpoint releases source without requeueing");
    remove_device(&d[0]);

    for(i = 0; i < POINTER_USB_COUNT; i++) {
        setup_device(&d[i]); check(mouse_driver.attach(&d[i]) == 0, "independent USB source slot allocated");
    }
    setup_device(&d[i]); check(mouse_driver.attach(&d[i]) < 0, "full source table fails without stealing a source");
    remove_device(&d[i]);
    remove_device(&d[2]);
    check(devices != 0 && !(devices & (1u << (POINTER_USB_FIRST + 2))), "unplug releases only that mouse's source");
    setup_device(&d[2]); check(mouse_driver.attach(&d[2]) == 0, "unplugged source slot can be reused");
    for(i = 0; i < POINTER_USB_COUNT; i++) remove_device(&d[i]);
    check(devices == 0, "all detached sources released");

    setup_device(&d[0]); check(mouse_driver.attach(&d[0]) == 0, "attach before retained-DMA case");
    d[0].dma_live = true; before = dma_frees; remove_device(&d[0]);
    check(dma_frees == before && !d[0].driver_data && devices == 0,
          "DMA is retained when controller may still write, while source is released");
    free(retained_dma);
}

int main(void) {
    test_reports(); test_wheel_layout(); test_descriptors(); test_driver();
    printf("USB MOUSE UNIT TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    exit(failures ? 1 : 0);
    return 0;
}
