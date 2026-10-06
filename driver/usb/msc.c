/*
SurfOS USB Mass Storage Driver
--------------------
File: msc.c     Date: 10/5/26 (USB storage)
--------------------
USB sticks, external drives and card readers: the Bulk-Only Transport (USB Mass
Storage Class, BOT 1.0) carrying SCSI block commands: INQUIRY, TEST UNIT READY,
READ CAPACITY (10, and 16 for big disks), READ (10), WRITE (10), REQUEST SENSE,
MODE SENSE (the write-protect switch), SYNCHRONIZE CACHE. Every logical unit
with a medium becomes a disk, usb0, usb1, ..., whose partitions are scanned like
any other disk's.

A command is synchronous: the task that reads or writes sends the command block,
moves the data and reads the status, waiting with xhci_bulk_sync(), which spins
for the first milliseconds because a stick answers fast. A stalled transfer gets
its endpoint reset; a garbled exchange gets the BOT reset recovery. The usb
thread looks at removable units every two seconds, so a card put into a card
reader appears and one taken out goes away. Unplugging unregisters the disks:
their partitions stay mounted but fail with I/O errors until unmounted. The
driver's structure is never freed, so a task that was inside it when the stick
came out finds it marked gone instead of freed memory.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/sync.h>
#include <surfos/timer.h>
#include <sys/usb.h>
#include <sys/bdev.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <blibc_common.h>

#define MSC_MAX_LUNS   4
#define MSC_CHUNK      32768            /* bytes per READ(10)/WRITE(10): one TRB in one 32 KB-aligned buffer */
#define CMD_TIMEOUT    20000            /* ms for the data and status of a command: flash can be slow to write */
#define POLL_TICKS     (2 * HZ)         /* how often removable units are asked whether a medium is there */

#define CBW_SIGNATURE  0x43425355       /* "USBC" */
#define CSW_SIGNATURE  0x53425355       /* "USBS" */
#define BOT_RESET      0xff             /* class requests to the interface */
#define BOT_GET_MAX_LUN 0xfe
#define BOT_CHECK      1                /* bot_command(): the device says the command failed (sense it) */

#define SK_NOT_READY   0x02
#define SK_UNIT_ATTENTION 0x06
#define ASC_NO_MEDIUM  0x3a

struct msc;

struct msc_lun {
    struct msc *m;
    u8 lun;
    bool inquired, usable, removable;
    char vendor[9], product[17];
    struct bdev *disk;                  /* registered while a medium is there (never freed) */
    u_long next_poll;                   /* tick of the next TEST UNIT READY for a removable unit */
};

struct msc {
    struct usb_device *udev;
    struct usb_endpoint *in, *out;
    u8 iface, nluns;
    u32 tag;
    mutex_t lock;                       /* one command at a time; held across the whole exchange */
    volatile bool gone;                 /* unplugged and quiesced: every call fails */
    u8 *cbw, *csw, *data;               /* DMA heap */
    void *small_raw, *data_raw;
    u8 sense_key, asc, ascq;
    u_long resets;
    struct msc_lun luns[MSC_MAX_LUNS];
};

static const struct bdev_ops msc_ops;

