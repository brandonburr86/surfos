/*
SurfOS Wi-Fi: the Intel Wireless-AC 9560's firmware: configuration, scans, a network
--------------------
File: iwl_mac.c  Date: 10/10/26 (the Wi-Fi brief: docs/superpowers/plans/2026-10-09-wifi.md, track I)
--------------------
What the driver asks of the running firmware (W2, step 4 of the brief's bring-up):
* the configuration iwx_init_hw() sends once the NVM is read: the transmit antennas, Bluetooth
  coexistence (Wi-Fi only), the platform's power stabilization (SOC_CONFIGURATION_CMD: this
  card's LTR delay and crystal latency, 1820 us each, as Linux's iwl_qu_medium_latency_mac_cfg
  has them for 8086:34f0), PCI Express LTR when the platform enabled it, the firmware's own
  thermal throttling, no power saving, the regulatory domain (MCC "ZZ": the NVM's own, which
  gives each channel's flags), the scan configuration, and beacon filtering off;
* scans: SCAN_REQ_UMAC (versions 14 and 15 share a layout) over the channels the regulatory
  domain allows, passive unless a network's name is asked for, ended by SCAN_COMPLETE_UMAC;
* received frames: each REPLY_RX_MPDU_CMD's descriptor checked (CRC, overrun), the frame handed
  to the station layer with its channel, band and signal (wlan_rx(), which copies it);
* W3 (step 5, 10/10/26): a network joined as iwx_auth() and iwx_run() do: the PHY context on
  its channel, the MAC context, the binding, the access point as station 0, a transmit queue
  for it (SCD_QUEUE_CONFIG_CMD version 3) and session protection; once associated, 802.11n as
  the access point answered the station layer's request (2 streams; 40 MHz at 5 GHz: the PHY
  context widened, the station, the MAC context and the firmware's rate control told), smart
  FIFO, multicast, no power saving. Frames go out on the queue (iwl_trans.c): management ones
  at the lowest basic rate, data at the firmware's; CCMP keys go to the firmware (ADD_STA_KEY),
  which encrypts what the station layer marks protect and decrypts what comes in; a decrypted
  frame goes up without its CCMP header once its PN is newer than the last.

Follows OpenBSD's sys/dev/pci/if_iwx.c (iwx_init_hw(), iwx_send_soc_conf(),
iwx_send_update_mcc_cmd(), iwx_config_umac_scan_reduced(), iwx_umac_scan_v14(),
iwx_fill_probe_req(), iwx_rx_mpdu_mq(), iwx_endscan(), iwx_auth(), iwx_run(), iwx_deauth(),
iwx_run_stop(), iwx_phy_ctxt_cmd_uhb_v3_v4(), iwx_phy_send_rlc(), iwx_mac_ctxt_cmd(),
iwx_binding_cmd(), iwx_add_sta_cmd(), iwx_enable_txq(), iwx_rs_init_v4(), iwx_add_sta_key_cmd(),
iwx_rx_hwdecrypt(), iwx_ccmp_decap()); the notices are in iwl.c. The SOC command's delay field
is encoded as Linux's iwl_set_soc_latency() does (the value in bits 3-2), and a decrypted
frame's MIC is taken as stripped, as Linux's iwlmvm marks it (RX_FLAG_MIC_STRIPPED).
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <mm/kalloc.h>
#include <fs/vfs.h>
#include <blibc_common.h>
#include "iwl_priv.h"

/* the NVM's channel list (if_iwx.c iwx_nvm_channels_8000): 2.4 GHz, then 5 GHz */
static const u8 nvm_channels[IWL_NUM_NVM_CHANNELS] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92,
    96, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165, 169, 173, 177, 181
};

static bool has_api(struct iwl_softc *sc, u_int bit) { return (sc->fw_api[bit / 32] >> (bit % 32)) & 1; }
static bool has_capa(struct iwl_softc *sc, u_int bit) { return (sc->fw_capa[bit / 32] >> (bit % 32)) & 1; }

/* the antennas the firmware and the NVM both allow (if_iwx.c iwx_fw_valid_tx_ant(), _rx_ant()) */
static u32 valid_tx_ant(struct iwl_softc *sc) {
    u32 a = IWL_FW_PHY_CFG_TX_CHAIN(sc->phy_sku);
    if(sc->tx_chains) a = a ? (a & sc->tx_chains) : sc->tx_chains;
    return a;
}

static u32 valid_rx_ant(struct iwl_softc *sc) {
    u32 a = IWL_FW_PHY_CFG_RX_CHAIN(sc->phy_sku);
    if(sc->rx_chains) a = a ? (a & sc->rx_chains) : sc->rx_chains;
    return a;
}

/* a command, its failure reported with its name */
static int cmd(struct iwl_softc *sc, u32 id, const void *data, u_int len, const char *name) {
    int r = iwl_cmd(sc, id, data, len, NULL, 0, NULL);
    if(r) kprintf("+IWL: %s: %s\n", name, strerror(r));
    return r;
}

/* how many of the card's channels may be used, and how many only listened to */
static void count_channels(struct iwl_softc *sc, u_int *n24, u_int *n5, u_int *passive) {
    u_int i;
    *n24 = *n5 = *passive = 0;
    for(i = 0; i < IWL_NUM_NVM_CHANNELS; i++) {
        if(!(sc->chan_flags[i] & IWL_NVM_CHANNEL_VALID)) continue;
        if(i < IWL_NUM_2GHZ_CHANNELS) (*n24)++;
        else (*n5)++;
        if(!(sc->chan_flags[i] & IWL_NVM_CHANNEL_ACTIVE)) (*passive)++;
    }
}

/* the regulatory domain: "ZZ", the NVM's own; the response gives every channel's flags (if_iwx.c
   iwx_send_update_mcc_cmd()) */
static int update_mcc(struct iwl_softc *sc) {
    struct iwl_mcc_update_cmd mc;
    const struct iwl_mcc_update_resp *rsp;
    u_int len = 0, i, n24, n5, passive;
    u8 *buf;
    int r;

    memset(&mc, 0, sizeof(mc));
    mc.mcc = ('Z' << 8) | 'Z';
    mc.source_id = (has_api(sc, IWL_API_WIFI_MCC_UPDATE) || has_capa(sc, IWL_CAPA_LAR_MULTI_MCC)) ?
                   IWL_MCC_SOURCE_GET_CURRENT : IWL_MCC_SOURCE_OLD_FW;
    buf = (u8 *)kalloc(IWL_RBUF_SIZE);
    if(!buf) return -ENOMEM;
    r = iwl_cmd(sc, IWL_WIDE_ID(IWL_LEGACY_GROUP, IWL_MCC_UPDATE_CMD), &mc, sizeof(mc), buf, IWL_RBUF_SIZE, &len);
    if(r) {
        kprintf("+IWL: MCC_UPDATE_CMD: %s\n", strerror(r));
        kfree(buf);
        return r;
    }
    rsp = (const struct iwl_mcc_update_resp *)buf;
    if(len < sizeof(*rsp) || rsp->n_channels > (IWL_RBUF_SIZE - sizeof(*rsp)) / 4 ||
       len != sizeof(*rsp) + rsp->n_channels * 4) {
        kprintf("+IWL: MCC_UPDATE_CMD: a response of %u bytes SurfOS cannot read\n", len);
        kfree(buf);
        return -EIO;
    }
    for(i = 0; i < IWL_NUM_NVM_CHANNELS && i < rsp->n_channels; i++) sc->chan_flags[i] = rsp->channels[i];
    if(!(sc->mac_sku_flags & IWL_NVM_MAC_SKU_BAND_5_2))
        for(i = IWL_NUM_2GHZ_CHANNELS; i < IWL_NUM_NVM_CHANNELS; i++) sc->chan_flags[i] &= ~IWL_NVM_CHANNEL_VALID;
    sc->mcc[0] = (char)(rsp->mcc >> 8);
    sc->mcc[1] = (char)rsp->mcc;
    sc->mcc[2] = 0;
    count_channels(sc, &n24, &n5, &passive);
    kprintf("+IWL: regulatory domain \"%s\" (status %u, source %u): %u channels at 2.4 GHz, %u at 5 GHz, %u of them "
            "only listened to\n", sc->mcc, rsp->status, rsp->source_id, n24, n5, passive);
    kfree(buf);
    return 0;
}

