/*
SurfOS Wi-Fi: Intel Wireless-AC 9560 registers
----------------------
File: iwl_reg.h  Date: 10/10/26
----------------------
The registers of the laptop's Wi-Fi card (8086:34f0: the CNVi "Qu" MAC of the Ice Lake
chipset with the "JF" radio module) that driver/wifi/iwl.c uses: the control and status
registers (CSR) in BAR0, the window onto the card's periphery registers (PRPH) and its
memory, and the codes CSR_HW_REV and CSR_HW_RF_ID name the chip with. The brief is
docs/superpowers/plans/2026-10-09-wifi.md.

The names and values are those of OpenBSD's sys/dev/pci/if_iwxreg.h and if_iwxvar.h
(IWX_ there, IWL_ here), which take them from Linux's iwlwifi; if_iwxreg.h carries the
following notice, kept here as its BSD licence asks:

 * Copyright(c) 2017 Intel Deutschland GmbH
 * Copyright(c) 2018 - 2019 Intel Corporation
 * All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
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

#ifndef _SURFOS_IWL_REG_H
#define _SURFOS_IWL_REG_H

#define IWL_BAR0_SIZE               0x4000      /* 16 KB of registers */

/* control and status registers (offsets in BAR0) */
#define IWL_CSR_HW_IF_CONFIG_REG    0x000
#define IWL_CSR_INT                 0x008       /* interrupt causes (write 1 to clear) */
#define IWL_CSR_INT_MASK            0x00c
#define IWL_CSR_FH_INT_STATUS       0x010       /* DMA causes (write 1 to clear) */
#define IWL_CSR_RESET               0x020
#define IWL_CSR_GP_CNTRL            0x024
#define IWL_CSR_HW_REV              0x028
#define IWL_CSR_GIO_REG             0x03c
#define IWL_CSR_MBOX_SET_REG        0x088
#define IWL_CSR_HW_RF_ID            0x09c
#define IWL_CSR_GIO_CHICKEN_BITS    0x100
#define IWL_CSR_MAC_ADDR0_OTP       0x380       /* the 22000 family's (if_iwx.c mac_addr_from_csr): */
#define IWL_CSR_MAC_ADDR1_OTP       0x384       /* the address in the card's OTP, */
#define IWL_CSR_MAC_ADDR0_STRAP     0x388       /* and one an OEM fused in, used when valid */
#define IWL_CSR_MAC_ADDR1_STRAP     0x38c
#define IWL_CSR_DBG_HPET_MEM_REG    0x240
#define IWL_CSR_DBG_LINK_PWR_MGMT_REG 0x250

/* CSR_HW_IF_CONFIG_REG */
#define IWL_HW_IF_CONFIG_BIT_HAP_WAKE_L1A   0x00080000
#define IWL_HW_IF_CONFIG_BIT_NIC_READY      0x00400000
#define IWL_HW_IF_CONFIG_PREPARE            0x08000000
#define IWL_HW_IF_CONFIG_ENABLE_PME         0x10000000

/* CSR_RESET */
#define IWL_RESET_SW_RESET              0x00000080
#define IWL_RESET_MASTER_DISABLED       0x00000100
#define IWL_RESET_STOP_MASTER           0x00000200
#define IWL_RESET_LINK_PWR_MGMT_DISABLED 0x80000000

/* CSR_GP_CNTRL */
#define IWL_GP_CNTRL_MAC_CLOCK_READY    0x00000001
#define IWL_GP_CNTRL_INIT_DONE          0x00000004
#define IWL_GP_CNTRL_MAC_ACCESS_REQ     0x00000008
#define IWL_GP_CNTRL_GOING_TO_SLEEP     0x00000010
#define IWL_GP_CNTRL_RFKILL_WAKE_L1A_EN 0x04000000
#define IWL_GP_CNTRL_HW_RF_KILL_SW      0x08000000  /* set: the radio may transmit (0 is the switch off) */

#define IWL_MBOX_SET_OS_ALIVE           0x20
#define IWL_GIO_VAL_L0S_DISABLED        0x00000002
#define IWL_GIO_CHICKEN_L1A_NO_L0S_RX   0x00800000
#define IWL_DBG_HPET_MEM_REG_VAL        0xffff0000

/* CSR_INT causes */
#define IWL_INT_BIT_RF_KILL             (1u << 7)

/* the window onto the card's periphery registers and its memory: only with the NIC awake
   (MAC_ACCESS_REQ granted), but for the few the power-up reads before that */
#define IWL_HBUS_TARG_MEM_RADDR     0x40c
#define IWL_HBUS_TARG_MEM_WADDR     0x410
#define IWL_HBUS_TARG_MEM_WDAT      0x418
#define IWL_HBUS_TARG_MEM_RDAT      0x41c
#define IWL_HBUS_TARG_PRPH_WADDR    0x444
#define IWL_HBUS_TARG_PRPH_RADDR    0x448
#define IWL_HBUS_TARG_PRPH_WDAT     0x44c
#define IWL_HBUS_TARG_PRPH_RDAT     0x450
#define IWL_PRPH_ADDR_MASK          0x000fffff  /* the 22000 family */

/* periphery registers */
#define IWL_PRPH_WPROT_22000        0xa04d00
#define IWL_PRPH_WFPM_ACCESS        (1u << 12)
#define IWL_HPM_DEBUG               0xa03440
#define IWL_PERSISTENCE_BIT         (1u << 12)
#define IWL_HPM_HIPM_GEN_CFG        0xa03458
#define IWL_HPM_HIPM_GEN_CFG_CR_PG_EN       (1u << 0)
#define IWL_HPM_HIPM_GEN_CFG_CR_SLP_EN      (1u << 1)
#define IWL_HPM_HIPM_GEN_CFG_CR_FORCE_ACTIVE (1u << 10)
#define IWL_UREG_CHICK              0xa05c00
#define IWL_UREG_CHICK_MSI_ENABLE   (1u << 24)

/* CSR_HW_REV: bits 15-4 the MAC's type; in this family bits 1-0 its step (A 0, B 1, C 2) */
#define IWL_HW_REV_TYPE(v)          (((v) >> 4) & 0xfff)
#define IWL_HW_REV_STEP(v)          ((v) & 0x3)
#define IWL_MAC_TYPE_QU             0x33
#define IWL_MAC_TYPE_QUZ            0x35
#define IWL_SILICON_B_STEP          1
#define IWL_SILICON_C_STEP          2

/* CSR_HW_RF_ID: bits 23-12 the radio's type, 11-8 its step */
#define IWL_HW_RFID_TYPE(v)         (((v) >> 12) & 0xfff)
#define IWL_HW_RFID_STEP(v)         (((v) >> 8) & 0xf)
#define IWL_HW_RFID_IS_CDB(v)       (((v) >> 28) & 1)
#define IWL_RF_TYPE_JF2             0x105
#define IWL_RF_TYPE_JF1             0x108
#define IWL_RF_TYPE_HR2             0x10a
#define IWL_RF_TYPE_HR1             0x10c

#endif