static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u32 le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static void put32le(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void put32be(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

/* zeroed DMA memory aligned to align (a power of two) */
static u8 *dma_block(u_int size, u_int align, void **raw) {
    u8 *p = (u8 *)kalloc_dma(size + align), *a;
    *raw = p;
    if(!p) return NULL;
    a = (u8 *)(((u_long)p + align - 1) & ~(u_long)(align - 1));
    memset(a, 0, size);
    return a;
}

/* SCSI strings are space-padded ASCII */
static void scsi_string(char *out, const u8 *in, u_int n) {
    u_int i;
    for(i = 0; i < n; i++) out[i] = (in[i] >= 0x20 && in[i] < 0x7f) ? (char)in[i] : ' ';
    out[n] = 0;
    while(n > 0 && out[n - 1] == ' ') out[--n] = 0;
}

/**** the Bulk-Only Transport ****/

/* BOT 5.3.4: a class reset of the interface, then both bulk endpoints back to toggle /
   sequence 0 at both ends. After this the device expects a new command block. */
static int reset_recovery(struct msc *m, int err) {
    if(m->udev->gone) return USB_ENODEV;
    m->resets++;
    usb_control(m->udev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, BOT_RESET, 0, m->iface, NULL, 0);
    usb_endpoint_reset(m->in);
    usb_endpoint_reset(m->out);
    return err < 0 ? err : USB_EIO;
}

/* One command: the command block wrapper (31 bytes), the data stage, the status wrapper
   (13 bytes). 0, BOT_CHECK, or USB_E*. *got: data bytes moved. m->lock is held. */
static int bot_command(struct msc_lun *l, const u8 *cdb, u_int cdb_len, u_int len, bool in, u_int timeout_ms, u_int *got) {
    struct msc *m = l->m;
    struct usb_endpoint *dep = in ? m->in : m->out;
    u32 tag = ++m->tag;
    int r, tries;
    *got = 0;
    if(m->gone || m->udev->gone) return USB_ENODEV;
    memset(m->cbw, 0, 31);
    put32le(m->cbw, CBW_SIGNATURE);
    put32le(m->cbw + 4, tag);
    put32le(m->cbw + 8, len);
    m->cbw[12] = in ? 0x80 : 0;
    m->cbw[13] = l->lun;
    m->cbw[14] = (u8)cdb_len;
    memcpy(m->cbw + 15, cdb, cdb_len);
    r = usb_bulk_sync(m->out, m->cbw, 31, 5000);
    if(r == USB_ESTALL) usb_endpoint_recover(m->out, 5000);
    if(r != 31) return reset_recovery(m, r);

    if(len) {
        r = usb_bulk_sync(dep, m->data, len, timeout_ms);
        if(r == USB_ESTALL) {
            /* the device stopped the data stage early: clear the halt, then the status follows */
            if(usb_endpoint_recover(dep, 5000) != 0) return reset_recovery(m, USB_EIO);
        } else if(r < 0) {
            return reset_recovery(m, r);
        } else {
            *got = (u_int)r;
        }
    }
    for(tries = 0; tries < 2; tries++) {
        r = usb_bulk_sync(m->in, m->csw, 13, timeout_ms);
        if(r != USB_ESTALL) break;
        if(usb_endpoint_recover(m->in, 5000) != 0) return reset_recovery(m, USB_EIO);
    }
    if(r != 13 || le32(m->csw) != CSW_SIGNATURE || le32(m->csw + 4) != tag) return reset_recovery(m, r < 0 ? r : USB_EIO);
    if(m->csw[12] == 0) return 0;
    if(m->csw[12] == 1) return BOT_CHECK;
    return reset_recovery(m, USB_EIO);                 /* phase error */
}

static void request_sense(struct msc_lun *l) {
    static const u8 cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    struct msc *m = l->m;
    u_int got;
    m->sense_key = 0xff;
    m->asc = m->ascq = 0;
    if(bot_command(l, cdb, 6, 18, true, 5000, &got) == 0 && got >= 14) {
        m->sense_key = m->data[2] & 0x0f;
        m->asc = m->data[12];
        m->ascq = m->data[13];
    }
}

/* a command with REQUEST SENSE after a failure, retried after a unit attention (power-on, a
   medium change); 0, BOT_CHECK with m->sense_*, or USB_E*. m->lock is held. */
static int scsi(struct msc_lun *l, const u8 *cdb, u_int cdb_len, u_int len, bool in, u_int timeout_ms, u_int *got) {
    int r = USB_EIO, tries;
    for(tries = 0; tries < 3; tries++) {
        r = bot_command(l, cdb, cdb_len, len, in, timeout_ms, got);
        if(r != BOT_CHECK) return r;
        request_sense(l);
        if(l->m->sense_key != SK_UNIT_ATTENTION) return BOT_CHECK;
    }
    return r;
}

/**** units and disks ****/

/* a free name: not registered, and not the device of a mount that outlived its stick */
static void pick_name(char *name, size_t size) {
    u_int n;
    for(n = 0; n < 100; n++) {
        const struct mount *mt;
        bool used;
        snprintf(name, size, "usb%u", n);
        used = bdev_find(name) != NULL;
        for(mt = vfs_mounts(); mt && !used; mt = mt->next) {
            struct bdev *d = mt->sb->dev;
            if(d && (!strcmp(d->name, name) || (d->parent && !strcmp(d->parent->name, name)))) used = true;
        }
        if(!used) return;
    }
}

static void lun_drop(struct msc_lun *l, const char *why) {
    struct bdev *d = l->disk;
    if(!d) return;
    l->disk = NULL;
    bdev_unregister(d);
    kprintf("+USB: %s: %s\n", d->name, why);
}

/* What is in the unit: INQUIRY once, then whether a medium is there, its size, the
   write-protect switch; a disk for it. m->lock is held. */
static void lun_probe(struct msc_lun *l, bool announce) {
    struct msc *m = l->m;
    u8 cdb[16];
    u_int got, tries;
    u32 blocks, bs;
    bool wp = false;
    struct bdev *d;
    int r;

    if(!l->inquired) {
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x12;                                 /* INQUIRY */
        cdb[4] = 36;
        if(scsi(l, cdb, 6, 36, true, 5000, &got) != 0 || got < 5) return;
        l->inquired = true;
        if((m->data[0] >> 5) == 3) return;             /* no such unit */
        l->usable = (m->data[0] & 0x1f) == 0x00 || (m->data[0] & 0x1f) == 0x0e;   /* block devices only */
        l->removable = (m->data[1] & 0x80) != 0;
        if(got >= 32) {
            scsi_string(l->vendor, m->data + 8, 8);
            scsi_string(l->product, m->data + 16, 16);
        }
        if(!l->usable) {
            kprintf("+USB: port %u: unit %u is a SCSI device of type %02x (not a disk), not used\n",
                    m->udev->port, l->lun, m->data[0] & 0x1f);
            return;
        }
    }
    if(!l->usable || l->disk) return;

    /* a medium? (a stick may answer "becoming ready" for a moment after it is plugged in) */
    for(tries = 0; ; tries++) {
        memset(cdb, 0, sizeof(cdb));                   /* TEST UNIT READY */
        r = scsi(l, cdb, 6, 0, false, 5000, &got);
        if(r == 0) break;
        if(r != BOT_CHECK || tries >= 10) return;
        if(m->sense_key == SK_NOT_READY && m->asc == ASC_NO_MEDIUM) return;   /* an empty card reader slot */
        usb_msleep(100);
    }
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;                                     /* READ CAPACITY (10) */
    if(scsi(l, cdb, 10, 8, true, 5000, &got) != 0 || got < 8) return;
    blocks = be32(m->data);
    bs = be32(m->data + 4);
    if(blocks == 0xffffffff) {                         /* 2 TB or more: READ CAPACITY (16) */
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x9e;
        cdb[1] = 0x10;
        cdb[13] = 32;
        if(scsi(l, cdb, 16, 32, true, 5000, &got) != 0 || got < 12) return;
        bs = be32(m->data + 8);
        blocks = be32(m->data) ? 0xffffffff : be32(m->data + 4);     /* the block layer counts in 32 bits */
        kprintf("+USB: port %u: a disk of 2 TB or more: only its first 2 TB are used\n", m->udev->port);
    } else {
        blocks++;                                      /* READ CAPACITY gives the last block */
    }
    if(bs != BDEV_BLOCK_SIZE) {
        kprintf("+USB: port %u: %u-byte blocks are not supported (only 512)\n", m->udev->port, bs);
        l->usable = false;
        return;
    }
    memset(cdb, 0, sizeof(cdb));                       /* MODE SENSE (6), all pages: the write-protect bit */
    cdb[0] = 0x1a;
    cdb[2] = 0x3f;
    cdb[4] = 192;
    if(scsi(l, cdb, 6, 192, true, 5000, &got) == 0 && got >= 4) wp = (m->data[2] & 0x80) != 0;

    d = (struct bdev *)kcalloc(1, sizeof(struct bdev));
    if(!d) return;
    pick_name(d->name, sizeof(d->name));
    snprintf(d->model, sizeof(d->model), "%s%s%s", l->vendor, l->vendor[0] && l->product[0] ? " " : "", l->product);
    if(!d->model[0]) strlcpy(d->model, "USB disk", sizeof(d->model));
    d->block_size = BDEV_BLOCK_SIZE;
    d->nblocks = blocks;
    d->bus = BDEV_BUS_USB;
    d->removable = l->removable;
    d->readonly = wp;
    d->ops = &msc_ops;
    d->priv = l;
    if(bdev_register(d) != 0) { kfree(d); return; }
    l->disk = d;
    kprintf("+USB: %s: %s, %lu MB (%lu blocks)%s%s\n", d->name, d->model, (u_long)(blocks / 2048), (u_long)blocks,
            l->removable ? ", removable" : "", wp ? ", write-protected" : "");
    (void)announce;
    mutex_unlock(&m->lock);                            /* the partition scan reads through msc_read */
    bdev_scan_partitions(d);
    mutex_lock(&m->lock);
}

/**** the block device ****/

static int msc_rw(struct bdev *d, u32 lba, u32 count, u8 *buf, bool write) {
    struct msc_lun *l = (struct msc_lun *)d->priv;
    struct msc *m = l->m;
    int r = 0;
    mutex_lock(&m->lock);
    while(count && r == 0) {
        u32 n = count > MSC_CHUNK / BDEV_BLOCK_SIZE ? MSC_CHUNK / BDEV_BLOCK_SIZE : count;
        u8 cdb[10];
        u_int got;
        if(m->gone || m->udev->gone || d->gone) { r = BDEV_EGONE; break; }
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = write ? 0x2a : 0x28;                  /* WRITE (10) / READ (10) */
        put32be(cdb + 2, lba);
        cdb[7] = (u8)(n >> 8);
        cdb[8] = (u8)n;
        if(write) memcpy(m->data, buf, n * BDEV_BLOCK_SIZE);
        r = scsi(l, cdb, 10, n * BDEV_BLOCK_SIZE, !write, CMD_TIMEOUT, &got);
        if(r == 0 && got != n * BDEV_BLOCK_SIZE) r = USB_EIO;
        if(r == BOT_CHECK && m->sense_key == SK_NOT_READY) usb_wake();   /* the medium went: the usb thread drops it */
        if(r) {
            kprintf("+USB: %s: %s of block %lu failed (%s)\n", d->name, write ? "write" : "read", (u_long)lba,
                    r == BOT_CHECK ? "the device reported an error" : usb_strerror(r));
            break;
        }
        if(!write) memcpy(buf, m->data, n * BDEV_BLOCK_SIZE);
        lba += n;
        buf += n * BDEV_BLOCK_SIZE;
        count -= n;
    }
    mutex_unlock(&m->lock);
    if(r == 0) return 0;
    return (r == USB_ENODEV || d->gone) ? BDEV_EGONE : BDEV_EIO;
}

static int msc_read(struct bdev *d, u32 lba, u32 count, void *buf) {
    return msc_rw(d, lba, count, (u8 *)buf, false);
}

static int msc_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    return msc_rw(d, lba, count, (u8 *)buf, true);
}

/* SYNCHRONIZE CACHE (10): what the stick holds in RAM goes to flash. A device that does not
   know the command has no cache to flush. */
static int msc_flush(struct bdev *d) {
    struct msc_lun *l = (struct msc_lun *)d->priv;
    struct msc *m = l->m;
    u8 cdb[10];
    u_int got;
    int r;
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x35;
    mutex_lock(&m->lock);
    r = (m->gone || m->udev->gone) ? USB_ENODEV : scsi(l, cdb, 10, 0, false, CMD_TIMEOUT, &got);
    mutex_unlock(&m->lock);
    if(r == USB_ENODEV) return BDEV_EGONE;
    return r < 0 ? BDEV_EIO : 0;                       /* BOT_CHECK: not supported, nothing to flush */
}

static const struct bdev_ops msc_ops = { msc_read, msc_write, msc_flush };

/**** the USB driver ****/

/* a Bulk-Only interface whose commands are SCSI (or the SCSI-like RBC, MMC, SFF-8070i sets) */
static const struct usb_interface_descriptor *bot_interface(const u8 *cfg, u_int len) {
    const u8 *p;
    for(p = usb_desc_next(cfg, len, NULL); p; p = usb_desc_next(cfg, len, p)) {
        const struct usb_interface_descriptor *i = (const struct usb_interface_descriptor *)p;
        if(p[1] != USB_DT_INTERFACE || p[0] < 9 || i->bAlternateSetting != 0) continue;
        if(i->bInterfaceClass != USB_CLASS_STORAGE || i->bInterfaceProtocol != 0x50) continue;
        if(i->bInterfaceSubClass == 0x06 || i->bInterfaceSubClass == 0x01 || i->bInterfaceSubClass == 0x02 ||
           i->bInterfaceSubClass == 0x05) return i;
    }
    return NULL;
}

static bool msc_match(struct usb_device *dev, const u8 *cfg, u_int len) {
    return bot_interface(cfg, len) != NULL;
}

static void describe(struct msc *m) {
    struct usb_device *udev = m->udev;
    u_int i, o = 0;
    udev->status[0] = 0;
    for(i = 0; i < m->nluns && o + 20 < sizeof(udev->status); i++) {
        struct msc_lun *l = &m->luns[i];
        if(!l->usable) continue;
        if(l->disk) o += snprintf(udev->status + o, sizeof(udev->status) - o, "%s%s (%lu MB)", o ? ", " : "",
                                  l->disk->name, (u_long)(l->disk->nblocks / 2048));
        else o += snprintf(udev->status + o, sizeof(udev->status) - o, "%sno medium", o ? ", " : "");
    }
    if(!o) snprintf(udev->status, sizeof(udev->status), "storage: no usable unit");
}

static int msc_attach(struct usb_device *udev) {
    const struct usb_interface_descriptor *ifd = bot_interface(udev->config, udev->config_len);
    const struct usb_endpoint_descriptor *in = NULL, *out = NULL;
    const struct usb_ss_ep_comp_descriptor *in_comp = NULL, *out_comp = NULL;
    const u8 *p;
    struct msc *m;
    u8 max_lun = 0;
    u_int i;
    int r;

    if(!ifd) return USB_EINVAL;
    for(p = usb_desc_next(udev->config, udev->config_len, (const u8 *)ifd); p && p[1] != USB_DT_INTERFACE;
        p = usb_desc_next(udev->config, udev->config_len, p)) {
        const struct usb_endpoint_descriptor *e = (const struct usb_endpoint_descriptor *)p;
        const u8 *n;
        if(p[1] != USB_DT_ENDPOINT || p[0] < 7 || (e->bmAttributes & 3) != USB_EP_BULK) continue;
        n = usb_desc_next(udev->config, udev->config_len, p);
        if(e->bEndpointAddress & USB_DIR_IN) {
            if(!in) { in = e; if(n && n[1] == USB_DT_SS_EP_COMP) in_comp = (const struct usb_ss_ep_comp_descriptor *)n; }
        } else if(!out) {
            out = e;
            if(n && n[1] == USB_DT_SS_EP_COMP) out_comp = (const struct usb_ss_ep_comp_descriptor *)n;
        }
    }
    if(!in || !out) return USB_EINVAL;

    m = (struct msc *)kcalloc(1, sizeof(struct msc));
    if(!m) return USB_ENOMEM;
    m->udev = udev;
    m->iface = ifd->bInterfaceNumber;
    mutex_init(&m->lock, "usb storage");
    m->cbw = dma_block(512, 512, &m->small_raw);         /* the command and status wrappers */
    m->data = dma_block(MSC_CHUNK, MSC_CHUNK, &m->data_raw);
    if(!m->cbw || !m->data) {
        if(m->small_raw) kfree_dma(m->small_raw);
        if(m->data_raw) kfree_dma(m->data_raw);
        kfree(m);
        return USB_ENOMEM;
    }
    m->csw = m->cbw + 64;
    m->in = usb_endpoint_add(udev, in, in_comp);
    m->out = usb_endpoint_add(udev, out, out_comp);
    r = (!m->in || !m->out) ? USB_ENOMEM : usb_configure_endpoints(udev);
    if(r == 0) r = usb_set_configuration(udev, udev->config_value);
    if(r < 0) {
        kprintf("+USB: port %u: storage: setting it up failed (%s)\n", udev->port, usb_strerror(r));
        kfree_dma(m->small_raw);
        kfree_dma(m->data_raw);
        kfree(m);
        return r;
    }
    /* units: GET MAX LUN, which a single-unit device may stall */
    if(usb_control(udev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE, BOT_GET_MAX_LUN, 0, m->iface, &max_lun, 1) != 1)
        max_lun = 0;
    m->nluns = (u8)((max_lun < MSC_MAX_LUNS ? max_lun : MSC_MAX_LUNS - 1) + 1);
    udev->driver_data = m;
    mutex_lock(&m->lock);
    for(i = 0; i < m->nluns; i++) {
        m->luns[i].m = m;
        m->luns[i].lun = (u8)i;
        lun_probe(&m->luns[i], false);
        m->luns[i].next_poll = getticks() + POLL_TICKS;
    }
    describe(m);
    mutex_unlock(&m->lock);
    return 0;
}

/* the usb thread, every second or so: a medium put in or taken out of a removable unit, or
   a drive that was not ready when it was plugged in */
static void msc_service(struct usb_device *udev) {
    struct msc *m = (struct msc *)udev->driver_data;
    bool changed = false;
    u_int i;
    if(!m || m->gone) return;
    mutex_lock(&m->lock);
    for(i = 0; i < m->nluns; i++) {
        struct msc_lun *l = &m->luns[i];
        u8 cdb[6];
        u_int got;
        int r;
        if(!l->usable || (l->disk && !l->removable) || (long)(getticks() - l->next_poll) < 0) continue;
        l->next_poll = getticks() + POLL_TICKS;
        if(!l->disk) {                                 /* an empty slot, or a drive still spinning up */
            lun_probe(l, true);
            if(l->disk) changed = true;
            continue;
        }
        memset(cdb, 0, sizeof(cdb));                   /* TEST UNIT READY */
        r = scsi(l, cdb, 6, 0, false, 5000, &got);
        if(r == BOT_CHECK && m->sense_key == SK_NOT_READY) {
            lun_drop(l, "the medium was taken out");
            changed = true;
        }
    }
    if(changed) describe(m);
    mutex_unlock(&m->lock);
}

/* unplugged: wait for a task inside a command to come out (every wait in here gives up
   once the device is gone), then refuse everything */
static void msc_quiesce(struct usb_device *udev) {
    struct msc *m = (struct msc *)udev->driver_data;
    if(!m) return;
    mutex_lock(&m->lock);
    m->gone = true;
    mutex_unlock(&m->lock);
}

static void msc_detach(struct usb_device *udev) {
    struct msc *m = (struct msc *)udev->driver_data;
    u_int i;
    if(!m) return;
    m->gone = true;
    for(i = 0; i < m->nluns; i++) lun_drop(&m->luns[i], "unplugged");
    if(!udev->dma_live) {                              /* else the controller may still write there */
        kfree_dma(m->small_raw);
        kfree_dma(m->data_raw);
    }
    m->cbw = m->csw = m->data = NULL;
    udev->driver_data = NULL;                          /* m itself stays: a disk's priv still points into it */
}

const struct usb_driver msc_driver = { "storage", msc_match, msc_attach, msc_quiesce, msc_detach, msc_service };