/* everything iwx_init_hw() sends after the init flow, in its order */
int iwl_mac_config(struct iwl_softc *sc) {
    struct iwl_tx_ant_cfg_cmd ant;
    struct iwl_bt_coex_cmd bt;
    struct iwl_soc_configuration_cmd soc;
    struct iwl_ltr_config_cmd ltr;
    struct iwl_temp_report_ths_cmd temp;
    struct iwl_device_power_cmd power;
    struct iwl_beacon_filter_cmd bf;
    struct iwl_scan_config scfg;
    int r;

    ant.valid = valid_tx_ant(sc);
    r = cmd(sc, IWL_WIDE_ID(IWL_LEGACY_GROUP, IWL_TX_ANT_CONFIGURATION_CMD), &ant, sizeof(ant), "TX_ANT_CONFIGURATION_CMD");
    if(r) return r;
    bt.mode = IWL_BT_COEX_WIFI;
    bt.enabled_modules = 0;
    r = cmd(sc, IWL_WIDE_ID(IWL_LEGACY_GROUP, IWL_BT_CONFIG), &bt, sizeof(bt), "BT_CONFIG");
    if(r) return r;
    soc.flags = IWL_SOC_FLAGS_LTR_APPLY_DELAY_1820 << IWL_SOC_FLAGS_LTR_APPLY_DELAY_SHIFT;    /* integrated */
    soc.latency = IWL_SOC_XTAL_LATENCY_34F0;
    r = cmd(sc, IWL_WIDE_ID(IWL_SYSTEM_GROUP, IWL_SOC_CONFIGURATION_CMD), &soc, sizeof(soc), "SOC_CONFIGURATION_CMD");
    if(r) return r;
    if(sc->ltr_enabled) {                       /* iwx_config_ltr(): the platform enabled LTR on the link */
        memset(&ltr, 0, sizeof(ltr));
        ltr.flags = IWL_LTR_CFG_FLAG_FEATURE_ENABLE;
        r = cmd(sc, IWL_WIDE_ID(IWL_LEGACY_GROUP, IWL_LTR_CONFIG), &ltr, sizeof(ltr), "LTR_CONFIG");
        if(r) kprintf("+IWL: (LTR left as it was)\n");  /* iwx carries on, too */
    }
    if(has_capa(sc, IWL_CAPA_CT_KILL_BY_FW)) {  /* an empty threshold list: the firmware throttles by itself */
        memset(&temp, 0, sizeof(temp));
        r = cmd(sc, IWL_WIDE_ID(IWL_PHY_OPS_GROUP, IWL_TEMP_REPORTING_THRESHOLDS_CMD), &temp, sizeof(temp),
                "TEMP_REPORTING_THRESHOLDS_CMD");
        if(r) return r;
    }
    memset(&power, 0, sizeof(power));           /* no power saving */
    r = cmd(sc, IWL_WIDE_ID(IWL_LEGACY_GROUP, IWL_POWER_TABLE_CMD), &power, sizeof(power), "POWER_TABLE_CMD");
    if(r) return r;
    if(sc->lar_enabled) {
        r = update_mcc(sc);
        if(r) return r;
    }
    if(!has_api(sc, IWL_API_REDUCED_SCAN_CONFIG)) {
        kprintf("+IWL: the firmware has no reduced scan configuration (API %u)\n", IWL_API_REDUCED_SCAN_CONFIG);
        return -ENOSYS;
    }
    memset(&scfg, 0, sizeof(scfg));
    if(iwl_cmd_version(sc, IWL_LONG_GROUP, IWL_SCAN_CFG_CMD) < 5 ||
       iwl_cmd_version(sc, IWL_LONG_GROUP, IWL_SCAN_CFG_CMD) == IWL_FW_CMD_VER_UNKNOWN)
        scfg.bcast_sta_id = 0xff;
    scfg.tx_chains = valid_tx_ant(sc);
    scfg.rx_chains = valid_rx_ant(sc);
    r = cmd(sc, IWL_WIDE_ID(IWL_LONG_GROUP, IWL_SCAN_CFG_CMD), &scfg, sizeof(scfg), "SCAN_CFG_CMD");
    if(r) return r;
    memset(&bf, 0, sizeof(bf));
    r = cmd(sc, IWL_WIDE_ID(IWL_LEGACY_GROUP, IWL_REPLY_BEACON_FILTERING_CMD), &bf, sizeof(bf), "BEACON_FILTERING_CMD");
    if(r) return r;
    kprintf("+IWL: configured: antennas tx 0x%x rx 0x%x, SOC latency 1820 us, LTR %s, %s\n", ant.valid, scfg.rx_chains,
            sc->ltr_enabled ? "on" : "off (not enabled on the link)",
            has_capa(sc, IWL_CAPA_CT_KILL_BY_FW) ? "thermal throttling by the firmware" : "no firmware thermal throttling");
    return 0;
}

/**** scanning ****/

/* the probe request the firmware sends on channels where probing is allowed: the header, an empty
   SSID (the firmware puts in the one asked for), the 2.4 GHz rates and the 5 GHz rates (if_iwx.c
   iwx_fill_probe_req(); no HT or VHT elements) */
static void fill_probe_req(struct iwl_softc *sc, struct iwl_scan_probe_req *preq) {
    static const u8 rates_2g[] = { 1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,  /* supported rates */
                                   50, 4, 0x30, 0x48, 0x60, 0x6c };                     /* extended */
    static const u8 rates_5g[] = { 1, 8, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c };
    u8 *f = preq->buf;
    u_int n = 0, start;

    memset(preq, 0, sizeof(*preq));
    f[n++] = 0x40;                              /* management, probe request */
    f[n++] = 0x00;
    n += 2;                                     /* duration: the hardware's */
    memset(f + n, 0xff, WLAN_ADDR_LEN);         /* to everyone */
    n += WLAN_ADDR_LEN;
    memcpy(f + n, sc->mac, WLAN_ADDR_LEN);
    n += WLAN_ADDR_LEN;
    memset(f + n, 0xff, WLAN_ADDR_LEN);
    n += WLAN_ADDR_LEN;
    n += 2;                                     /* sequence: the hardware's */
    f[n++] = 0;                                 /* SSID, empty */
    f[n++] = 0;
    preq->mac_header.offset = 0;
    preq->mac_header.len = (u16)n;

    start = n;
    memcpy(f + n, rates_2g, sizeof(rates_2g));
    n += sizeof(rates_2g);
    if(has_capa(sc, IWL_CAPA_DS_PARAM_SET_IE_SUPPORT)) {
        f[n++] = 3;                             /* DS parameter set: the firmware fills the channel */
        f[n++] = 1;
        f[n++] = 0;
    }
    preq->band_data[0].offset = (u16)start;
    preq->band_data[0].len = (u16)(n - start);
    if(sc->mac_sku_flags & IWL_NVM_MAC_SKU_BAND_5_2) {
        start = n;
        memcpy(f + n, rates_5g, sizeof(rates_5g));
        n += sizeof(rates_5g);
        preq->band_data[1].offset = (u16)start;
        preq->band_data[1].len = (u16)(n - start);
    }
    preq->common_data.offset = (u16)n;
    preq->common_data.len = 0;
}

/* SCAN_REQ_UMAC, version 14's layout (if_iwx.c iwx_umac_scan_v14()); the results come as received
   frames, the end as SCAN_COMPLETE_UMAC */
