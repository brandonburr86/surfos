/*
SurfOS Wi-Fi: the Intel Wireless-AC 9560
--------------------
File: iwl.c  Date: 10/10/26 (the Wi-Fi brief: docs/superpowers/plans/2026-10-09-wifi.md, track I)
--------------------
The laptop's Wi-Fi card: the CNVi "Qu" MAC of the Ice Lake chipset (PCI 8086:34f0) with the
9560 "JF" radio module, driven the way OpenBSD's iwx(4) drives this family, with Intel's
firmware (API 77, include/sys/iwl_fw.h) and the registers of include/sys/iwl_reg.h.

What it does (W1, steps 2 and 3 of the brief's bring-up):
* At boot, with profile=laptop only (the driver table's "iwlwifi"): it finds the card, maps
  its 16 KB of registers and reads CSR_HW_REV and CSR_HW_RF_ID, which say which MAC (Qu or
  QuZ, its step) and which radio it is; those name the firmware file. It touches nothing
  else on the card, registers wlan0 with the station layer and starts its thread, `iwl`,
  which polls the card while the firmware runs.
* `wifi up` (start()): the power-up of iwx_start_hw() (the card made ready, its persistence
  bit cleared, a reset, power gating forced (an integrated card), the clock started and
  waited for, MSI chosen over MSI-X: SurfOS polls); CSR_HW_RF_ID read again, the radio
  switch read; the firmware file read from /initrd/lib/firmware/ and checked
  (iwl_fw_parse()); then, step 3 (10/10/26), the firmware loaded and started through the
  context info (iwl_trans.c) up to ALIVE, the init flow of iwx_run_init_mvm_ucode()
  (INIT_EXTENDED_CFG_CMD for the NVM, NVM_ACCESS_COMPLETE, INIT_COMPLETE), NVM_GET_INFO
  (the bands, 802.11n/ac, the antennas) and the MAC address from the card's registers:
  wlan0 is up. Scanning is the next step (W2): scan() and the rest still fail with ENOSYS.
* `wifi down` (stop()): the receive DMA and the card stopped (iwx_stop_device()).
`wifi status` and the `+IWL:` lines in dmesg say how far a start got; a firmware error is
reported with its error logs (iwx_nic_error()).

Polled, no interrupt: the card's interrupt line stays masked (INTx disabled, MSI and MSI-X
off). Every wait on the hardware is bounded.

The register sequences follow OpenBSD's sys/dev/pci/if_iwx.c, which carries these notices,
kept here as their licences ask:

 * Copyright (c) 2014, 2016 genua gmbh <info@genua.de>
 *   Author: Stefan Sperling <stsp@openbsd.org>
 * Copyright (c) 2014 Fixup Software Ltd.
 * Copyright (c) 2017, 2019, 2020 Stefan Sperling <stsp@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

 * Based on BSD-licensed source modules in the Linux iwlwifi driver,
 * which were used as the reference documentation for this implementation.
 *
 * Copyright(c) 2017 Intel Deutschland GmbH
 * Copyright(c) 2018 - 2019 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/timer.h>
#include <surfos/task.h>
#include <mm/kalloc.h>
#include <mm/paging.h>
#include <fs/vfs.h>
#include <blibc_common.h>
#include "iwl_priv.h"

#define FW_DIR      "/initrd/lib/firmware/"
#define FW_MAX      (4u << 20)                  /* a firmware file larger than this is not one of ours */
#define POLL_MS     10                          /* the thread's pace while the firmware runs */

/* the PCI ids this driver takes: the laptop's card only, for now */
static const u16 iwl_devices[] = { 0x34f0 };

/**** registers ****/

/* until (reg & mask) == (bits & mask), up to timeout_us (if_iwx.c iwx_poll_bit()) */
bool iwl_poll_bit(struct iwl_softc *sc, u_int reg, u32 bits, u32 mask, u_int timeout_us) {
    for(;;) {
        if((iwl_rd(sc, reg) & mask) == (bits & mask)) return true;
        if(timeout_us < 10) return false;
        timeout_us -= 10;
        udelay(10);
    }
}

/* keep the card's MAC awake for register and memory access through the HBUS window */
bool iwl_nic_lock(struct iwl_softc *sc) {
    if(sc->nic_locks > 0) {
        sc->nic_locks++;
        return true;
    }
    iwl_setbits(sc, IWL_CSR_GP_CNTRL, IWL_GP_CNTRL_MAC_ACCESS_REQ);
    udelay(2);
    if(iwl_poll_bit(sc, IWL_CSR_GP_CNTRL, IWL_GP_CNTRL_MAC_CLOCK_READY,
                IWL_GP_CNTRL_MAC_CLOCK_READY | IWL_GP_CNTRL_GOING_TO_SLEEP, 150000)) {
        sc->nic_locks++;
        return true;
    }
    kprintf("+IWL: the card did not wake for register access (GP_CNTRL 0x%08x)\n", iwl_rd(sc, IWL_CSR_GP_CNTRL));
    return false;
}

