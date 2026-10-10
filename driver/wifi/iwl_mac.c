/*
SurfOS Wi-Fi: the Intel Wireless-AC 9560's firmware, configured and scanning
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
  to the station layer with its channel, band and signal (wlan_rx(), which copies it).

Follows OpenBSD's sys/dev/pci/if_iwx.c (iwx_init_hw(), iwx_send_soc_conf(),
iwx_send_update_mcc_cmd(), iwx_config_umac_scan_reduced(), iwx_umac_scan_v14(),
iwx_fill_probe_req(), iwx_rx_mpdu_mq(), iwx_endscan()); the notices are in iwl.c. The SOC
command's delay field is encoded as Linux's iwl_set_soc_latency() does (the value in bits 3-2).
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

/* REPLY_RX_MPDU_CMD (if_iwx.c iwx_rx_mpdu_mq()): the descriptor, then the frame */
void iwl_mac_rx_mpdu(struct iwl_softc *sc, const struct iwl_rx_packet *pkt, u_int paylen) {
    const struct iwl_rx_mpdu_desc *d = (const struct iwl_rx_mpdu_desc *)pkt->data;
    const u8 *f;
    struct wlan_rx_info info;
    u_int len, hl;
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
    if((d->status & IWL_RX_MPDU_RES_STATUS_SEC_ENC_MSK) != IWL_RX_MPDU_RES_STATUS_SEC_NO_ENC) {
        /* protected: handed up only once W3 installs keys; the firmware's decryption status says how it went */
        sc->rx_protected++;
        return;
    }
    if(sc->scanning) sc->scan_frames++;
    if(d->mac_flags2 & IWL_RX_MPDU_MFLG2_PAD) {
        /* two bytes of padding after the header: the frame put back together in a buffer of its own */
        u8 *copy;
        hl = hdr_len(f);
        if(hl + 2 > len) {
            sc->rx_bad++;
            return;
        }
        copy = (u8 *)kalloc(len - 2);
        if(!copy) return;
        memcpy(copy, f, hl);
        memcpy(copy + hl, f + hl + 2, len - hl - 2);
        wlan_rx(sc->w, copy, len - 2, &info);
        kfree(copy);
        return;
    }
    wlan_rx(sc->w, f, len, &info);
}