int iwl_mac_scan_begin(struct iwl_softc *sc, const struct wlan_scan_req *req) {
    struct iwl_scan_req_umac_v14 *c;
    struct iwl_scan_general_params_v10 *g;
    struct iwl_scan_channel_params_v6 *cp;
    u_int i, n = 0, max, ver;
    u32 ssid_bits = 0;
    int r;

    ver = iwl_cmd_version(sc, IWL_LONG_GROUP, IWL_SCAN_REQ_UMAC);
    if(ver != 14 && ver != 15) {
        kprintf("+IWL: the firmware wants SCAN_REQ_UMAC version %u; SurfOS has 14 and 15\n", ver);
        return -ENOSYS;
    }
    c = (struct iwl_scan_req_umac_v14 *)kcalloc(1, sizeof(*c));
    if(!c) return -ENOMEM;
    c->uid = 0;
    c->ooc_priority = IWL_SCAN_PRIORITY_EXT_6;

    g = &c->general_params;
    g->flags = IWL_UMAC_SCAN_GEN_FLAGS_V2_PASS_ALL | IWL_UMAC_SCAN_GEN_FLAGS_V2_NTFY_ITER_COMPLETE |
               IWL_UMAC_SCAN_GEN_FLAGS_V2_ADAPTIVE_DWELL;
    if(!req->ssid_len) g->flags |= IWL_UMAC_SCAN_GEN_FLAGS_V2_FORCE_PASSIVE;    /* as iwx: no name, no probes */
    g->adwell_default_social_chn = IWL_SCAN_ADWELL_DEFAULT_N_APS_SOCIAL;
    g->adwell_default_2g = IWL_SCAN_ADWELL_DEFAULT_LB_N_APS;
    g->adwell_default_5g = IWL_SCAN_ADWELL_DEFAULT_HB_N_APS;
    g->adwell_max_budget = IWL_SCAN_ADWELL_MAX_BUDGET_FULL_SCAN;
    g->scan_priority = IWL_SCAN_PRIORITY_EXT_6;
    for(i = 0; i < IWL_SCAN_TWO_LMACS; i++) {
        g->active_dwell[i] = IWL_SCAN_DWELL_ACTIVE;
        g->passive_dwell[i] = IWL_SCAN_DWELL_PASSIVE;
    }

    c->periodic_params.schedule[0].interval = 0;    /* once */
    c->periodic_params.schedule[0].iter_count = 1;

    fill_probe_req(sc, &c->probe_params.preq);
    if(req->ssid_len) {
        c->probe_params.direct_scan[0].id = 0;
        c->probe_params.direct_scan[0].len = req->ssid_len;
        memcpy(c->probe_params.direct_scan[0].ssid, req->ssid, req->ssid_len);
        ssid_bits = 1;
    }

    /* the channels: those asked for, or every one the regulatory domain allows */
    cp = &c->channel_params;
    cp->flags = IWL_SCAN_CHANNEL_FLAG_ENABLE_CHAN_ORDER;
    cp->n_aps_override[0] = IWL_SCAN_ADWELL_N_APS_GO_FRIENDLY;
    cp->n_aps_override[1] = IWL_SCAN_ADWELL_N_APS_SOCIAL_CHS;
    max = sc->n_scan_channels < IWL_SCAN_MAX_CHANNELS ? sc->n_scan_channels : IWL_SCAN_MAX_CHANNELS;
    for(i = 0; i < IWL_NUM_NVM_CHANNELS && n < max; i++) {
        u_int band = i < IWL_NUM_2GHZ_CHANNELS ? WLAN_BAND_2GHZ : WLAN_BAND_5GHZ;
        if(!(sc->chan_flags[i] & IWL_NVM_CHANNEL_VALID)) continue;
        if(req->nchannels) {
            u_int k;
            for(k = 0; k < req->nchannels; k++)
                if(req->channels[k] == nvm_channels[i] && req->bands[k] == band) break;
            if(k == req->nchannels) continue;
        }
        cp->channel_config[n].flags = ssid_bits;
        cp->channel_config[n].channel_num = nvm_channels[i];
        cp->channel_config[n].band = band == WLAN_BAND_5GHZ ? IWL_PHY_BAND_5 : IWL_PHY_BAND_24;
        cp->channel_config[n].iter_count = 1;
        cp->channel_config[n].iter_interval = 0;
        n++;
    }
    cp->count = (u8)n;
    if(!n) {
        kfree(c);
        return -EINVAL;
    }
    if(!has_api(sc, IWL_API_SCAN_EXT_CHAN_VER)) {
        /* version 1 of a channel's layout has no band: the number, the iteration count, a 16-bit interval */
        for(i = 0; i < n; i++) {
            cp->channel_config[i].band = 1;     /* where version 1 has the iteration count */
            cp->channel_config[i].iter_count = 0;
        }
    }
    sc->scan_uid = 0;
    sc->scanning = true;
    sc->scan_frames = 0;
    r = iwl_cmd(sc, IWL_WIDE_ID(IWL_LONG_GROUP, IWL_SCAN_REQ_UMAC), c, sizeof(*c), NULL, 0, NULL);
    kfree(c);
    if(r) {
        sc->scanning = false;
        kprintf("+IWL: SCAN_REQ_UMAC: %s\n", strerror(r));
        return r;
    }
    kprintf("+IWL: scanning %u channels%s%s\n", n, req->ssid_len ? ", probing for a network by name" : ", listening",
            ver == 15 ? " (request version 15)" : "");
    return 0;
}

/* SCAN_ABORT_UMAC: the firmware ends the scan with SCAN_COMPLETE_UMAC */
void iwl_mac_scan_abort(struct iwl_softc *sc) {
    struct iwl_umac_scan_abort a;
    if(!sc->scanning) return;
    a.uid = sc->scan_uid;
    a.flags = 0;
    if(iwl_cmd(sc, IWL_WIDE_ID(IWL_LONG_GROUP, IWL_SCAN_ABORT_UMAC), &a, sizeof(a), NULL, 0, NULL))
        kprintf("+IWL: SCAN_ABORT_UMAC refused\n");
}

/* a scan's end, once (if_iwx.c iwx_endscan()) */
void iwl_mac_scan_done(struct iwl_softc *sc, u_int status) {
    if(!sc->scanning) return;
    sc->scanning = false;
    kprintf("+IWL: scan %s: %lu frames heard\n", status == 2 ? "aborted" : "done", sc->scan_frames);
    wlan_scan_done(sc->w, status == 2 ? -EINTR : 0);   /* 1 completed, 2 aborted (iwx's SCAN_OFFLOAD_*) */
}

/**** received frames ****/

/* the 802.11 header's length (address 4, QoS, HT control) */
static u_int hdr_len(const u8 *f) {
    u_int type = (f[0] >> 2) & 3, n = 24;
    if(type == 1) return 10;                    /* control frames: never handed up */
    if((f[1] & 3) == 3) n += 6;                 /* to and from the distribution system */
    if(type == 2 && (f[0] & 0x80)) {            /* QoS data */
        n += 2;
        if(f[1] & 0x80) n += 4;                 /* +HTC */
    } else if(type == 0 && (f[1] & 0x80)) {
        n += 4;                                 /* a management frame's HT control */
    }
    return n;
}

/* the PN of a CCMP header (if_iwx.c iwx_ccmp_decap()) */
static u64 ccmp_pn(const u8 *iv) {
    return (u64)iv[0] | ((u64)iv[1] << 8) | ((u64)iv[4] << 16) | ((u64)iv[5] << 24) | ((u64)iv[6] << 32) |
           ((u64)iv[7] << 40);
}

/* REPLY_RX_MPDU_CMD (if_iwx.c iwx_rx_mpdu_mq(), iwx_rx_hwdecrypt(), iwx_ccmp_decap()): the descriptor, then the
   frame. A frame the firmware decrypted (CCMP, its MIC checked and stripped, as Linux's iwlmvm has it) goes up
   without its CCMP header and with the Protected bit clear, once its PN is newer than the last (a replay is
   dropped); the two bytes of padding after a header (and after a CCMP header) go; an A-MSDU's subframes come
   one by one, each with the header, its A-MSDU bit cleared. */