void iwl_nic_unlock(struct iwl_softc *sc) {
    if(sc->nic_locks > 0 && --sc->nic_locks == 0) iwl_clrbits(sc, IWL_CSR_GP_CNTRL, IWL_GP_CNTRL_MAC_ACCESS_REQ);
}

u32 iwl_prph_read(struct iwl_softc *sc, u32 addr) {
    iwl_wr(sc, IWL_HBUS_TARG_PRPH_RADDR, (addr & IWL_PRPH_ADDR_MASK) | (3u << 24));
    return iwl_rd(sc, IWL_HBUS_TARG_PRPH_RDAT);
}

void iwl_prph_write(struct iwl_softc *sc, u32 addr, u32 v) {
    iwl_wr(sc, IWL_HBUS_TARG_PRPH_WADDR, (addr & IWL_PRPH_ADDR_MASK) | (3u << 24));
    iwl_wr(sc, IWL_HBUS_TARG_PRPH_WDAT, v);
}

/* (prph & mask) | bits, with the NIC held awake (if_iwx.c iwx_set_bits_mask_prph()) */
static int prph_update(struct iwl_softc *sc, u32 addr, u32 bits, u32 mask) {
    if(!iwl_nic_lock(sc)) return -EBUSY;
    iwl_prph_write(sc, addr, (iwl_prph_read(sc, addr) & mask) | bits);
    iwl_nic_unlock(sc);
    return 0;
}

/**** power ****/

static bool set_hw_ready(struct iwl_softc *sc) {
    iwl_setbits(sc, IWL_CSR_HW_IF_CONFIG_REG, IWL_HW_IF_CONFIG_BIT_NIC_READY);
    if(!iwl_poll_bit(sc, IWL_CSR_HW_IF_CONFIG_REG, IWL_HW_IF_CONFIG_BIT_NIC_READY, IWL_HW_IF_CONFIG_BIT_NIC_READY, 50))
        return false;
    iwl_setbits(sc, IWL_CSR_MBOX_SET_REG, IWL_MBOX_SET_OS_ALIVE);
    return true;
}

/* the card ready for the driver: up to about 1.75 s (if_iwx.c iwx_prepare_card_hw()) */
static int prepare_card_hw(struct iwl_softc *sc) {
    u_int tries, t = 0;
    if(set_hw_ready(sc)) return 0;
    iwl_setbits(sc, IWL_CSR_DBG_LINK_PWR_MGMT_REG, IWL_RESET_LINK_PWR_MGMT_DISABLED);
    mdelay(1);
    for(tries = 0; tries < 10; tries++) {
        iwl_setbits(sc, IWL_CSR_HW_IF_CONFIG_REG, IWL_HW_IF_CONFIG_PREPARE);
        do {
            if(set_hw_ready(sc)) return 0;
            udelay(200);
            t += 200;
        } while(t < 150000);
        mdelay(25);
    }
    return -ETIMEDOUT;
}

static int clear_persistence_bit(struct iwl_softc *sc) {
    u32 hpm = iwl_prph_read(sc, IWL_HPM_DEBUG);
    if(hpm != 0xa5a5a5a0 && (hpm & IWL_PERSISTENCE_BIT)) {
        if(iwl_prph_read(sc, IWL_PRPH_WPROT_22000) & IWL_PRPH_WFPM_ACCESS) {
            kprintf("+IWL: the persistence bit cannot be cleared (write protected)\n");
            return -EACCES;
        }
        iwl_prph_write(sc, IWL_HPM_DEBUG, hpm & ~IWL_PERSISTENCE_BIT);
    }
    return 0;
}

static void sw_reset(struct iwl_softc *sc) {
    iwl_setbits(sc, IWL_CSR_RESET, IWL_RESET_SW_RESET);
    mdelay(5);
}

static int force_power_gating(struct iwl_softc *sc) {
    int r = prph_update(sc, IWL_HPM_HIPM_GEN_CFG, IWL_HPM_HIPM_GEN_CFG_CR_FORCE_ACTIVE, ~0u);
    if(r) return r;
    udelay(20);
    r = prph_update(sc, IWL_HPM_HIPM_GEN_CFG, IWL_HPM_HIPM_GEN_CFG_CR_PG_EN | IWL_HPM_HIPM_GEN_CFG_CR_SLP_EN, ~0u);
    if(r) return r;
    udelay(20);
    return prph_update(sc, IWL_HPM_HIPM_GEN_CFG, 0, ~IWL_HPM_HIPM_GEN_CFG_CR_FORCE_ACTIVE);
}

