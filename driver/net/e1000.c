/*
SurfOS Intel 8254x (e1000) Ethernet Driver
--------------------
File: e1000.c   Date: 10/3/26 (roadmap N1)
--------------------
The NIC QEMU provides by default (82540EM, 8086:100e). Registers are memory
mapped through ioremap(); receive and transmit descriptor rings and their
buffers live in the 1:1 DMA heap. Received frames are copied to the stack
from the interrupt handler (netdev_rx queues them for the net thread);
transmit fills the next descriptor and bumps the tail.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/interrupt.h>
#include <surfos/timer.h>
#include <surfos/irq.h>
#include <sys/pci.h>
#include <sys/driver.h>
#include <mm/paging.h>
#include <mm/kalloc.h>
#include <net/net.h>
#include <blibc_common.h>

#define E1000_VENDOR 0x8086
#define E1000_DEVICE 0x100E

/* registers (byte offsets) */
#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_EERD    0x0014
#define REG_ICR     0x00C0
#define REG_IMS     0x00D0
#define REG_IMC     0x00D8
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_TIPG    0x0410
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_RDTR    0x2820
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818
#define REG_MTA     0x5200
#define REG_RAL     0x5400
#define REG_RAH     0x5404

#define CTRL_SLU    (1 << 6)
#define CTRL_RST    (1 << 26)
#define STATUS_LU   (1 << 1)
#define RCTL_EN     (1 << 1)
#define RCTL_SBP    (1 << 2)
#define RCTL_UPE    (1 << 3)
#define RCTL_MPE    (1 << 4)
#define RCTL_BAM    (1 << 15)
#define RCTL_BSIZE_2048 0
#define RCTL_SECRC  (1 << 26)
#define TCTL_EN     (1 << 1)
#define TCTL_PSP    (1 << 3)
#define TCTL_CT(x)  ((x) << 4)
#define TCTL_COLD(x) ((x) << 12)
#define ICR_TXDW    (1 << 0)
#define ICR_LSC     (1 << 2)
#define ICR_RXDMT0  (1 << 4)
#define ICR_RXO     (1 << 6)
#define ICR_RXT0    (1 << 7)
#define RAH_AV      (1 << 31)

#define RX_DESCS 32
#define TX_DESCS 32
#define BUF_SIZE 2048

struct rx_desc {
    u64 addr;
    u16 length, csum;
    u8 status, errors;
    u16 special;
} __attribute__((packed));

struct tx_desc {
    u64 addr;
    u16 length;
    u8 cso, cmd, status, css;
    u16 special;
} __attribute__((packed));

#define RXD_STAT_DD  1
#define RXD_STAT_EOP 2
#define TXD_CMD_EOP  (1 << 0)
#define TXD_CMD_IFCS (1 << 1)
#define TXD_CMD_RS   (1 << 3)
#define TXD_STAT_DD  1

struct e1000 {
    volatile u32 *regs;
    struct pci_dev *pci;
    struct rx_desc *rx;                 /* DMA heap: physical == virtual */
    struct tx_desc *tx;
    u8 *rx_buf[RX_DESCS], *tx_buf[TX_DESCS];
    u_int rx_next, tx_next;
    struct netdev dev;
    u_long irqs;
};

static inline u32 rd(struct e1000 *e, u32 reg) { return e->regs[reg / 4]; }
static inline void wr(struct e1000 *e, u32 reg, u32 val) { e->regs[reg / 4] = val; }

static bool eeprom_read(struct e1000 *e, u8 addr, u16 *val) {
    u32 v;
    int spins;
    wr(e, REG_EERD, ((u32)addr << 8) | 1);
    for(spins = 0; spins < 100000; spins++) {
        v = rd(e, REG_EERD);
        if(v & (1 << 4)) { *val = (u16)(v >> 16); return true; }
    }
    return false;
}