void iwl_mac_rx_mpdu(struct iwl_softc *sc, const struct iwl_rx_packet *pkt, u_int paylen) {
    const struct iwl_rx_mpdu_desc *d = (const struct iwl_rx_mpdu_desc *)pkt->data;
    const u8 *f;
    u8 *copy;
    struct wlan_rx_info info;
    u_int len, hl, sec, out, crypt = 0, pad = 0;
    int ea, eb;

    if(paylen < sizeof(*d)) return;
    if(!(d->status & IWL_RX_MPDU_RES_STATUS_CRC_OK) || !(d->status & IWL_RX_MPDU_RES_STATUS_OVERRUN_OK)) {
        sc->rx_bad++;
        return;
    }
    len = d->mpdu_len;
    if(len < 24 || len > paylen - sizeof(*d)) {
        sc->rx_bad++;
        return;
    }
    f = pkt->data + sizeof(*d);
    if(((f[0] >> 2) & 3) == 1) return;          /* control frames: nothing for the station layer */
    memset(&info, 0, sizeof(info));
    info.channel = d->channel;
    info.band = d->channel > 14 ? WLAN_BAND_5GHZ : WLAN_BAND_2GHZ;
    ea = d->energy_a ? -(int)d->energy_a : -127;     /* iwx's -256 for "nothing" does not fit an i8 */
    eb = d->energy_b ? -(int)d->energy_b : -127;
    info.rssi = (i8)(ea > eb ? ea : eb);
    hl = hdr_len(f);
    if(hl > len) {
        sc->rx_bad++;
        return;
    }
    sec = d->status & IWL_RX_MPDU_RES_STATUS_SEC_ENC_MSK;
    if(f[1] & 0x40) {                           /* protected */
        if(sec != IWL_RX_MPDU_RES_STATUS_SEC_CCM_ENC ||
           (d->status & (IWL_RX_MPDU_RES_STATUS_DEC_DONE | IWL_RX_MPDU_RES_STATUS_MIC_OK)) !=
           (IWL_RX_MPDU_RES_STATUS_DEC_DONE | IWL_RX_MPDU_RES_STATUS_MIC_OK)) {
            sc->rx_undecryptable++;             /* no key yet, or not ours */
            return;
        }
        crypt = 8;
        info.decrypted = true;
    } else if(sec != IWL_RX_MPDU_RES_STATUS_SEC_NO_ENC) {
        sc->rx_bad++;
        return;
    }
    if(d->mac_flags2 & IWL_RX_MPDU_MFLG2_PAD) pad = 2;
    if(hl + pad + crypt > len) {
        sc->rx_bad++;
        return;
    }
    if(crypt) {
        const u8 *iv = f + hl;                  /* the padding, if any, comes after the IV (iwx_rx_mpdu_mq()) */
        u64 pn;
        bool group = f[4] & 1, same_ok = (d->mac_flags2 & IWL_RX_MPDU_MFLG2_AMSDU) && (d->amsdu_info & 0x7f);
        u_int tid = 16, kid = (iv[3] >> 6) & 3;
        u64 *last;
        if(!(iv[3] & 0x20)) {                   /* the ExtIV bit: a CCMP header has it */
            sc->rx_bad++;
            return;
        }
        if(((f[0] >> 2) & 3) == 2 && (f[0] & 0x80)) tid = f[hl - 2 - ((f[1] & 0x80) ? 4 : 0)] & 0xf;
        pn = ccmp_pn(iv);
        last = group ? &sc->rx_group_pn[kid] : &sc->rx_pn[tid];
        if(same_ok ? pn < *last : pn <= *last) {
            sc->rx_replays++;
            return;
        }
        *last = pn;
    }
    if(sc->scanning) sc->scan_frames++;
    if(((f[0] >> 2) & 3) == 2) {                /* data: to us, or to a group */
        if(f[4] & 1) sc->rx_data_group++;
        else sc->rx_data_ucast++;
    }
    out = len - pad - crypt;
    copy = (u8 *)kalloc(out);
    if(!copy) return;
    memcpy(copy, f, hl);
    memcpy(copy + hl, f + hl + pad + crypt, len - hl - pad - crypt);
    if(crypt) copy[1] &= (u8)~0x40;             /* decrypted: the Protected bit off, as the station layer reads it */
    if((d->mac_flags2 & IWL_RX_MPDU_MFLG2_AMSDU) && ((copy[0] >> 2) & 3) == 2 && (copy[0] & 0x80))
        copy[hl - 2 - ((copy[1] & 0x80) ? 4 : 0)] &= (u8)~0x80;   /* one subframe, delivered as a plain frame */
    wlan_rx(sc->w, copy, out, &info);
    kfree(copy);
}

/**** joining a network (W3) ****/

/* the 802.11 rates the firmware's rate tables index: 1 2 5.5 11 Mbps (CCK), 6 9 12 18 24 36 48 54 (OFDM),
   in 500 kb/s (if_iwx.c iwx_rs_rval2idx() against ieee80211_std_rateset_11g) */
static const u8 rate_vals[12] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };

static int rate_index(u8 rval) {
    u_int i;
    for(i = 0; i < 12; i++) if(rate_vals[i] == rval) return (int)i;
    return -1;
}

/* rate_n_flags (version 2) for a legacy rate index: antenna A, CCK or OFDM, the rate (if_iwx.c
   iwx_tx_fill_cmd()) */
static u32 legacy_rate(int idx) {
    if(idx < 4) return IWL_RATE_MCS_ANT_A_MSK | IWL_RATE_MCS_CCK_MSK | ((u32)idx & IWL_RATE_LEGACY_RATE_MSK);
    return IWL_RATE_MCS_ANT_A_MSK | IWL_RATE_MCS_LEGACY_OFDM_MSK | ((u32)(idx - 4) & IWL_RATE_LEGACY_RATE_MSK);
}

/* the access point's rates from a list of rates (supported and extended elements' bytes); the lowest basic
   one, for management frames */
static void take_rates(struct iwl_softc *sc, const u8 *rates, u_int n, bool five) {
    int idx, low = -1;
    u_int i;
    for(i = 0; i < n; i++) {
        idx = rate_index(rates[i] & 0x7f);
        if(idx < 0 || (five && idx < 4)) continue;   /* no CCK at 5 GHz */
        sc->ap_rates |= 1u << idx;
        if(rates[i] & 0x80) {
            sc->ap_basic |= 1u << idx;
            if(low < 0 || idx < low) low = idx;
        }
    }
    if(low < 0) low = five ? 4 : 0;             /* none marked basic: 6 Mbps, or 1 Mbps */
    sc->mgmt_rate = legacy_rate(low);
}

/* the rates the card may answer at (ACK, CTS): the basic rates and every mandatory one below them, as
   802.11-2007 9.6 asks (if_iwx.c iwx_ack_rates()) */
static void ack_rates(struct iwl_softc *sc, u32 *cck, u32 *ofdm) {
    u32 b = sc->ap_basic, o = (b >> 4) & 0xff;
    int i, lowest_ofdm = -1;
    *cck = 0;
    if(sc->bss.band != WLAN_BAND_5GHZ)
        for(i = 3; i >= 0; i--)
            if(b & (1u << i)) {                 /* the highest basic CCK rate and every one below it */
                *cck = (1u << (i + 1)) - 1;
                break;
            }
    for(i = 0; i < 8; i++)
        if(o & (1u << i)) {
            lowest_ofdm = i;
            break;
        }
    if(lowest_ofdm < 0 || 4 < lowest_ofdm) o |= 1u << 4;    /* 24 Mbps */
    if(lowest_ofdm < 0 || 2 < lowest_ofdm) o |= 1u << 2;    /* 12 Mbps */
    o |= 1u << 0;                                           /* 6 Mbps, always */
    *ofdm = o;
}