/* the card's basic functions after a reset; no firmware yet (if_iwx.c iwx_apm_init()) */
int iwl_apm_init(struct iwl_softc *sc) {
    iwl_setbits(sc, IWL_CSR_GIO_CHICKEN_BITS, IWL_GIO_CHICKEN_L1A_NO_L0S_RX);
    iwl_setbits(sc, IWL_CSR_DBG_HPET_MEM_REG, IWL_DBG_HPET_MEM_REG_VAL);
    iwl_setbits(sc, IWL_CSR_HW_IF_CONFIG_REG, IWL_HW_IF_CONFIG_BIT_HAP_WAKE_L1A);
    iwl_setbits(sc, IWL_CSR_GIO_REG, IWL_GIO_VAL_L0S_DISABLED);    /* iwx_apm_config(): L0s off, always */
    iwl_setbits(sc, IWL_CSR_GP_CNTRL, IWL_GP_CNTRL_INIT_DONE);
    if(!iwl_poll_bit(sc, IWL_CSR_GP_CNTRL, IWL_GP_CNTRL_MAC_CLOCK_READY, IWL_GP_CNTRL_MAC_CLOCK_READY, 25000)) {
        kprintf("+IWL: the card's clock did not stabilize (GP_CNTRL 0x%08x)\n", iwl_rd(sc, IWL_CSR_GP_CNTRL));
        return -ETIMEDOUT;
    }
    return 0;
}

static void apm_stop(struct iwl_softc *sc) {
    iwl_setbits(sc, IWL_CSR_DBG_LINK_PWR_MGMT_REG, IWL_RESET_LINK_PWR_MGMT_DISABLED);
    iwl_setbits(sc, IWL_CSR_HW_IF_CONFIG_REG, IWL_HW_IF_CONFIG_PREPARE | IWL_HW_IF_CONFIG_ENABLE_PME);
    mdelay(1);
    iwl_clrbits(sc, IWL_CSR_DBG_LINK_PWR_MGMT_REG, IWL_RESET_LINK_PWR_MGMT_DISABLED);
    mdelay(5);
    iwl_setbits(sc, IWL_CSR_RESET, IWL_RESET_STOP_MASTER);           /* its DMA stops */
    if(!iwl_poll_bit(sc, IWL_CSR_RESET, IWL_RESET_MASTER_DISABLED, IWL_RESET_MASTER_DISABLED, 100))
        kprintf("+IWL: the card's DMA did not stop\n");
    iwl_clrbits(sc, IWL_CSR_GP_CNTRL, IWL_GP_CNTRL_INIT_DONE);
}

void iwl_disable_interrupts(struct iwl_softc *sc) {
    iwl_wr(sc, IWL_CSR_INT_MASK, 0);
    iwl_wr(sc, IWL_CSR_INT, ~0u);                   /* every cause acknowledged */
    iwl_wr(sc, IWL_CSR_FH_INT_STATUS, ~0u);
}

/* the radio switch: GP_CNTRL bit 27 clear means it is off (if_iwx.c iwx_check_rfkill()) */
bool iwl_check_rfkill(struct iwl_softc *sc) {
    sc->rfkill = !(iwl_rd(sc, IWL_CSR_GP_CNTRL) & IWL_GP_CNTRL_HW_RF_KILL_SW);
    return sc->rfkill;
}