static int e1000_send(struct netdev *dev, struct netbuf *nb) {
    struct e1000 *e = (struct e1000 *)dev->priv;
    u_int i = e->tx_next, spins;
    struct tx_desc *d = &e->tx[i];
    u_long flags;
    if(nb->len > BUF_SIZE) return -1;
    for(spins = 0; spins < 100000 && !(d->status & TXD_STAT_DD) && d->cmd; spins++) udelay(10); /* ring full? wait */
    if(!(d->status & TXD_STAT_DD) && d->cmd) return -1;
    flags = irq_save();
    memcpy(e->tx_buf[i], netbuf_data(nb), nb->len);
    d->addr = (u64)(u_long)e->tx_buf[i];
    d->length = (u16)nb->len;
    d->cso = d->css = 0;
    d->status = 0;
    d->cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    e->tx_next = (i + 1) % TX_DESCS;
    wr(e, REG_TDT, e->tx_next);
    irq_restore(flags);
    return 0;
}

static void e1000_receive(struct e1000 *e) {
    for(;;) {
        struct rx_desc *d = &e->rx[e->rx_next];
        if(!(d->status & RXD_STAT_DD)) break;
        if((d->status & RXD_STAT_EOP) && !d->errors) netdev_rx(&e->dev, e->rx_buf[e->rx_next], d->length);
        else e->dev.rx_dropped++;
        d->status = 0;
        wr(e, REG_RDT, e->rx_next);            /* this slot is free again */
        e->rx_next = (e->rx_next + 1) % RX_DESCS;
    }
}

static int e1000_isr(u_int irq, void *param) {
    struct e1000 *e = (struct e1000 *)param;
    u32 icr = rd(e, REG_ICR);                  /* reading acknowledges */
    if(!icr) return 0;
    e->irqs++;
    if(icr & ICR_LSC) e->dev.link_up = (rd(e, REG_STATUS) & STATUS_LU) != 0;
    if(icr & (ICR_RXT0 | ICR_RXDMT0 | ICR_RXO)) e1000_receive(e);
    return 0;
}

static const struct netdev_ops e1000_ops = { e1000_send, NULL };