u_int iwl_hdr_len(const u8 *f) { return hdr_len(f); }

/* a command whose response is a status word: ADD_STA's and ADD_STA_KEY's success is 1 (in the low byte),
   the binding's 0 (iwx_send_cmd_pdu_status()) */
static int cmd_status(struct iwl_softc *sc, u32 id, const void *data, u_int len, const char *name, bool sta_style) {
    struct iwl_cmd_response resp;
    u_int rlen = 0;
    int r;
    resp.status = 0;
    r = iwl_cmd(sc, id, data, len, &resp, sizeof(resp), &rlen);
    if(!r && rlen != sizeof(resp)) r = -EIO;
    if(!r && (sta_style ? (resp.status & IWL_ADD_STA_STATUS_MASK) != IWL_ADD_STA_SUCCESS : resp.status != 0)) r = -EIO;
    if(r) kprintf("+IWL: %s: %s (status 0x%x)\n", name, strerror(r), resp.status);
    return r;
}

/* the PHY context on the network's channel, 20 MHz, one chain idle and one active (if_iwx.c iwx_auth(),
   iwx_phy_ctxt_cmd_uhb_v3_v4(); with RLC_CONFIG_CMD version 2 the chains go there, iwx_phy_send_rlc()) */
static int phy_ctxt(struct iwl_softc *sc, u32 action) {
    u_int rlc = iwl_cmd_version(sc, IWL_DATA_PATH_GROUP, IWL_RLC_CONFIG_CMD);
    u32 chains = (valid_rx_ant(sc) << IWL_PHY_RX_CHAIN_VALID_POS) | ((u32)sc->phy_chains << IWL_PHY_RX_CHAIN_CNT_POS) |
                 ((u32)sc->phy_chains << IWL_PHY_RX_CHAIN_MIMO_CNT_POS);
    u8 band = sc->bss.band == WLAN_BAND_5GHZ ? IWL_PHY_BAND_5 : IWL_PHY_BAND_24;
    int r;
    if(has_capa(sc, IWL_CAPA_ULTRA_HB_CHANNELS)) {
        struct iwl_phy_context_cmd_uhb c;
        memset(&c, 0, sizeof(c));
        c.id_and_color = IWL_FW_CMD_ID_AND_COLOR(0, 0);
        c.action = action;
        c.channel = sc->bss.channel;
        c.band = band;
        c.width = sc->phy_width;
        c.ctrl_pos = sc->phy_ctrl_pos;
        c.lmac_id = IWL_LMAC_24G_INDEX;         /* (no CDB support: one LMAC) */
        if(rlc != 2) c.rxchain_info = chains;
        r = cmd(sc, IWL_WIDE_ID(IWL_LEGACY_GROUP, IWL_PHY_CONTEXT_CMD), &c, sizeof(c), "PHY_CONTEXT_CMD");
    } else {
        struct iwl_phy_context_cmd c;
        memset(&c, 0, sizeof(c));
        c.id_and_color = IWL_FW_CMD_ID_AND_COLOR(0, 0);
        c.action = action;
        c.channel = sc->bss.channel;
        c.band = band;
        c.width = sc->phy_width;
        c.ctrl_pos = sc->phy_ctrl_pos;
        c.lmac_id = IWL_LMAC_24G_INDEX;
        if(rlc != 2) c.rxchain_info = chains;
        r = cmd(sc, IWL_WIDE_ID(IWL_LEGACY_GROUP, IWL_PHY_CONTEXT_CMD), &c, sizeof(c), "PHY_CONTEXT_CMD");
    }
    if(!r && rlc == 2 && action != IWL_FW_CTXT_ACTION_REMOVE) {
        struct iwl_rlc_config_cmd rc;
        memset(&rc, 0, sizeof(rc));
        rc.phy_id = 0;
        rc.rx_chain_info = chains;
        r = cmd(sc, IWL_WIDE_ID(IWL_DATA_PATH_GROUP, IWL_RLC_CONFIG_CMD), &rc, sizeof(rc), "RLC_CONFIG_CMD");
    }
    return r;
}

/* the MAC context: a station on the access point, its ACK rates, EDCA (802.11's defaults until the access
   point's own come: W4), beacons let through until associated (if_iwx.c iwx_mac_ctxt_cmd()) */
static int mac_ctxt(struct iwl_softc *sc, u32 action, bool assoc) {
    static const struct { u8 fifo, aifsn, ecwmin, ecwmax; u16 txop; } edca[IWL_AC_NUM] = {
        { IWL_TX_FIFO_BE, 3, 4, 10, 0 }, { IWL_TX_FIFO_BK, 7, 4, 10, 0 },
        { IWL_TX_FIFO_VI, 2, 3, 4, 94 }, { IWL_TX_FIFO_VO, 2, 2, 3, 47 },
    };
    struct iwl_mac_ctx_cmd c;
    u_int i;
    memset(&c, 0, sizeof(c));
    c.id_and_color = IWL_FW_CMD_ID_AND_COLOR(0, 0);
    c.action = action;
    if(action != IWL_FW_CTXT_ACTION_REMOVE) {
        c.mac_type = IWL_FW_MAC_TYPE_BSS_STA;
        c.tsf_id = IWL_TSF_ID_A;
        memcpy(c.node_addr, sc->mac, 6);
        memcpy(c.bssid_addr, sc->bss.bssid, 6);
        {
            u32 cck, ofdm;
            ack_rates(sc, &cck, &ofdm);
            c.cck_rates = cck;
            c.ofdm_rates = ofdm;
        }
        c.cck_short_preamble = (sc->bss.capability & 0x0020) ? IWL_MAC_FLG_SHORT_PREAMBLE : 0;
        c.short_slot = (sc->bss.band == WLAN_BAND_5GHZ || (sc->bss.capability & 0x0400)) ? IWL_MAC_FLG_SHORT_SLOT : 0;
        for(i = 0; i < IWL_AC_NUM; i++) {
            struct iwl_ac_qos *q = &c.ac[edca[i].fifo];
            q->cw_min = (u16)((1u << edca[i].ecwmin) - 1);
            q->cw_max = (u16)((1u << edca[i].ecwmax) - 1);
            q->aifsn = edca[i].aifsn;
            q->fifos_mask = (u8)(1u << edca[i].fifo);
            q->edca_txop = edca[i].txop;
        }
        c.qos_flags = IWL_MAC_QOS_FLG_UPDATE_EDCA;
        if(sc->ht) {
            c.qos_flags |= IWL_MAC_QOS_FLG_TGN;
            c.protection_flags = sc->ht_prot;
        }
        c.filter_flags = IWL_MAC_FILTER_ACCEPT_GRP;
        if(!assoc) c.filter_flags |= IWL_MAC_FILTER_IN_BEACON;
        c.sta.is_assoc = assoc;
        c.sta.bi = sc->bss.beacon_interval ? sc->bss.beacon_interval : 100;
        c.sta.dtim_interval = c.sta.bi;         /* the DTIM period is not known: 1 */
        c.sta.listen_interval = 10;
        c.sta.assoc_id = assoc ? (sc->aid & 0x3fff) : 0;
    }
    return cmd(sc, IWL_WIDE_ID(IWL_LEGACY_GROUP, IWL_MAC_CONTEXT_CMD), &c, sizeof(c), "MAC_CONTEXT_CMD");
}

static int binding(struct iwl_softc *sc, u32 action) {
    struct iwl_binding_cmd c;
    u_int i;
    memset(&c, 0, sizeof(c));
    c.id_and_color = IWL_FW_CMD_ID_AND_COLOR(0, 0);         /* the PHY context's */
    c.action = action;
    c.phy = IWL_FW_CMD_ID_AND_COLOR(0, 0);
    c.macs[0] = IWL_FW_CMD_ID_AND_COLOR(0, 0);
    for(i = 1; i < IWL_MAX_MACS_IN_BINDING; i++) c.macs[i] = IWL_FW_CTXT_INVALID;
    c.lmac_id = IWL_LMAC_24G_INDEX;
    return cmd_status(sc, IWL_WIDE_ID(IWL_LONG_GROUP, IWL_BINDING_CONTEXT_CMD), &c, sizeof(c), "BINDING_CONTEXT_CMD",
                      false);
}

