/*
SurfOS Wi-Fi: the Intel Wireless-AC 9560's firmware transport
--------------------
File: iwl_trans.c  Date: 10/10/26 (the Wi-Fi brief: docs/superpowers/plans/2026-10-09-wifi.md, track I)
--------------------
The host memory the card reads and writes, and what goes through it (W1, step 3):
* the context info (include/sys/iwl_reg.h): where the firmware's sections are, where the
  receive queue and the command queue are. The card loads its firmware from it by itself
  once UREG_CPU_INIT_RUN is set ("self load": no section is written through registers);
* the firmware's sections copied into the 1:1 DMA heap, 4 KB aligned: the runtime image
  (the first CPU's and the second CPU's, about 700 KB) is freed once the firmware reports
  ALIVE; the paged sections (about 600 KB) stay while it runs, as the card fetches them;
* the receive queue: IWL_RX_RING buffers of 4 KB with their free and used lists and the
  status the card writes. Buffers are read in order and handed back in place, as iwx does
  for everything but received frames;
* the command queue (queue 0): IWL_CMD_RING TFDs, each with a command buffer, a command
  being its header and payload in one or two pieces (the first 20 bytes, then the rest).

Polled, never interrupted: iwl_poll() reads the interrupt causes (ALIVE, a firmware or
hardware error, the radio switch) and the receive queue, under rx_lock; the driver's
thread (iwl.c) calls it every 10 ms while the firmware runs, and a command's sender calls
it while it waits for the response, so whoever gets there first handles a packet.

Follows OpenBSD's sys/dev/pci/if_iwx.c (iwx_ctxt_info_init(), iwx_init_fw_sec(),
iwx_start_fw(), iwx_load_firmware(), iwx_send_cmd(), iwx_notif_intr(), iwx_rx_pkt(),
iwx_intr(), iwx_nic_error()); the notices are in iwl.c.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/timer.h>
#include <mm/kalloc.h>
#include <fs/vfs.h>
#include <blibc_common.h>
#include "iwl_priv.h"

#define ALIGN4K(n)  (((n) + 4095u) & ~4095u)

static inline u32 phys(const void *p) { return (u32)(u_long)p; }    /* the DMA heap is mapped 1:1 below 1 GB */

/* zeroed DMA heap memory aligned to align (a power of two); *raw is what kfree_dma() takes */
static void *dma_zalloc(u_int size, u_int align, void **raw) {
    u8 *p = (u8 *)kalloc_dma(size + align), *a;
    *raw = p;
    if(!p) return NULL;
    a = (u8 *)(((u_long)p + align - 1) & ~(u_long)(align - 1));
    memset(a, 0, size);
    return a;
}

static void dma_free(void **raw) {
    if(*raw) kfree_dma(*raw);
    *raw = NULL;
}

/* log2 of a power of two */
static u_int log2u(u_int n) {
    u_int l = 0;
    while(n > 1) { n >>= 1; l++; }
    return l;
}

/**** allocation, once ****/

int iwl_trans_alloc(struct iwl_softc *sc) {
    struct iwl_rx_ring *rx = &sc->rxq;
    struct iwl_cmd_ring *cq = &sc->cmdq;
    if(sc->trans_ready) return 0;
    sc->ctxt = (struct iwl_context_info *)dma_zalloc(sizeof(struct iwl_context_info), 4096, &sc->raw_ctxt);
    rx->free = (u64 *)dma_zalloc(IWL_RX_RING * sizeof(u64), 256, &rx->raw_free);
    rx->used = (u32 *)dma_zalloc(IWL_RX_RING * sizeof(u32), 256, &rx->raw_used);
    rx->stat = (volatile struct iwl_rb_status *)dma_zalloc(sizeof(struct iwl_rb_status), 16, &rx->raw_stat);
    rx->buf = (u8 *)dma_zalloc(IWL_RX_RING * IWL_RBUF_SIZE, 4096, &rx->raw_buf);
    cq->desc = (struct iwl_tfh_tfd *)dma_zalloc(IWL_CMD_RING * sizeof(struct iwl_tfh_tfd), 256, &cq->raw_desc);
    cq->cmd = (struct iwl_device_cmd *)dma_zalloc(IWL_CMD_RING * sizeof(struct iwl_device_cmd), 64, &cq->raw_cmd);
    if(!sc->ctxt || !rx->free || !rx->used || !rx->stat || !rx->buf || !cq->desc || !cq->cmd) {
        kprintf("+IWL: not enough DMA memory for the queues\n");
        dma_free(&sc->raw_ctxt);
        dma_free(&rx->raw_free);
        dma_free(&rx->raw_used);
        dma_free(&rx->raw_stat);
        dma_free(&rx->raw_buf);
        dma_free(&cq->raw_desc);
        dma_free(&cq->raw_cmd);
        return -ENOMEM;
    }
    mutex_init(&sc->rx_lock, "iwl-rx");
    mutex_init(&sc->cmd_lock, "iwl-cmd");
    sc->trans_ready = true;
    return 0;
}