/* power up (if_iwx.c iwx_start_hw(), for the 22000 family's integrated cards, without MSI-X) */
static int start_hw(struct iwl_softc *sc) {
    int r = prepare_card_hw(sc);
    if(r) {
        kprintf("+IWL: the card did not become ready (HW_IF_CONFIG 0x%08x)\n", iwl_rd(sc, IWL_CSR_HW_IF_CONFIG_REG));
        return r;
    }
    r = clear_persistence_bit(sc);
    if(r) return r;
    sw_reset(sc);
    iwl_setbits(sc, IWL_CSR_GP_CNTRL, IWL_GP_CNTRL_INIT_DONE);
    udelay(20);
    if(!iwl_poll_bit(sc, IWL_CSR_GP_CNTRL, IWL_GP_CNTRL_MAC_CLOCK_READY, IWL_GP_CNTRL_MAC_CLOCK_READY, 25000)) {
        kprintf("+IWL: the card's clock did not stabilize after the reset (GP_CNTRL 0x%08x)\n", iwl_rd(sc, IWL_CSR_GP_CNTRL));
        return -ETIMEDOUT;
    }
    r = force_power_gating(sc);
    if(r) return r;
    sw_reset(sc);
    r = iwl_apm_init(sc);
    if(r) return r;
    if(iwl_nic_lock(sc)) {                          /* iwx_conf_msix_hw() without MSI-X: the causes in CSR_INT */
        iwl_prph_write(sc, IWL_UREG_CHICK, IWL_UREG_CHICK_MSI_ENABLE);
        iwl_nic_unlock(sc);
    }
    iwl_wr(sc, IWL_CSR_INT_MASK, IWL_INT_BIT_RF_KILL);   /* iwx_enable_rfkill_int(): only the cause is read */
    iwl_setbits(sc, IWL_CSR_GP_CNTRL, IWL_GP_CNTRL_RFKILL_WAKE_L1A_EN);
    iwl_check_rfkill(sc);
    return 0;
}

/* everything off and the card back in its low-power state (if_iwx.c iwx_stop_device()) */
static void stop_device(struct iwl_softc *sc) {
    iwl_disable_interrupts(sc);
    iwl_clrbits(sc, IWL_CSR_GP_CNTRL, IWL_GP_CNTRL_MAC_ACCESS_REQ);
    sc->nic_locks = 0;
    apm_stop(sc);
    sw_reset(sc);
    iwl_disable_interrupts(sc);                     /* stopping raises one when the radio switch is off */
    iwl_wr(sc, IWL_CSR_INT_MASK, IWL_INT_BIT_RF_KILL);
    iwl_setbits(sc, IWL_CSR_GP_CNTRL, IWL_GP_CNTRL_RFKILL_WAKE_L1A_EN);
    iwl_check_rfkill(sc);
    prepare_card_hw(sc);
}

/**** the MAC address ****/

static void flip_address(u32 a0, u32 a1, u8 *mac) {
    mac[0] = (u8)(a0 >> 24);
    mac[1] = (u8)(a0 >> 16);
    mac[2] = (u8)(a0 >> 8);
    mac[3] = (u8)a0;
    mac[4] = (u8)(a1 >> 8);
    mac[5] = (u8)a1;
}