/* the access point as the firmware's station 0, a legacy (non-HT) one for now (if_iwx.c iwx_add_sta_cmd()) */
static int add_sta(struct iwl_softc *sc, bool update) {
    struct iwl_add_sta_cmd c;
    memset(&c, 0, sizeof(c));
    c.sta_id = IWL_STATION_ID;
    c.station_type = IWL_STA_LINK;
    c.mac_id_n_color = IWL_FW_CMD_ID_AND_COLOR(0, 0);
    if(!update) memcpy(c.addr, sc->bss.bssid, 6);
    c.add_modify = update ? 1 : 0;
    c.station_flags_msk = IWL_STA_FLG_FAT_EN_MSK | IWL_STA_FLG_MIMO_EN_MSK;   /* (20 MHz, one stream, unless HT) */
    if(update && sc->ht) {
        c.station_flags_msk |= IWL_STA_FLG_MAX_AGG_SIZE_MSK | IWL_STA_FLG_AGG_MPDU_DENS_MSK;
        if(sc->mimo) c.station_flags |= IWL_STA_FLG_MIMO_EN_MIMO2;
        if(sc->ht40) c.station_flags |= IWL_STA_FLG_FAT_EN_40MHZ;
        c.station_flags |= ((u32)(sc->ap_ampdu & 3) << IWL_STA_FLG_MAX_AGG_SIZE_SHIFT) & IWL_STA_FLG_MAX_AGG_SIZE_MSK;
        if(((sc->ap_ampdu >> 2) & 7) >= 4)   /* 2, 4, 8 or 16 us apart: as iwx_add_sta_cmd() maps them */
            c.station_flags |= ((u32)((sc->ap_ampdu >> 2) & 7) << IWL_STA_FLG_AGG_MPDU_DENS_SHIFT);
    }
    return cmd_status(sc, IWL_WIDE_ID(IWL_LONG_GROUP, IWL_ADD_STA), &c, sizeof(c), "ADD_STA", true);
}

/* the transmit queue for the station's management and non-QoS data (TID 15), as iwx_enable_mgmt_queue()
   with SCD_QUEUE_CONFIG_CMD version 3; the firmware's response names the queue */
static int enable_queue(struct iwl_softc *sc) {
    struct iwl_scd_queue_cfg_cmd c;
    struct iwl_tx_queue_cfg_rsp rsp;
    u_int len = 0, ver = iwl_cmd_version(sc, IWL_DATA_PATH_GROUP, IWL_SCD_QUEUE_CONFIG_CMD);
    int r;
    if(ver != 3) {
        kprintf("+IWL: SCD_QUEUE_CONFIG_CMD version %u: SurfOS has version 3\n", ver);
        return -ENOSYS;
    }
    r = iwl_txq_alloc(sc);
    if(r) return r;
    iwl_txq_reset(sc);
    memset(&c, 0, sizeof(c));
    c.operation = IWL_SCD_QUEUE_ADD;
    c.u.add.sta_mask = 1u << IWL_STATION_ID;
    c.u.add.tid = IWL_MGMT_TID;
    c.u.add.flags = 0;
    c.u.add.cb_size = 8 - 3;                    /* log2(256) - 3 */
    c.u.add.bc_dram_addr = (u32)(u_long)sc->txq.bc;
    c.u.add.tfdq_dram_addr = (u32)(u_long)sc->txq.desc;
    memset(&rsp, 0, sizeof(rsp));
    r = iwl_cmd(sc, IWL_WIDE_ID(IWL_DATA_PATH_GROUP, IWL_SCD_QUEUE_CONFIG_CMD), &c, sizeof(c), &rsp, sizeof(rsp), &len);
    if(!r && len != sizeof(rsp)) r = -EIO;
    if(r) {
        kprintf("+IWL: SCD_QUEUE_CONFIG_CMD: %s\n", strerror(r));
        return r;
    }
    if(rsp.queue_number == 0 || rsp.queue_number > 31 || rsp.write_pointer != 0) {
        kprintf("+IWL: the firmware gave queue %u at %u: not usable\n", rsp.queue_number, rsp.write_pointer);
        return -EIO;
    }
    mutex_lock(&sc->tx_lock);
    sc->txq.qid = rsp.queue_number;
    sc->txq.enabled = true;
    mutex_unlock(&sc->tx_lock);
    return 0;
}

static void disable_queue(struct iwl_softc *sc) {
    struct iwl_scd_queue_cfg_cmd c;
    if(!sc->txq.enabled) return;
    mutex_lock(&sc->tx_lock);
    sc->txq.enabled = false;                    /* no frame after this */
    mutex_unlock(&sc->tx_lock);
    memset(&c, 0, sizeof(c));
    c.operation = IWL_SCD_QUEUE_REMOVE;
    c.u.remove.sta_mask = 1u << IWL_STATION_ID;
    c.u.remove.tid = IWL_MGMT_TID;
    cmd(sc, IWL_WIDE_ID(IWL_DATA_PATH_GROUP, IWL_SCD_QUEUE_CONFIG_CMD), &c, sizeof(c), "SCD_QUEUE_CONFIG_CMD (remove)");
    iwl_txq_reset(sc);
}

/* session protection: the firmware stays on the channel through authentication and association */
static int protect_session(struct iwl_softc *sc, bool on) {
    struct iwl_session_prot_cmd c;
    memset(&c, 0, sizeof(c));
    c.id_and_color = IWL_FW_CMD_ID_AND_COLOR(0, 0);
    c.action = on ? IWL_FW_CTXT_ACTION_ADD : IWL_FW_CTXT_ACTION_REMOVE;
    c.conf_id = IWL_SESSION_PROTECT_CONF_ASSOC;
    c.duration_tu = on ? (sc->bss.beacon_interval ? sc->bss.beacon_interval * 9u : 900u) : 0;
    return cmd(sc, IWL_WIDE_ID(IWL_MAC_CONF_GROUP, IWL_SESSION_PROTECTION_CMD), &c, sizeof(c), "SESSION_PROTECTION_CMD");
}

/* everything join() set up, undone in reverse (if_iwx.c iwx_deauth()) */
static void unjoin(struct iwl_softc *sc) {
    struct iwl_rm_sta_cmd rm;
    if(sc->session_active) {
        protect_session(sc, false);
        sc->session_active = false;
    }
    if(sc->sta_active) {
        struct iwl_tx_path_flush_cmd fl;
        memset(&fl, 0, sizeof(fl));
        fl.sta_id = IWL_STATION_ID;
        fl.tid_mask = 0xffff;
        cmd(sc, IWL_WIDE_ID(IWL_LONG_GROUP, IWL_TXPATH_FLUSH), &fl, sizeof(fl), "TXPATH_FLUSH");
        disable_queue(sc);
        memset(&rm, 0, sizeof(rm));
        rm.sta_id = IWL_STATION_ID;
        cmd(sc, IWL_WIDE_ID(IWL_LONG_GROUP, IWL_REMOVE_STA), &rm, sizeof(rm), "REMOVE_STA");
        sc->sta_active = false;
    }
    if(sc->binding_active) {
        binding(sc, IWL_FW_CTXT_ACTION_REMOVE);
        sc->binding_active = false;
    }
    if(sc->mac_active) {
        mac_ctxt(sc, IWL_FW_CTXT_ACTION_REMOVE, false);
        sc->mac_active = false;
    }
    if(sc->phy_active) {
        phy_ctxt(sc, IWL_FW_CTXT_ACTION_REMOVE);
        sc->phy_active = false;
    }
    sc->have_ptk = sc->have_gtk = false;
}