/* every free buffer's address and index on the free list (if_iwx.c iwx_update_rx_desc()) */
static void post_rx_buffers(struct iwl_softc *sc) {
    u_int i;
    for(i = 0; i < IWL_RX_RING; i++) sc->rxq.free[i] = (u64)phys(sc->rxq.buf + i * IWL_RBUF_SIZE) | (i & 0xfff);
}

static void reset_rings(struct iwl_softc *sc) {
    sc->rxq.cur = 0;
    memset((void *)sc->rxq.stat, 0, sizeof(struct iwl_rb_status));
    post_rx_buffers(sc);
    memset(sc->cmdq.desc, 0, IWL_CMD_RING * sizeof(struct iwl_tfh_tfd));
    sc->cmdq.cur = 0;
    sc->cmdq.cur_hw = 0;
}

/**** the firmware's sections in host memory ****/

/* count sections from first, each copied to a 4 KB boundary of one block; their addresses in table */
static int copy_sections(const struct iwl_fw *fw, u_int first, u_int count, u64 *table, void **raw) {
    u_int i, total = 0, off = 0;
    u8 *base;
    if(count > IWL_MAX_DRAM_ENTRY) return -EINVAL;
    for(i = 0; i < count; i++) total += ALIGN4K(fw->sec[first + i].len);
    if(!total) return 0;
    base = (u8 *)dma_zalloc(total, 4096, raw);
    if(!base) return -ENOMEM;
    for(i = 0; i < count; i++) {
        const struct iwl_fw_section *s = &fw->sec[first + i];
        memcpy(base + off, s->data, s->len);
        table[i] = phys(base + off);
        off += ALIGN4K(s->len);
    }
    return 0;
}

/* the runtime image (the first CPU's and the second CPU's sections, in one block) and the paged
   sections (in another) (if_iwx.c iwx_init_fw_sec()) */
static int load_sections(struct iwl_softc *sc, const struct iwl_fw *fw) {
    void *raw_lmac = NULL, *raw_umac = NULL;
    int r;
    r = copy_sections(fw, fw->lmac_first, fw->lmac_count, sc->ctxt->lmac_img, &raw_lmac);
    if(!r) r = copy_sections(fw, fw->umac_first, fw->umac_count, sc->ctxt->umac_img, &raw_umac);
    if(!r) r = copy_sections(fw, fw->paging_first, fw->paging_count, sc->ctxt->virtual_img, &sc->raw_paging);
    if(r) {
        dma_free(&raw_lmac);
        dma_free(&raw_umac);
        dma_free(&sc->raw_paging);
        return r;
    }
    /* both halves of the runtime image go together once ALIVE comes: keep them as one list */
    sc->raw_img = raw_lmac;
    sc->raw_img_umac = raw_umac;
    return 0;
}

static void free_image(struct iwl_softc *sc) {
    dma_free(&sc->raw_img);
    dma_free(&sc->raw_img_umac);
}

/**** firmware load ****/