static bool valid_address(const u8 *mac) {
    static const u8 reserved[WLAN_ADDR_LEN] = { 0x02, 0xcc, 0xaa, 0xff, 0xee, 0x00 };
    static const u8 zero[WLAN_ADDR_LEN];
    static const u8 bcast[WLAN_ADDR_LEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    return memcmp(mac, reserved, WLAN_ADDR_LEN) && memcmp(mac, zero, WLAN_ADDR_LEN) &&
           memcmp(mac, bcast, WLAN_ADDR_LEN) && !(mac[0] & 1);
}

/* the address the card came with: an OEM's in the strap registers when valid, else the OTP's
   (if_iwx.c iwx_set_mac_addr_from_csr(), which reads them once the firmware runs) */
static bool read_address(struct iwl_softc *sc, u8 *mac) {
    if(!iwl_nic_lock(sc)) return false;
    flip_address(iwl_rd(sc, IWL_CSR_MAC_ADDR0_STRAP), iwl_rd(sc, IWL_CSR_MAC_ADDR1_STRAP), mac);
    if(!valid_address(mac)) flip_address(iwl_rd(sc, IWL_CSR_MAC_ADDR0_OTP), iwl_rd(sc, IWL_CSR_MAC_ADDR1_OTP), mac);
    iwl_nic_unlock(sc);
    return valid_address(mac);
}

/**** which chip, which firmware ****/

static const char *mac_name(u_int type) {
    if(type == IWL_MAC_TYPE_QU) return "Qu";
    if(type == IWL_MAC_TYPE_QUZ) return "QuZ";
    return "unknown MAC";
}

static const char *rf_name(u_int type) {
    switch(type) {
    case IWL_RF_TYPE_JF1: return "JF1 (9461/9462)";
    case IWL_RF_TYPE_JF2: return "JF2 (9560)";
    case IWL_RF_TYPE_HR1: return "HR1 (AX101)";
    case IWL_RF_TYPE_HR2: return "HR2 (AX201)";
    case 0: return "not read yet";
    default: return "unknown radio";
    }
}

/* the file Linux's iwlwifi would load for this MAC with a JF radio; NULL for one we do not have */
static const char *choose_firmware(u_int mac_type, u_int mac_step, u_int rf_type) {
    if(rf_type != IWL_RF_TYPE_JF1 && rf_type != IWL_RF_TYPE_JF2) return NULL;
    if(mac_type == IWL_MAC_TYPE_QU && mac_step == IWL_SILICON_B_STEP) return "iwlwifi-Qu-b0-jf-b0-77.ucode";
    if(mac_type == IWL_MAC_TYPE_QU && mac_step == IWL_SILICON_C_STEP) return "iwlwifi-Qu-c0-jf-b0-77.ucode";
    if(mac_type == IWL_MAC_TYPE_QUZ) return "iwlwifi-QuZ-a0-jf-b0-77.ucode";
    return NULL;
}

static void identify(struct iwl_softc *sc) {
    sc->mac_type = IWL_HW_REV_TYPE(sc->hw_rev);
    sc->mac_step = IWL_HW_REV_STEP(sc->hw_rev);
    sc->rf_type = IWL_HW_RFID_TYPE(sc->hw_rf_id);
    sc->fw_name = choose_firmware(sc->mac_type, sc->mac_step, sc->rf_type);
}

/* the firmware file, read whole (the caller frees *file) and checked */
static int read_firmware(struct iwl_softc *sc, u8 **file, u_int *len, struct iwl_fw *fw) {
    char path[96];
    struct stat st;
    struct file *f;
    const char *why;
    u_int got = 0;
    int r;

    *file = NULL;
    snprintf(path, sizeof(path), FW_DIR "%s", sc->fw_name);
    r = vfs_stat(path, &st);
    if(r) {
        kprintf("+IWL: %s: %s (the image was built without the Wi-Fi firmware: tools/fetch-firmware.py)\n", path,
                strerror(r));
        return r;
    }
    if(!st.size || st.size > FW_MAX) {
        kprintf("+IWL: %s: %u bytes is not a firmware file's size\n", path, (u_int)st.size);
        return -EINVAL;
    }
    *file = (u8 *)kalloc(st.size);
    if(!*file) return -ENOMEM;
    r = vfs_open(path, O_RDONLY, &f);
    if(r) goto bad;
    while(got < st.size) {
        r = vfs_read(f, *file + got, st.size - got);
        if(r <= 0) break;
        got += r;
    }
    vfs_close(f);
    if(r < 0) goto bad;
    if(got != st.size) {
        r = -EIO;
        goto bad;
    }
    *len = got;
    r = iwl_fw_parse(*file, got, fw, &why);
    if(r) {
        kprintf("+IWL: %s: not a usable firmware file: %s\n", path, why);
        goto out;
    }
    kprintf("+IWL: %s: %u bytes, version %s, %u sections (%u + %u + %u paged), %u scan channels\n", path, got,
            fw->version, fw->lmac_count + fw->umac_count + fw->paging_count, fw->lmac_count, fw->umac_count,
            fw->paging_count, fw->n_scan_channels);
    return 0;
bad:
    kprintf("+IWL: %s: cannot be read: %s\n", path, strerror(r));
out:
    kfree(*file);
    *file = NULL;
    return r;
}

/**** the firmware's first commands ****/

/* what the firmware file says that is still needed once the file is freed */
static int keep_fw_info(struct iwl_softc *sc, const struct iwl_fw *fw) {
    memcpy(sc->fw_api, fw->api, sizeof(sc->fw_api));
    memcpy(sc->fw_capa, fw->capa, sizeof(sc->fw_capa));
    sc->phy_sku = fw->phy_sku;
    sc->n_scan_channels = fw->n_scan_channels;
    sc->lmac_error_table = fw->lmac_error_table;    /* ALIVE says them again */
    sc->umac_error_table = fw->umac_error_table;
    if(sc->cmd_versions) kfree(sc->cmd_versions);
    sc->cmd_versions = NULL;
    sc->n_cmd_versions = 0;
    if(fw->n_cmd_versions) {
        sc->cmd_versions = (struct iwl_fw_cmd_version *)kalloc(fw->n_cmd_versions * sizeof(struct iwl_fw_cmd_version));
        if(!sc->cmd_versions) return -ENOMEM;
        memcpy(sc->cmd_versions, fw->cmd_versions, fw->n_cmd_versions * sizeof(struct iwl_fw_cmd_version));
        sc->n_cmd_versions = fw->n_cmd_versions;
    }
    return 0;
}

static bool fw_has_api(struct iwl_softc *sc, u_int bit) { return (sc->fw_api[bit / 32] >> (bit % 32)) & 1; }
static bool fw_has_capa(struct iwl_softc *sc, u_int bit) { return (sc->fw_capa[bit / 32] >> (bit % 32)) & 1; }

/* the init flow of if_iwx.c iwx_run_init_mvm_ucode(sc, 1): the NVM commands announced and done, INIT_COMPLETE
   waited for; then NVM_GET_INFO (iwx_nvm_get()) */
static int init_firmware(struct iwl_softc *sc) {
    u32 init_flags = IWL_INIT_NVM, zero = 0;
    const struct iwl_nvm_get_info_head *h;
    u_int rsp_len = 0, want;
    u8 *rsp;
    int r;

    r = iwl_cmd(sc, IWL_WIDE_ID(IWL_SYSTEM_GROUP, IWL_INIT_EXTENDED_CFG_CMD), &init_flags, sizeof(init_flags), NULL, 0,
                NULL);
    if(r) {
        kprintf("+IWL: INIT_EXTENDED_CFG_CMD: %s\n", strerror(r));
        return r;
    }
    r = iwl_cmd(sc, IWL_WIDE_ID(IWL_REGULATORY_AND_NVM_GROUP, IWL_NVM_ACCESS_COMPLETE), &zero, sizeof(zero), NULL, 0,
                NULL);
    if(r) {
        kprintf("+IWL: NVM_ACCESS_COMPLETE: %s\n", strerror(r));
        return r;
    }
    r = iwl_wait_init_complete(sc, 2000);
    if(r) {
        kprintf("+IWL: no INIT_COMPLETE from the firmware: %s\n", strerror(r));
        return r;
    }
    want = fw_has_api(sc, IWL_API_REGULATORY_NVM_INFO) ? IWL_NVM_INFO_RSP_LEN_V4 : IWL_NVM_INFO_RSP_LEN_V3;
    rsp = (u8 *)kalloc(IWL_NVM_INFO_RSP_LEN_V4);
    if(!rsp) return -ENOMEM;
    r = iwl_cmd(sc, IWL_WIDE_ID(IWL_REGULATORY_AND_NVM_GROUP, IWL_NVM_GET_INFO), &zero, sizeof(zero), rsp,
                IWL_NVM_INFO_RSP_LEN_V4, &rsp_len);
    if(!r && rsp_len != want) {
        kprintf("+IWL: NVM_GET_INFO: %u bytes, not the %u of its layout\n", rsp_len, want);
        r = -EIO;
    } else if(r) {
        kprintf("+IWL: NVM_GET_INFO: %s\n", strerror(r));
    }
    if(r) {
        kfree(rsp);
        return r;
    }
    h = (const struct iwl_nvm_get_info_head *)rsp;
    sc->nvm_version = h->nvm_version;
    sc->mac_sku_flags = h->mac_sku_flags;
    sc->tx_chains = h->tx_chains;
    sc->rx_chains = h->rx_chains;
    sc->lar_enabled = h->lar_enabled && fw_has_capa(sc, IWL_CAPA_LAR_SUPPORT);
    kprintf("+IWL: NVM version 0x%x%s: bands %s%s, 802.11%s%s%s, %s, antennas tx 0x%x rx 0x%x, %u addresses, "
            "regulatory: %s\n", sc->nvm_version, (h->flags & IWL_NVM_GENERAL_FLAGS_EMPTY_OTP) ? " (an empty OTP)" : "",
            (sc->mac_sku_flags & IWL_NVM_MAC_SKU_BAND_2_4) ? "2.4" : "",
            (sc->mac_sku_flags & IWL_NVM_MAC_SKU_BAND_5_2) ? " 5 GHz" : " GHz",
            (sc->mac_sku_flags & IWL_NVM_MAC_SKU_11N) ? "n" : "", (sc->mac_sku_flags & IWL_NVM_MAC_SKU_11AC) ? "/ac" : "",
            (sc->mac_sku_flags & IWL_NVM_MAC_SKU_11AX) ? "/ax" : "",
            (sc->mac_sku_flags & IWL_NVM_MAC_SKU_MIMO_DISABLED) ? "no MIMO" : "MIMO", sc->tx_chains, sc->rx_chains,
            h->n_hw_addrs, sc->lar_enabled ? "location aware (LAR)" : "the NVM's channel list");
    kfree(rsp);
    return 0;
}

/**** the station layer's operations ****/

/* a start that went wrong: everything off, and what happened kept for wifi status */
static int start_failed(struct iwl_softc *sc, int r, const char *what) {
    if(what) snprintf(sc->last, sizeof(sc->last), "%s (%s)", what, strerror(r));
    kprintf("+IWL: %s\n", sc->last);
    iwl_trans_stop(sc);
    stop_device(sc);
    return r;
}

static int iwl_start(struct wlan_dev *w) {
    struct iwl_softc *sc = (struct iwl_softc *)w->priv;
    struct iwl_fw *fw;
    u8 *file, mac[WLAN_ADDR_LEN];
    char m[18];
    u_int len;
    int r;

    sc->last[0] = 0;
    pci_dev_write16(sc->pci, PCI_COMMAND, pci_dev_read16(sc->pci, PCI_COMMAND) | PCI_COMMAND_MEMORY |
                    PCI_COMMAND_MASTER | PCI_COMMAND_INTX_DISABLE);
    iwl_disable_interrupts(sc);
    r = start_hw(sc);
    if(r) return start_failed(sc, r, "the card did not power up");
    sc->hw_rf_id = iwl_rd(sc, IWL_CSR_HW_RF_ID);    /* Linux reads it once the card is up */
    identify(sc);
    kprintf("+IWL: powered up: RF id 0x%08x (%s, step %c), the radio switch is %s\n", sc->hw_rf_id,
            rf_name(sc->rf_type), 'A' + IWL_HW_RFID_STEP(sc->hw_rf_id), sc->rfkill ? "OFF" : "on");
    if(!sc->fw_name) {
        snprintf(sc->last, sizeof(sc->last), "no firmware file for %s %c0 with %s", mac_name(sc->mac_type),
                 'A' + sc->mac_step, rf_name(sc->rf_type));
        return start_failed(sc, -ENOENT, NULL);     /* (-ENODEV would read as "the device is down") */
    }
    fw = (struct iwl_fw *)kalloc(sizeof(*fw));
    if(!fw) return start_failed(sc, -ENOMEM, "no memory for the firmware");
    r = read_firmware(sc, &file, &len, fw);
    if(r) {
        kfree(fw);
        snprintf(sc->last, sizeof(sc->last), "%s: %s", sc->fw_name, strerror(r));
        return start_failed(sc, r, NULL);
    }
    strlcpy(sc->fw_version, fw->version, sizeof(sc->fw_version));
    r = keep_fw_info(sc, fw);
    if(!r) r = iwl_trans_alloc(sc);
    if(!r) r = iwl_load_firmware(sc, fw);       /* copies what the card needs: the file can go */
    kfree(file);
    kfree(fw);
    if(r) return start_failed(sc, r, "the firmware did not start");
    r = init_firmware(sc);
    if(r) return start_failed(sc, r, "the firmware's init flow failed");
    if(!read_address(sc, mac)) {
        snprintf(sc->last, sizeof(sc->last), "no valid MAC address in the card (%s)", macfmt(mac, m));
        return start_failed(sc, -EIO, NULL);
    }
    memcpy(sc->mac, mac, WLAN_ADDR_LEN);
    memcpy(w->mac, mac, WLAN_ADDR_LEN);
    snprintf(sc->last, sizeof(sc->last), "firmware %s running", sc->fw_version);
    kprintf("+IWL: up: firmware %s, MAC %s, %lu packets from the card so far; scanning is the next step\n",
            sc->fw_version, macfmt(mac, m), sc->rx_packets);
    return 0;
}

static void iwl_stop(struct wlan_dev *w) {
    struct iwl_softc *sc = (struct iwl_softc *)w->priv;
    iwl_trans_stop(sc);
    stop_device(sc);
    snprintf(sc->last, sizeof(sc->last), "stopped (wifi down)");
    kprintf("+IWL: stopped\n");
}

static int iwl_scan(struct wlan_dev *w, const struct wlan_scan_req *req) { return -ENOSYS; }
static void iwl_scan_abort(struct wlan_dev *w) { }
static int iwl_join(struct wlan_dev *w, const struct wlan_bss *bss) { return -ENOSYS; }
static int iwl_tx_mgmt(struct wlan_dev *w, const u8 *frame, u_int len) { return -ENOSYS; }
static int iwl_associated(struct wlan_dev *w, u16 aid, const u8 *ies, u_int ies_len) { return -ENOSYS; }
static int iwl_set_key(struct wlan_dev *w, const struct wlan_key *key) { return -ENOSYS; }
static int iwl_tx_data(struct wlan_dev *w, struct netbuf *nb, bool protect) { return -ENOSYS; }
static void iwl_leave(struct wlan_dev *w) { }

static void iwl_status(struct wlan_dev *w, char *buf, size_t size) {
    struct iwl_softc *sc = (struct iwl_softc *)w->priv;
    if(sc->fw_state == IWL_FW_ALIVE)
        snprintf(buf, size, "Intel 9560 (%s %c0, %s): firmware %s running, %lu packets from the card", mac_name(sc->mac_type),
                 'A' + sc->mac_step, rf_name(sc->rf_type), sc->fw_version, sc->rx_packets);
    else
        snprintf(buf, size, "Intel 9560 (%s %c0, %s): %s", mac_name(sc->mac_type), 'A' + sc->mac_step,
                 rf_name(sc->rf_type), sc->last[0] ? sc->last : (sc->fw_name ? sc->fw_name : "no firmware file for it"));
}

/* the driver's thread: while the firmware runs, the card's notifications every POLL_MS */
static void iwl_thread(void *arg) {
    struct iwl_softc *sc = (struct iwl_softc *)arg;
    for(;;) {
        if(sc->fw_state != IWL_FW_DOWN) iwl_poll(sc);
        sleep_ms(sc->fw_state != IWL_FW_DOWN ? POLL_MS : 100);
    }
}

static const struct wlan_hw_ops iwl_ops = {
    iwl_start, iwl_stop, iwl_scan, iwl_scan_abort, iwl_join, iwl_tx_mgmt, iwl_associated, iwl_set_key,
    iwl_tx_data, iwl_leave, iwl_status
};

/**** at boot ****/

static int attach(struct pci_dev *pci) {
    struct iwl_softc *sc;
    u64 bar = ((u64)pci->bar_hi[0] << 32) | pci->bar[0];
    const char *why = NULL;

    if(pci->bar_io[0] || !bar || pci->bar_size[0] < IWL_BAR0_SIZE) {
        kprintf("+IWL: %02x:%02x.%x: no 16 KB register BAR\n", pci->bus, pci->slot, pci->func);
        return -1;
    }
    sc = (struct iwl_softc *)kcalloc(1, sizeof(*sc));
    if(!sc) return -1;
    sc->pci = pci;
    sc->pcie_cap = pci_find_capability(pci, PCI_CAP_ID_EXP);
    /* the registers: memory decoding on (the firmware leaves it on); bus mastering waits for wifi up. The
       other drivers also clear an old card's retry timeout at 0x41; on this one 0x41 is a pointer in the
       capability list (its PCI Express capability is at 0x40), so it is left alone */
    pci_dev_write16(pci, PCI_COMMAND, pci_dev_read16(pci, PCI_COMMAND) | PCI_COMMAND_MEMORY);
    sc->regs = (volatile u8 *)ioremap64(bar, IWL_BAR0_SIZE, &why);
    if(!sc->regs) {
        kprintf("+IWL: registers at 0x%x%08x cannot be mapped: %s\n", pci->bar_hi[0], pci->bar[0], why);
        kfree(sc);
        return -1;
    }
    sc->hw_rev = iwl_rd(sc, IWL_CSR_HW_REV);
    if(sc->hw_rev == 0xffffffff) {
        kprintf("+IWL: %02x:%02x.%x: the registers do not answer (HW_REV 0xffffffff)\n", pci->bus, pci->slot, pci->func);
        kfree(sc);
        return -1;
    }
    sc->hw_rf_id = iwl_rd(sc, IWL_CSR_HW_RF_ID);
    identify(sc);
    kprintf("+IWL: %02x:%02x.%x %04x:%04x (subsystem %04x:%04x), registers at 0x%x%08x: HW_REV 0x%08x (%s %c0), "
            "RF_ID 0x%08x (%s), firmware %s\n", pci->bus, pci->slot, pci->func, pci->vendor, pci->device,
            pci->subsys_vendor, pci->subsys_id, pci->bar_hi[0], pci->bar[0], sc->hw_rev, mac_name(sc->mac_type),
            'A' + sc->mac_step, sc->hw_rf_id, rf_name(sc->rf_type), sc->fw_name ? sc->fw_name : "unknown until wifi up");
    sc->w = wlan_register(&iwl_ops, sc);
    if(!sc->w) {
        kprintf("+IWL: the station layer refused the card\n");
        kfree(sc);
        return -1;
    }
    if(!kthread_create("iwl", iwl_thread, sc, PL_HIGH, TF_KTHREAD | TF_DETACHED))
        kprintf("+IWL: no thread for the card: its notifications are read only while a command waits\n");
    return 0;
}

/* the driver table's "iwlwifi": 0 found, 1 none here, -1 one that failed */
int init_iwlwifi(void) {
    struct pci_dev *d;
    u_int i;
    int found = 0, r = 1;
    for(i = 0; i < sizeof(iwl_devices) / sizeof(iwl_devices[0]); i++)
        for(d = pci_find_device(0x8086, iwl_devices[i], NULL); d; d = pci_find_device(0x8086, iwl_devices[i], d)) {
            found++;
            if(attach(d) == 0) r = 0;
            else if(r) r = -1;
        }
    return found ? r : 1;
}