/* the station layer's join(): tuned to the network, the access point a station, a queue to it, and the
   channel held for the handshake (if_iwx.c iwx_auth()) */
int iwl_mac_join(struct iwl_softc *sc, const struct wlan_bss *bss) {
    int r;
    if(sc->phy_active) unjoin(sc);              /* another network's, still there */
    sc->bss = *bss;
    sc->aid = 0;
    sc->ap_rates = sc->ap_basic = 0;
    sc->ht = sc->ht40 = sc->mimo = false;
    sc->ht_prot = 0;
    sc->phy_width = IWL_PHY_VHT_CHANNEL_MODE20;
    sc->phy_ctrl_pos = IWL_PHY_VHT_CTRL_POS_1_BELOW;
    sc->phy_chains = 1;
    take_rates(sc, bss->rates, bss->nrates, bss->band == WLAN_BAND_5GHZ);
    memset(sc->rx_pn, 0, sizeof(sc->rx_pn));
    memset(sc->rx_group_pn, 0, sizeof(sc->rx_group_pn));
    r = phy_ctxt(sc, IWL_FW_CTXT_ACTION_ADD);
    if(r) goto fail;
    sc->phy_active = true;
    r = mac_ctxt(sc, IWL_FW_CTXT_ACTION_ADD, false);
    if(r) goto fail;
    sc->mac_active = true;
    r = binding(sc, IWL_FW_CTXT_ACTION_ADD);
    if(r) goto fail;
    sc->binding_active = true;
    r = add_sta(sc, false);
    if(r) goto fail;
    sc->sta_active = true;
    r = enable_queue(sc);
    if(r) goto fail;
    r = protect_session(sc, true);
    if(r) goto fail;
    sc->session_active = true;
    kprintf("+IWL: joined %02x:%02x:%02x:%02x:%02x:%02x on channel %u (%s GHz): transmit queue %u, rates 0x%03x\n",
            bss->bssid[0], bss->bssid[1], bss->bssid[2], bss->bssid[3], bss->bssid[4], bss->bssid[5], bss->channel,
            bss->band == WLAN_BAND_5GHZ ? "5" : "2.4", sc->txq.qid, sc->ap_rates);
    return 0;
fail:
    unjoin(sc);
    return r;
}

/* a management frame (authentication, association, deauthentication): the lowest basic rate, never
   encrypted */
int iwl_mac_tx_mgmt(struct iwl_softc *sc, const u8 *frame, u_int len) {
    return iwl_tx_frame(sc, frame, len, IWL_TX_FLAGS_CMD_RATE | IWL_TX_FLAGS_ENCRYPT_DIS, sc->mgmt_rate);
}

/* the station layer's associated(): the association response's rates, the station and the MAC context
   associated, smart FIFO, multicast, power and the firmware's rate control (if_iwx.c iwx_run()) */
int iwl_mac_associated(struct iwl_softc *sc, u16 aid, const u8 *ies, u_int ies_len) {
    struct iwl_sf_cfg_cmd sf;
    struct iwl_mcast_filter_cmd mf;
    struct iwl_mac_power_cmd pm;
    struct iwl_tlc_config_cmd_v4 tlc;
    u_int i;
    int r;

    sc->aid = aid;
    {                                           /* supported (1) and extended supported (50) rates */
        u8 rates[32];
        u_int n = 0;
        for(i = 0; i + 2 <= ies_len && i + 2 + ies[i + 1] <= ies_len; i += 2 + ies[i + 1])
            if((ies[i] == 1 || ies[i] == 50) && n + ies[i + 1] <= sizeof(rates)) {
                memcpy(rates + n, ies + i + 2, ies[i + 1]);
                n += ies[i + 1];
            }
        if(n) {
            sc->ap_rates = sc->ap_basic = 0;
            take_rates(sc, rates, n, sc->bss.band == WLAN_BAND_5GHZ);
        }
    }
    /* 802.11n: the station layer's association request offered HT (2 streams; 20/40 MHz at 5 GHz) when the
       access point has it, so the card follows what the access point answered (if_iwx.c iwx_run()) */
    {
        const u8 *cap = NULL, *op = NULL;
        for(i = 0; i + 2 <= ies_len && i + 2 + ies[i + 1] <= ies_len; i += 2 + ies[i + 1]) {
            if(ies[i] == 45 && ies[i + 1] >= 26) cap = ies + i + 2;
            if(ies[i] == 61 && ies[i + 1] >= 22) op = ies + i + 2;
        }
        if(!cap && sc->bss.ht) cap = sc->bss.ht_cap;
        if(cap) {
            u16 capinfo = (u16)(cap[0] | (cap[1] << 8));
            int ci = -1;
            u_int k;
            for(k = IWL_NUM_2GHZ_CHANNELS; k < IWL_NUM_NVM_CHANNELS; k++) if(nvm_channels[k] == sc->bss.channel) ci = (int)k;
            sc->ht = true;
            sc->ap_ampdu = cap[2];
            sc->ap_mcs[0] = cap[3];
            sc->ap_mcs[1] = cap[4];
            sc->ap_capinfo = capinfo;
            sc->mimo = !(sc->mac_sku_flags & IWL_NVM_MAC_SKU_MIMO_DISABLED) && cap[4] != 0 && valid_rx_ant(sc) == 3;
            sc->ht40 = sc->bss.band == WLAN_BAND_5GHZ && op && ((op[1] & 3) == 1 || (op[1] & 3) == 3) && (op[1] & 4) &&
                       (capinfo & 0x0002) && ci >= 0 && (sc->chan_flags[ci] & IWL_NVM_CHANNEL_40MHZ);
            sc->phy_chains = sc->mimo ? 2 : 1;
            sc->phy_width = sc->ht40 ? IWL_PHY_VHT_CHANNEL_MODE40 : IWL_PHY_VHT_CHANNEL_MODE20;
            sc->phy_ctrl_pos = (sc->ht40 && (op[1] & 3) == 3) ? IWL_PHY_VHT_CTRL_POS_1_ABOVE : IWL_PHY_VHT_CTRL_POS_1_BELOW;
            sc->ht_prot = 0;
            if(op && ((op[2] & 3) == 1 || (op[2] & 3) == 3 || ((op[2] & 3) == 2 && sc->ht40)))
                sc->ht_prot = IWL_MAC_PROT_FLG_HT_PROT | IWL_MAC_PROT_FLG_FAT_PROT;
            r = phy_ctxt(sc, IWL_FW_CTXT_ACTION_MODIFY);
            if(r) return r;
        }
    }
    r = add_sta(sc, true);
    if(r) return r;
    r = mac_ctxt(sc, IWL_FW_CTXT_ACTION_MODIFY, true);
    if(r) return r;
    if(!has_api(sc, IWL_API_SMART_FIFO_OFFLOAD)) {
        memset(&sf, 0, sizeof(sf));
        sf.state = IWL_SF_FULL_ON;
        sf.watermark[0] = IWL_SF_W_MARK_SCAN;
        sf.watermark[1] = sc->ht ? (sc->mimo ? IWL_SF_W_MARK_MIMO2 : IWL_SF_W_MARK_SISO) : IWL_SF_W_MARK_LEGACY;
        for(i = 0; i < IWL_SF_NUM_SCENARIO; i++) {
            static const u32 full_on[IWL_SF_NUM_SCENARIO][2] = {
                { 2016, 320 }, { 2016, 320 }, { 10016, 2016 }, { 2016, 320 }, { 2016, 320 } };
            sf.long_delay_timeouts[i][0] = sf.long_delay_timeouts[i][1] = IWL_SF_LONG_DELAY_AGING_TIMER;
            sf.full_on_timeouts[i][0] = full_on[i][0];
            sf.full_on_timeouts[i][1] = full_on[i][1];
        }
        r = cmd(sc, IWL_WIDE_ID(IWL_LEGACY_GROUP, IWL_REPLY_SF_CFG_CMD), &sf, sizeof(sf), "REPLY_SF_CFG_CMD");
        if(r) return r;
    }
    memset(&mf, 0, sizeof(mf));                 /* every multicast from our access point (iwx_allow_mcast()) */
    mf.filter_own = 1;
    mf.pass_all = 1;
    memcpy(mf.bssid, sc->bss.bssid, 6);
    r = cmd(sc, IWL_WIDE_ID(IWL_LEGACY_GROUP, IWL_MCAST_FILTER_CMD), &mf, sizeof(mf), "MCAST_FILTER_CMD");
    if(r) return r;
    memset(&pm, 0, sizeof(pm));                 /* no power saving (iwx_set_pslevel(level 0)) */
    pm.id_and_color = IWL_FW_CMD_ID_AND_COLOR(0, 0);
    pm.keep_alive_seconds = IWL_POWER_KEEP_ALIVE_PERIOD_SEC;
    r = cmd(sc, IWL_WIDE_ID(IWL_LEGACY_GROUP, IWL_MAC_PM_POWER_TABLE), &pm, sizeof(pm), "MAC_PM_POWER_TABLE");
    if(r) return r;
    memset(&tlc, 0, sizeof(tlc));               /* the firmware's rate control over the legacy rates (iwx_rs_init_v4()) */
    tlc.sta_id = IWL_STATION_ID;
    tlc.max_ch_width = IWL_TLC_MNG_CH_WIDTH_20MHZ;
    tlc.mode = IWL_TLC_MNG_MODE_NON_HT;
    tlc.chains = IWL_TLC_MNG_CHAIN_A_MSK;
    tlc.non_ht_rates = (u16)(sc->ap_rates ? sc->ap_rates : 0xff0);
    tlc.max_mpdu_len = 2312;
    if(sc->ht) {                                /* MCS 0-7 on one stream, 8-15 on two (iwx_rs_ht_rates()) */
        tlc.mode = IWL_TLC_MNG_MODE_HT;
        tlc.ht_rates[0][0] = sc->ap_mcs[0];
        if(sc->mimo) {
            tlc.ht_rates[1][0] = sc->ap_mcs[1];
            tlc.chains = IWL_TLC_MNG_CHAIN_A_MSK | IWL_TLC_MNG_CHAIN_B_MSK;
        }
        tlc.max_ch_width = sc->ht40 ? IWL_TLC_MNG_CH_WIDTH_40MHZ : IWL_TLC_MNG_CH_WIDTH_20MHZ;
        if(sc->ap_capinfo & 0x0020) tlc.sgi_ch_width_supp |= 1u << IWL_TLC_MNG_CH_WIDTH_20MHZ;
        if(sc->ht40 && (sc->ap_capinfo & 0x0040)) tlc.sgi_ch_width_supp |= 1u << IWL_TLC_MNG_CH_WIDTH_40MHZ;
        if(valid_tx_ant(sc) == 3 && (sc->ap_capinfo & 0x0300)) tlc.flags |= IWL_TLC_MNG_CFG_FLAGS_STBC_MSK;
        tlc.max_mpdu_len = 3839;
    }
    if(iwl_cmd_version(sc, IWL_DATA_PATH_GROUP, IWL_TLC_MNG_CONFIG_CMD) != 4)
        kprintf("+IWL: TLC_MNG_CONFIG_CMD version %u, sent as version 4\n",
                iwl_cmd_version(sc, IWL_DATA_PATH_GROUP, IWL_TLC_MNG_CONFIG_CMD));
    r = cmd(sc, IWL_WIDE_ID(IWL_DATA_PATH_GROUP, IWL_TLC_MNG_CONFIG_CMD), &tlc, sizeof(tlc), "TLC_MNG_CONFIG_CMD");
    if(r) return r;
    if(sc->session_active) {                    /* associated: the channel need not be held any more */
        protect_session(sc, false);
        sc->session_active = false;
    }
    kprintf("+IWL: associated, AID %u, rates 0x%03x, management at 0x%x; %s\n", aid & 0x3fff, sc->ap_rates,
            sc->mgmt_rate, sc->ht ? (sc->ht40 ? (sc->mimo ? "802.11n, 40 MHz, 2 streams" : "802.11n, 40 MHz, 1 stream") :
            (sc->mimo ? "802.11n, 20 MHz, 2 streams" : "802.11n, 20 MHz, 1 stream")) : "legacy rates");
    return 0;
}

