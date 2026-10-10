/*
SurfOS Wi-Fi: the Intel Wireless-AC 9560, inside the driver
----------------------
File: iwl_priv.h  Date: 10/10/26
----------------------
What driver/wifi/iwl.c (the card: PCI, power, the station layer's operations) and
driver/wifi/iwl_trans.c (the firmware's transport: host memory the card reads and writes,
the firmware load, commands and what the card sends back) share. Nothing outside
driver/wifi/ includes it. The brief is docs/superpowers/plans/2026-10-09-wifi.md; the
notices of the code these follow are in iwl.c and include/sys/iwl_reg.h.
*/

#ifndef _SURFOS_IWL_PRIV_H
#define _SURFOS_IWL_PRIV_H

#include <surfos/types.h>
#include <surfos/sync.h>
#include <sys/pci.h>
#include <sys/iwl_reg.h>
#include <sys/iwl_fw.h>
#include <net/wlan.h>

#define IWL_RX_RING         64          /* receive buffers: a power of two, 8 to 2048 (the brief: few) */
#define IWL_RBUF_SIZE       4096
#define IWL_CMD_RING        256         /* the command queue's TFDs, as iwx has them */
#define IWL_TFD_QUEUE_SIZE  256         /* a write pointer counts modulo this (the 22000 family) */
#define IWL_CMD_TIMEOUT_MS  2000
#define IWL_ALIVE_TIMEOUT_MS 2000        /* iwx waits 1 s; the time costs nothing when it comes */

enum iwl_fw_state { IWL_FW_DOWN = 0, IWL_FW_LOADING, IWL_FW_ALIVE, IWL_FW_ERROR };

struct iwl_rx_ring {
    u64 *free;                          /* free buffers: address | index (the buffers are 4 KB aligned) */
    u32 *used;                          /* the card's used-buffer list (not read: buffers come in order) */
    volatile struct iwl_rb_status *stat;
    u8 *buf;                            /* IWL_RX_RING buffers of 4 KB, back to back */
    void *raw_free, *raw_used, *raw_stat, *raw_buf;
    u_int cur;                          /* the next buffer to read */
};

struct iwl_cmd_ring {
    struct iwl_tfh_tfd *desc;
    struct iwl_device_cmd *cmd;         /* one per TFD */
    void *raw_desc, *raw_cmd;
    u_int cur;                          /* the next TFD to fill */
    u_int cur_hw;                       /* the write pointer (modulo IWL_TFD_QUEUE_SIZE) */
};

struct iwl_softc {
    struct pci_dev *pci;
    volatile u8 *regs;
    u8 pcie_cap;                        /* the PCI Express capability's offset, 0 if none */
    u32 hw_rev, hw_rf_id;               /* as read (the RF id again at each start) */
    u_int mac_type, mac_step, rf_type;
    const char *fw_name;                /* the file for this MAC and radio, NULL if none */
    char fw_version[32];                /* of the file, once read and checked */
    int nic_locks;
    bool rfkill;                        /* the radio switch is off */
    u8 mac[WLAN_ADDR_LEN];
    char last[128];                     /* how far the last start got (wifi status) */
    struct wlan_dev *w;

    /* step 3: the firmware's transport (iwl_trans.c) */
    bool trans_ready;                   /* the rings and the context info are allocated */
    volatile int fw_state;              /* enum iwl_fw_state */
    struct iwl_context_info *ctxt;
    void *raw_ctxt;
    void *raw_img, *raw_img_umac;       /* the firmware's sections in host memory: the runtime image's
                                           two halves (freed at ALIVE) */
    void *raw_paging;                   /* and the paged ones (kept while it runs) */
    struct iwl_rx_ring rxq;
    struct iwl_cmd_ring cmdq;
    mutex_t rx_lock;                    /* one reader of the card's interrupt causes and receive queue */
    mutex_t cmd_lock;                   /* one command at a time */
    volatile bool cmd_done, cmd_failed;
    u_int cmd_idx;
    u8 *cmd_resp;                       /* where the response's payload goes, NULL: nowhere */
    u_int cmd_resp_size, cmd_resp_len;
    volatile bool alive_seen, init_complete, error_dumped;
    u16 alive_status;
    u32 lmac_error_table, umac_error_table;   /* from ALIVE (or the firmware file) */
    u32 fw_api[IWL_FW_API_WORDS], fw_capa[IWL_FW_API_WORDS];
    struct iwl_fw_cmd_version *cmd_versions;  /* copied from the file, which is freed */
    u_int n_cmd_versions;
    u32 phy_sku, n_scan_channels;
    /* what NVM_GET_INFO said */
    u16 nvm_version;
    u32 mac_sku_flags, tx_chains, rx_chains;
    bool lar_enabled;
    u_long rx_packets, rx_notifs_other;
};

/* registers */
static inline u32 iwl_rd(struct iwl_softc *sc, u_int off) { return *(volatile u32 *)(sc->regs + off); }
static inline void iwl_wr(struct iwl_softc *sc, u_int off, u32 v) { *(volatile u32 *)(sc->regs + off) = v; }
static inline void iwl_setbits(struct iwl_softc *sc, u_int off, u32 bits) { iwl_wr(sc, off, iwl_rd(sc, off) | bits); }
static inline void iwl_clrbits(struct iwl_softc *sc, u_int off, u32 bits) { iwl_wr(sc, off, iwl_rd(sc, off) & ~bits); }

/* iwl.c */
bool iwl_poll_bit(struct iwl_softc *sc, u_int reg, u32 bits, u32 mask, u_int timeout_us);
bool iwl_nic_lock(struct iwl_softc *sc);
void iwl_nic_unlock(struct iwl_softc *sc);
u32 iwl_prph_read(struct iwl_softc *sc, u32 addr);
void iwl_prph_write(struct iwl_softc *sc, u32 addr, u32 v);
int iwl_apm_init(struct iwl_softc *sc);
void iwl_disable_interrupts(struct iwl_softc *sc);
bool iwl_check_rfkill(struct iwl_softc *sc);

/* iwl_trans.c */
int iwl_trans_alloc(struct iwl_softc *sc);                      /* once: rings, context info */
int iwl_load_firmware(struct iwl_softc *sc, const struct iwl_fw *fw);   /* up to ALIVE */
void iwl_trans_stop(struct iwl_softc *sc);                      /* receive DMA off, rings reset, paging freed */
void iwl_poll(struct iwl_softc *sc);                            /* causes and received packets */
int iwl_cmd(struct iwl_softc *sc, u32 id, const void *data, u_int len, void *resp, u_int resp_size,
            u_int *resp_len);           /* waits for the response; its payload copied when resp */
int iwl_wait_init_complete(struct iwl_softc *sc, u_int ms);
void iwl_dump_error(struct iwl_softc *sc);                      /* the firmware's error logs, once */
u_int iwl_cmd_version(struct iwl_softc *sc, u8 group, u8 cmd);
u_int iwl_notif_version(struct iwl_softc *sc, u8 group, u8 cmd);

#endif