static int e1000_attach(struct pci_dev *pci) {
    struct e1000 *e = (struct e1000 *)kcalloc(1, sizeof(struct e1000));
    u16 w;
    int i;
    u8 *ring;

    if(!e) return -1;
    e->pci = pci;
    pci_enable_device(pci);                    /* memory decoding and bus mastering */
    e->regs = (volatile u32 *)ioremap(pci->bar[0], pci->bar_size[0] ? pci->bar_size[0] : 0x20000, false);
    if(!e->regs) { kfree(e); return -1; }

    wr(e, REG_IMC, 0xFFFFFFFF);
    wr(e, REG_CTRL, rd(e, REG_CTRL) | CTRL_RST);
    mdelay(10);
    wr(e, REG_IMC, 0xFFFFFFFF);
    wr(e, REG_CTRL, (rd(e, REG_CTRL) | CTRL_SLU) & ~(1 << 3));   /* link up, no LRST */

    /* the MAC address: EEPROM words 0-2, else the receive address register the firmware left */
    if(eeprom_read(e, 0, &w)) {
        e->dev.mac[0] = w & 0xFF; e->dev.mac[1] = w >> 8;
        eeprom_read(e, 1, &w); e->dev.mac[2] = w & 0xFF; e->dev.mac[3] = w >> 8;
        eeprom_read(e, 2, &w); e->dev.mac[4] = w & 0xFF; e->dev.mac[5] = w >> 8;
    } else {
        u32 ral = rd(e, REG_RAL), rah = rd(e, REG_RAH);
        e->dev.mac[0] = ral; e->dev.mac[1] = ral >> 8; e->dev.mac[2] = ral >> 16; e->dev.mac[3] = ral >> 24;
        e->dev.mac[4] = rah; e->dev.mac[5] = rah >> 8;
    }
    wr(e, REG_RAL, e->dev.mac[0] | (e->dev.mac[1] << 8) | (e->dev.mac[2] << 16) | ((u32)e->dev.mac[3] << 24));
    wr(e, REG_RAH, e->dev.mac[4] | (e->dev.mac[5] << 8) | RAH_AV);
    for(i = 0; i < 128; i++) wr(e, REG_MTA + i * 4, 0);

    /* rings: 16-byte aligned, in the DMA heap so the addresses are physical */
    ring = (u8 *)kalloc_dma(RX_DESCS * sizeof(struct rx_desc) + TX_DESCS * sizeof(struct tx_desc) + 128);
    if(!ring) { kfree(e); return -1; }
    e->rx = (struct rx_desc *)(((u_long)ring + 127) & ~127UL);
    e->tx = (struct tx_desc *)((u8 *)e->rx + RX_DESCS * sizeof(struct rx_desc));
    memset(e->rx, 0, RX_DESCS * sizeof(struct rx_desc) + TX_DESCS * sizeof(struct tx_desc));
    for(i = 0; i < RX_DESCS; i++) {
        e->rx_buf[i] = (u8 *)kalloc_dma(BUF_SIZE);
        if(!e->rx_buf[i]) return -1;
        e->rx[i].addr = (u64)(u_long)e->rx_buf[i];
    }
    for(i = 0; i < TX_DESCS; i++) {
        e->tx_buf[i] = (u8 *)kalloc_dma(BUF_SIZE);
        if(!e->tx_buf[i]) return -1;
        e->tx[i].status = TXD_STAT_DD;         /* free */
    }
    wr(e, REG_RDBAL, (u32)(u_long)e->rx);
    wr(e, REG_RDBAH, 0);
    wr(e, REG_RDLEN, RX_DESCS * sizeof(struct rx_desc));
    wr(e, REG_RDH, 0);
    wr(e, REG_RDT, RX_DESCS - 1);
    wr(e, REG_RDTR, 0);
    wr(e, REG_TDBAL, (u32)(u_long)e->tx);
    wr(e, REG_TDBAH, 0);
    wr(e, REG_TDLEN, TX_DESCS * sizeof(struct tx_desc));
    wr(e, REG_TDH, 0);
    wr(e, REG_TDT, 0);
    wr(e, REG_TIPG, 10 | (8 << 10) | (6 << 20));
    wr(e, REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT(0x10) | TCTL_COLD(0x40));
    wr(e, REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC);
    /* QEMU's model holds every frame that arrives within one second of this RCTL write (a workaround
       for drivers that enable RX before the ring is set up), so the first reply after boot is late. */

    e->dev.ops = &e1000_ops;
    e->dev.priv = e;
    e->dev.mtu = ETH_MTU;
    e->dev.link_up = (rd(e, REG_STATUS) & STATUS_LU) != 0;
    netdev_register(&e->dev);

    add_irq_handler(pci->irq, e1000_isr, e);
    _enable_irq(pci->irq);
    rd(e, REG_ICR);
    wr(e, REG_IMS, ICR_RXT0 | ICR_RXDMT0 | ICR_RXO | ICR_LSC);   /* no TXDW: transmit completion is polled */
    {
        char m[18];
        kprintf("+E1000: %s is %02x:%02x.%x at 0x%08lx irq %u, MAC %s, link %s\n", e->dev.name, pci->bus, pci->slot, pci->func,
                (u_long)pci->bar[0], pci->irq, macfmt(e->dev.mac, m), e->dev.link_up ? "up" : "down");
    }
    return 0;
}

int init_e1000(void) {
    struct pci_dev *d = NULL;
    int found = 0;
    while((d = pci_find_device(E1000_VENDOR, E1000_DEVICE, d)) != NULL) {
        if(e1000_attach(d) == 0) found++;
    }
    return found ? 0 : 1;
}