/* the MAC's step and the firmware's radio type, step and dash in HW_IF_CONFIG (if_iwx.c iwx_nic_config()) */
static void nic_config(struct iwl_softc *sc) {
    u32 radio_type = sc->phy_sku & 0x3, radio_step = (sc->phy_sku >> 2) & 0x3, radio_dash = (sc->phy_sku >> 4) & 0x3;
    u32 v, mask = IWL_HW_IF_CONFIG_MSK_MAC_DASH | IWL_HW_IF_CONFIG_MSK_MAC_STEP | IWL_HW_IF_CONFIG_MSK_PHY_STEP |
                  IWL_HW_IF_CONFIG_MSK_PHY_DASH | IWL_HW_IF_CONFIG_MSK_PHY_TYPE | IWL_HW_IF_CONFIG_BIT_RADIO_SI |
                  IWL_HW_IF_CONFIG_BIT_MAC_SI;
    v = iwl_rd(sc, IWL_CSR_HW_IF_CONFIG_REG) & ~mask;
    v |= sc->mac_step << IWL_HW_IF_CONFIG_POS_MAC_STEP;    /* the dash is 0 in this family */
    v |= radio_type << IWL_HW_IF_CONFIG_POS_PHY_TYPE;
    v |= radio_step << IWL_HW_IF_CONFIG_POS_PHY_STEP;
    v |= radio_dash << IWL_HW_IF_CONFIG_POS_PHY_DASH;
    iwl_wr(sc, IWL_CSR_HW_IF_CONFIG_REG, v);
}

int iwl_load_firmware(struct iwl_softc *sc, const struct iwl_fw *fw) {
    struct iwl_context_info *ci = sc->ctxt;
    u_long deadline;
    int r;

    /* iwx_start_fw(): the radio switch handshake bits cleared, the NIC set up */
    iwl_wr(sc, IWL_CSR_INT, ~0u);
    iwl_disable_interrupts(sc);
    iwl_wr(sc, IWL_CSR_UCODE_DRV_GP1_CLR, IWL_UCODE_SW_BIT_RFKILL);
    iwl_wr(sc, IWL_CSR_UCODE_DRV_GP1_CLR, IWL_UCODE_DRV_GP1_BIT_CMD_BLOCKED);
    iwl_wr(sc, IWL_CSR_INT, ~0u);
    r = iwl_apm_init(sc);                       /* iwx_nic_init(): again, as iwx does */
    if(r) return r;
    nic_config(sc);
    *(volatile u8 *)(sc->regs + IWL_CSR_INT_COALESCING) = IWL_HOST_INT_TIMEOUT_DEF;
    iwl_setbits(sc, IWL_CSR_MAC_SHADOW_REG_CTRL, 0x800fffff);   /* write pointers without waking the MAC */
    /* the causes read (iwx's whole set, so that an error shows while it loads too); the line itself stays
       off: INTx disabled, no MSI */
    iwl_wr(sc, IWL_CSR_INT_MASK, IWL_INT_INI_SET_MASK);

    /* iwx_ctxt_info_init() */
    reset_rings(sc);
    memset(ci, 0, sizeof(*ci));
    ci->mac_id = (u16)iwl_rd(sc, IWL_CSR_HW_REV);
    ci->version = 0;
    ci->size = sizeof(*ci) / 4;
    ci->control_flags = IWL_CTXT_INFO_TFD_FORMAT_LONG | (log2u(IWL_RX_RING) << IWL_CTXT_INFO_RB_CB_SIZE_POS) |
                        (IWL_CTXT_INFO_RB_SIZE_4K << IWL_CTXT_INFO_RB_SIZE_POS);
    ci->free_rbd_addr = phys(sc->rxq.free);
    ci->used_rbd_addr = phys(sc->rxq.used);
    ci->status_wr_ptr = phys((const void *)sc->rxq.stat);
    ci->cmd_queue_addr = phys(sc->cmdq.desc);
    ci->cmd_queue_size = (u8)(log2u(IWL_CMD_RING) - 3);
    r = load_sections(sc, fw);
    if(r) {
        kprintf("+IWL: the firmware's sections do not fit in DMA memory (%s)\n", strerror(r));
        return r;
    }
    sc->alive_seen = false;
    sc->alive_status = 0;
    sc->init_complete = false;
    sc->error_dumped = false;
    sc->fw_state = IWL_FW_LOADING;
    iwl_wr(sc, IWL_CSR_CTXT_INFO_BA, phys(ci));
    iwl_wr(sc, IWL_CSR_CTXT_INFO_BA + 4, 0);
    if(!iwl_nic_lock(sc)) {
        sc->fw_state = IWL_FW_DOWN;
        free_image(sc);
        return -EBUSY;
    }
    iwl_prph_write(sc, IWL_HPM_MAC_LTR_CSR, IWL_HPM_MAC_LTR_ENABLE_ALL);    /* iwx_set_ltr(): integrated */
    iwl_prph_write(sc, IWL_HPM_UMAC_LTR, IWL_LTR_VAL_250US);
    iwl_prph_write(sc, IWL_UREG_CPU_INIT_RUN, 1);                          /* the card loads it now */
    iwl_nic_unlock(sc);

    /* ALIVE: the interrupt cause first (the card has set up its receive DMA), then the notification */
    deadline = getticks() + (IWL_ALIVE_TIMEOUT_MS * HZ) / 1000 + 1;
    while(!sc->alive_seen && sc->fw_state == IWL_FW_LOADING && (long)(getticks() - deadline) < 0) {
        iwl_poll(sc);
        if(!sc->alive_seen) sleep_ms(1);
    }
    free_image(sc);                             /* what the card copied; the paged sections stay */
    if(!sc->alive_seen) {
        kprintf("+IWL: no ALIVE from the firmware in %u ms (CSR_INT 0x%08x, GP_CNTRL 0x%08x)\n", IWL_ALIVE_TIMEOUT_MS,
                iwl_rd(sc, IWL_CSR_INT), iwl_rd(sc, IWL_CSR_GP_CNTRL));
        if(sc->fw_state == IWL_FW_ERROR) iwl_dump_error(sc);
        return -ETIMEDOUT;
    }
    if(sc->alive_status != IWL_ALIVE_STATUS_OK) {
        kprintf("+IWL: the firmware reports ALIVE with status 0x%04x, not 0xcafe\n", sc->alive_status);
        iwl_dump_error(sc);
        return -EIO;
    }
    sc->fw_state = IWL_FW_ALIVE;
    return 0;
}