/* ADD_STA_KEY: a CCMP key, the pairwise one at offset 0, a group key at 1 (if_iwx.c iwx_add_sta_key_cmd()) */
int iwl_mac_set_key(struct iwl_softc *sc, const struct wlan_key *key) {
    struct iwl_add_sta_key_cmd c;
    int r;
    if(key->cipher != WLAN_CIPHER_CCMP || key->len != 16 || key->index > 3) return -EINVAL;
    memset(&c, 0, sizeof(c));
    c.sta_id = IWL_STATION_ID;
    c.key_flags = (u16)(IWL_STA_KEY_FLG_CCM | IWL_STA_KEY_FLG_WEP_KEY_MAP |
                        ((key->index << IWL_STA_KEY_FLG_KEYID_POS) & IWL_STA_KEY_FLG_KEYID_MSK));
    if(key->pairwise) {
        c.key_offset = 0;
    } else {
        c.key_offset = 1;
        c.key_flags |= IWL_STA_KEY_MULTICAST;
    }
    memcpy(c.key, key->key, key->len);
    r = cmd_status(sc, IWL_WIDE_ID(IWL_LONG_GROUP, IWL_ADD_STA_KEY), &c, sizeof(c), "ADD_STA_KEY", true);
    memset(&c, 0, sizeof(c));                   /* no copy of a key left on the stack */
    if(r) return r;
    if(key->pairwise) {
        sc->have_ptk = true;
        memset(sc->rx_pn, 0, sizeof(sc->rx_pn));
    } else {
        u_int i;
        u64 rsc = 0;
        for(i = 0; i < 6; i++) rsc |= (u64)key->rsc[i] << (8 * i);
        sc->have_gtk = true;
        sc->rx_group_pn[key->index] = rsc;      /* frames up to the RSC message 3 gave are old */
    }
    return 0;
}

/* a data frame: unicast at the firmware's rate, group at the lowest basic rate; encrypted by the firmware
   when protect (the Protected bit set: the firmware adds the CCMP header and MIC) */
int iwl_mac_tx_data(struct iwl_softc *sc, const u8 *frame, u_int len, bool protect) {
    u8 hdr[40];
    u_int hl;
    u32 flags = 0, rate = 0;
    bool group = frame[4] & 1;
    if(len < 24) return -EINVAL;
    hl = hdr_len(frame);
    if(hl > sizeof(hdr) || hl > len) return -EINVAL;
    if(group) {
        flags |= IWL_TX_FLAGS_CMD_RATE;
        rate = sc->mgmt_rate;
    }
    if(len >= hl + 8 && frame[hl + 6] == 0x88 && frame[hl + 7] == 0x8e) flags |= IWL_TX_FLAGS_HIGH_PRI;   /* EAPOL */
    if(!protect) return iwl_tx_frame(sc, frame, len, flags | IWL_TX_FLAGS_ENCRYPT_DIS, rate);
    {                                           /* the Protected bit on a copy of the header */
        u8 *copy = (u8 *)kalloc(len);
        int r;
        if(!copy) return -ENOMEM;
        memcpy(copy, frame, len);
        copy[1] |= 0x40;
        r = iwl_tx_frame(sc, copy, len, flags, rate);
        kfree(copy);
        return r;
    }
}

/* the station layer's leave(): out of the network (if_iwx.c iwx_run_stop() and iwx_deauth()) */
void iwl_mac_leave(struct iwl_softc *sc) {
    if(sc->mac_active && sc->aid) mac_ctxt(sc, IWL_FW_CTXT_ACTION_MODIFY, false);
    unjoin(sc);
    sc->aid = 0;
    kprintf("+IWL: left the network\n");
}