/* the receive DMA stopped, the queues reset, the paged sections freed (if_iwx.c iwx_stop_device()'s
   first half); the card itself is stopped by iwl.c */
void iwl_trans_stop(struct iwl_softc *sc) {
    u_int i;
    if(!sc->trans_ready) return;
    mutex_lock(&sc->rx_lock);
    sc->fw_state = IWL_FW_DOWN;
    if(iwl_nic_lock(sc)) {
        iwl_prph_write(sc, IWL_RFH_RXF_DMA_CFG, 0);
        for(i = 0; i < 1000 && !(iwl_prph_read(sc, IWL_RFH_GEN_STATUS) & IWL_RXF_DMA_IDLE); i++) udelay(10);
        iwl_nic_unlock(sc);
    }
    reset_rings(sc);
    free_image(sc);
    dma_free(&sc->raw_paging);
    sc->cmd_done = true;                        /* a waiter, if any, gives up */
    sc->cmd_failed = true;
    mutex_unlock(&sc->rx_lock);
}

/**** what the card sends back ****/

static const char *error_name(u32 id) {
    static const struct { const char *name; u8 num; } names[] = {
        { "NMI_INTERRUPT_WDG", 0x34 }, { "SYSASSERT", 0x35 }, { "UCODE_VERSION_MISMATCH", 0x37 },
        { "BAD_COMMAND", 0x38 }, { "BAD_COMMAND", 0x39 }, { "NMI_INTERRUPT_DATA_ACTION_PT", 0x3c },
        { "FATAL_ERROR", 0x3d }, { "NMI_TRM_HW_ERR", 0x46 }, { "NMI_INTERRUPT_TRM", 0x4c },
        { "NMI_INTERRUPT_BREAK_POINT", 0x54 }, { "NMI_INTERRUPT_WDG_RXF_FULL", 0x5c },
        { "NMI_INTERRUPT_WDG_NO_RBD_RXF_FULL", 0x64 }, { "NMI_INTERRUPT_HOST", 0x66 },
        { "NMI_INTERRUPT_LMAC_FATAL", 0x70 }, { "NMI_INTERRUPT_UMAC_FATAL", 0x71 },
        { "NMI_INTERRUPT_OTHER_LMAC_FATAL", 0x73 }, { "NMI_INTERRUPT_ACTION_PT", 0x7c },
        { "NMI_INTERRUPT_UNKNOWN", 0x84 }, { "NMI_INTERRUPT_INST_ACTION_PT", 0x86 },
    };
    u_int i;
    for(i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if(names[i].num == (id & ~0xf0000000u)) return names[i].name;
    return "ADVANCED_SYSASSERT";
}

/* dwords of the card's memory (if_iwx.c iwx_read_mem()) */
static int read_mem(struct iwl_softc *sc, u32 addr, u32 *buf, u_int dwords) {
    u_int i;
    if(!iwl_nic_lock(sc)) return -EBUSY;
    iwl_wr(sc, IWL_HBUS_TARG_MEM_RADDR, addr);
    for(i = 0; i < dwords; i++) buf[i] = iwl_rd(sc, IWL_HBUS_TARG_MEM_RDAT);
    iwl_nic_unlock(sc);
    return 0;
}

/* the firmware's error logs, as iwx_nic_error() and iwx_nic_umac_error() print them (the lines that
   say most); once per start */
void iwl_dump_error(struct iwl_softc *sc) {
    struct iwl_error_table t;
    struct iwl_umac_error_table u;
    if(sc->error_dumped) return;
    sc->error_dumped = true;
    if(sc->lmac_error_table < IWL_ERROR_TABLE_MIN ||
       read_mem(sc, sc->lmac_error_table, (u32 *)&t, sizeof(t) / 4) || !t.valid) {
        kprintf("+IWL: no firmware error log (its address 0x%08x)\n", sc->lmac_error_table);
    } else {
        kprintf("+IWL: firmware error log: 0x%08x %s\n", t.error_id, error_name(t.error_id));
        kprintf("+IWL:   trm_hw_status %08x %08x, branchlink2 %08x, interruptlink %08x %08x\n", t.trm_hw_status0,
                t.trm_hw_status1, t.blink2, t.ilink1, t.ilink2);
        kprintf("+IWL:   data %08x %08x %08x, time gp1 %08x gp2 %08x, ucode %u.%u type %08x, hw %08x\n", t.data1,
                t.data2, t.data3, t.gp1, t.gp2, t.major, t.minor, t.fw_rev_type, t.hw_ver);
        kprintf("+IWL:   last host command %08x (id %08x), isr %08x %08x %08x %08x %08x\n", t.hcmd, t.last_cmd_id,
                t.isr0, t.isr1, t.isr2, t.isr3, t.isr4);
    }
    if(sc->umac_error_table >= IWL_ERROR_TABLE_MIN && !read_mem(sc, sc->umac_error_table, (u32 *)&u, sizeof(u) / 4)
       && u.valid) {
        kprintf("+IWL: UMAC error log: 0x%08x %s, branchlink %08x %08x, interruptlink %08x %08x\n", u.error_id,
                error_name(u.error_id), u.blink1, u.blink2, u.ilink1, u.ilink2);
        kprintf("+IWL:   data %08x %08x %08x, umac %u.%u, last host command %08x\n", u.data1, u.data2, u.data3,
                u.umac_major, u.umac_minor, u.cmd_header);
    }
}

static void handle_alive(struct iwl_softc *sc, const struct iwl_rx_packet *pkt, u_int paylen) {
    const struct iwl_alive_resp *a = (const struct iwl_alive_resp *)pkt->data;
    /* version 4, 5 (+ the SKU id) or 6 and 7 (+ an IMR block): the start is the same */
    if(paylen != sizeof(*a) && paylen != sizeof(*a) + 12 && paylen != sizeof(*a) + 28) {
        kprintf("+IWL: an ALIVE of %u bytes, a layout SurfOS does not know\n", paylen);
        sc->alive_status = 0;
        sc->alive_seen = true;
        return;
    }
    sc->alive_status = a->status;
    if(a->lmac_data[0].error_event_table_ptr) sc->lmac_error_table = a->lmac_data[0].error_event_table_ptr;
    if(a->umac_data.error_info_addr) sc->umac_error_table = a->umac_data.error_info_addr;
    kprintf("+IWL: ALIVE, status 0x%04x: LMAC %u.%08x, UMAC %u.%u, error logs at 0x%08x and 0x%08x\n", a->status,
            a->lmac_data[0].ucode_major, a->lmac_data[0].ucode_minor, a->umac_data.umac_major, a->umac_data.umac_minor,
            sc->lmac_error_table, sc->umac_error_table);
    sc->alive_seen = true;
}

/* one packet: a response to the command at its index, or a notification */
static void rx_packet(struct iwl_softc *sc, const struct iwl_rx_packet *pkt, u_int len) {
    u_int paylen = len - sizeof(*pkt), code = pkt->hdr.code, group = pkt->hdr.flags & ~IWL_CMD_FAILED_MSK;
    sc->rx_packets++;
    if(!(pkt->hdr.qid & 0x80)) {                /* a response: queue 0 is the command queue */
        if(pkt->hdr.qid != 0 || pkt->hdr.idx != sc->cmd_idx || sc->cmd_done) return;
        sc->cmd_failed = (pkt->hdr.flags & IWL_CMD_FAILED_MSK) != 0;
        sc->cmd_resp_len = paylen;
        if(sc->cmd_resp) memcpy(sc->cmd_resp, pkt->data, paylen < sc->cmd_resp_size ? paylen : sc->cmd_resp_size);
        sc->cmd_done = true;
        return;
    }
    if(group > IWL_LONG_GROUP) {                /* the wide groups' notifications */
        if(group == IWL_PHY_OPS_GROUP && code == IWL_CT_KILL_NOTIFICATION)
            kprintf("+IWL: the card is at its critical temperature (%u C)\n", paylen >= 2 ? pkt->data[0] | (pkt->data[1] << 8) : 0);
        else sc->rx_notifs_other++;
        return;
    }
    switch(code) {                              /* the legacy group's (some come in the long group too) */
    case IWL_ALIVE:
        handle_alive(sc, pkt, paylen);
        break;
    case IWL_INIT_COMPLETE_NOTIF:
        sc->init_complete = true;
        break;
    case IWL_REPLY_ERROR:
        if(paylen >= 5)
            kprintf("+IWL: the firmware refused command 0x%02x (error 0x%x)\n", pkt->data[4],
                    pkt->data[0] | (pkt->data[1] << 8) | (pkt->data[2] << 16) | ((u32)pkt->data[3] << 24));
        break;
    case IWL_REPLY_RX_MPDU_CMD:
        iwl_mac_rx_mpdu(sc, pkt, paylen);
        break;
    case IWL_SCAN_COMPLETE_UMAC:
        iwl_mac_scan_done(sc, paylen >= sizeof(struct iwl_umac_scan_complete) ?
                          ((const struct iwl_umac_scan_complete *)pkt->data)->status : 1);
        break;
    case IWL_SCAN_ITERATION_COMPLETE_UMAC:
        iwl_mac_scan_done(sc, 1);
        break;
    case IWL_MCC_CHUB_UPDATE_CMD:
        if(paylen >= 2) kprintf("+IWL: the firmware reports the country \"%c%c\"\n", pkt->data[1], pkt->data[0]);
        break;
    default:
        sc->rx_notifs_other++;
        break;
    }
}

/* the packets in one receive buffer, each 64-byte aligned (if_iwx.c iwx_rx_pkt(), iwx_rx_pkt_valid());
   what was read is zeroed before the buffer goes back, so that a buffer filled with fewer packets
   next time never shows an old one after its last */
static void rx_buffer(struct iwl_softc *sc, u8 *buf) {
    u_int off = 0, len;
    while(off + sizeof(struct iwl_rx_packet) <= IWL_RBUF_SIZE) {
        const struct iwl_rx_packet *pkt = (const struct iwl_rx_packet *)(buf + off);
        if(((pkt->hdr.qid & 0x7f) == 0 && pkt->hdr.idx == 0 && pkt->hdr.code == 0 && pkt->hdr.flags == 0) ||
           pkt->len_n_flags == IWL_RX_FRAME_INVALID)
            break;
        len = sizeof(pkt->len_n_flags) + (pkt->len_n_flags & IWL_RX_FRAME_SIZE_MSK);
        if(len < sizeof(*pkt) || len > IWL_RBUF_SIZE - off) break;
        rx_packet(sc, pkt, len);
        off += (len + IWL_RX_FRAME_ALIGN - 1) & ~(IWL_RX_FRAME_ALIGN - 1);
    }
    memset(buf, 0, off < IWL_RBUF_SIZE ? off + sizeof(struct iwl_rx_packet) : IWL_RBUF_SIZE);
}

/* the card's interrupt causes and its receive queue (if_iwx.c iwx_intr() and iwx_notif_intr()) */
void iwl_poll(struct iwl_softc *sc) {
    u32 r1, r2;
    u_int hw;
    bool any = false;
    if(!sc->trans_ready) return;
    mutex_lock(&sc->rx_lock);
    if(sc->fw_state == IWL_FW_DOWN) {
        mutex_unlock(&sc->rx_lock);
        return;
    }
    r1 = iwl_rd(sc, IWL_CSR_INT);
    if(r1 == 0xffffffff || (r1 & 0xfffffff0) == 0xa5a5a5a0) {   /* gone, or asleep */
        mutex_unlock(&sc->rx_lock);
        return;
    }
    r2 = iwl_rd(sc, IWL_CSR_FH_INT_STATUS);
    if(r1) iwl_wr(sc, IWL_CSR_INT, r1);
    if(r2) iwl_wr(sc, IWL_CSR_FH_INT_STATUS, r2);
    if(r1 & IWL_INT_BIT_ALIVE) {                /* the card has set up its receive DMA: give it the buffers */
        post_rx_buffers(sc);
        iwl_wr(sc, IWL_RFH_Q0_FRBDCB_WIDX_TRG, 8);
    }
    if(r1 & IWL_INT_BIT_RF_KILL) iwl_check_rfkill(sc);
    if((r1 & (IWL_INT_BIT_SW_ERR | IWL_INT_BIT_HW_ERR)) && sc->fw_state != IWL_FW_ERROR) {
        kprintf("+IWL: %s (CSR_INT 0x%08x)\n", (r1 & IWL_INT_BIT_SW_ERR) ? "the firmware stopped with an error" :
                "a hardware error", r1);
        sc->fw_state = IWL_FW_ERROR;
        sc->cmd_failed = true;
        sc->cmd_done = true;
        mutex_unlock(&sc->rx_lock);
        iwl_dump_error(sc);
        return;
    }
    hw = (sc->rxq.stat->closed_rb_num & 0xfff) & (IWL_RX_RING - 1);
    while(sc->rxq.cur != hw) {
        rx_buffer(sc, sc->rxq.buf + sc->rxq.cur * IWL_RBUF_SIZE);
        sc->rxq.cur = (sc->rxq.cur + 1) % IWL_RX_RING;
        any = true;
    }
    if(any) iwl_wr(sc, IWL_RFH_Q0_FRBDCB_WIDX_TRG, ((hw == 0) ? IWL_RX_RING - 1 : hw - 1) & ~7u);
    mutex_unlock(&sc->rx_lock);
}

/**** commands ****/

int iwl_cmd(struct iwl_softc *sc, u32 id, const void *data, u_int len, void *resp, u_int resp_size,
            u_int *resp_len) {
    struct iwl_cmd_ring *cq = &sc->cmdq;
    struct iwl_device_cmd *cmd;
    struct iwl_tfh_tfd *tfd;
    void *raw_big = NULL;
    u_int idx, total = sizeof(struct iwl_cmd_header_wide) + len;
    u32 pa;
    u_long deadline;
    int r = 0;

    if(sc->fw_state != IWL_FW_ALIVE) return -ENODEV;
    if(!((id >> 8) & 0xff)) id |= IWL_LONG_GROUP << 8;     /* firmware API 50 on refuses group 0 (iwx_send_cmd()) */
    mutex_lock(&sc->cmd_lock);
    idx = cq->cur;
    if(len > sizeof(cmd->data)) {               /* larger than a slot: a buffer of its own */
        if(total > IWL_RBUF_SIZE) {
            mutex_unlock(&sc->cmd_lock);
            return -EINVAL;
        }
        cmd = (struct iwl_device_cmd *)dma_zalloc(total, 64, &raw_big);
        if(!cmd) {
            mutex_unlock(&sc->cmd_lock);
            return -ENOMEM;
        }
    } else {
        cmd = &cq->cmd[idx];
        memset(cmd, 0, sizeof(*cmd));
    }
    pa = phys(cmd);
    cmd->hdr.opcode = (u8)id;
    cmd->hdr.group_id = (u8)(id >> 8);
    cmd->hdr.idx = (u8)idx;
    cmd->hdr.qid = 0;
    cmd->hdr.length = (u16)len;
    cmd->hdr.version = 0;
    if(len) memcpy(cmd->data, data, len);
    tfd = &cq->desc[idx];
    memset(tfd, 0, sizeof(*tfd));
    tfd->tbs[0].tb_len = (u16)(total < IWL_FIRST_TB_SIZE ? total : IWL_FIRST_TB_SIZE);
    tfd->tbs[0].addr = pa;
    tfd->num_tbs = 1;
    if(total > IWL_FIRST_TB_SIZE) {
        tfd->tbs[1].tb_len = (u16)(total - IWL_FIRST_TB_SIZE);
        tfd->tbs[1].addr = pa + IWL_FIRST_TB_SIZE;
        tfd->num_tbs = 2;
    }
    mutex_lock(&sc->rx_lock);                   /* the response is matched under it */
    sc->cmd_idx = idx;
    sc->cmd_resp = (u8 *)resp;
    sc->cmd_resp_size = resp ? resp_size : 0;
    sc->cmd_resp_len = 0;
    sc->cmd_failed = false;
    sc->cmd_done = false;
    mutex_unlock(&sc->rx_lock);
    cq->cur = (idx + 1) % IWL_CMD_RING;
    cq->cur_hw = (cq->cur_hw + 1) % IWL_TFD_QUEUE_SIZE;
    iwl_wr(sc, IWL_HBUS_TARG_WRPTR, (0u << 16) | cq->cur_hw);

    deadline = getticks() + (IWL_CMD_TIMEOUT_MS * HZ) / 1000 + 1;
    while(!sc->cmd_done && (long)(getticks() - deadline) < 0) {
        iwl_poll(sc);
        if(!sc->cmd_done) sleep_ms(1);
    }
    mutex_lock(&sc->rx_lock);
    if(!sc->cmd_done) {
        kprintf("+IWL: no response to command 0x%04x in %u ms\n", id, IWL_CMD_TIMEOUT_MS);
        r = -ETIMEDOUT;
    } else if(sc->cmd_failed) {
        r = sc->fw_state == IWL_FW_ALIVE ? -EIO : -ENODEV;
    }
    if(resp_len) *resp_len = sc->cmd_resp_len;
    sc->cmd_resp = NULL;
    sc->cmd_done = true;                        /* a late response is dropped */
    mutex_unlock(&sc->rx_lock);
    dma_free(&raw_big);
    mutex_unlock(&sc->cmd_lock);
    return r;
}

int iwl_wait_init_complete(struct iwl_softc *sc, u_int ms) {
    u_long deadline = getticks() + (ms * HZ) / 1000 + 1;
    while(!sc->init_complete && sc->fw_state == IWL_FW_ALIVE && (long)(getticks() - deadline) < 0) {
        iwl_poll(sc);
        if(!sc->init_complete) sleep_ms(1);
    }
    if(sc->init_complete) return 0;
    return sc->fw_state == IWL_FW_ALIVE ? -ETIMEDOUT : -EIO;
}

/* the versions the firmware file gives its commands (IWL_FW_CMD_VER_UNKNOWN when it does not say) */
u_int iwl_cmd_version(struct iwl_softc *sc, u8 group, u8 cmd) {
    u_int i;
    for(i = 0; i < sc->n_cmd_versions; i++)
        if(sc->cmd_versions[i].group == group && sc->cmd_versions[i].cmd == cmd) return sc->cmd_versions[i].cmd_ver;
    return IWL_FW_CMD_VER_UNKNOWN;
}

u_int iwl_notif_version(struct iwl_softc *sc, u8 group, u8 cmd) {
    u_int i;
    for(i = 0; i < sc->n_cmd_versions; i++)
        if(sc->cmd_versions[i].group == group && sc->cmd_versions[i].cmd == cmd) return sc->cmd_versions[i].notif_ver;
    return IWL_FW_CMD_VER_UNKNOWN;
}
