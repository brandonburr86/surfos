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
#define IWL_CSR_INT_COALESCING      0x004       /* a byte: interrupt coalescing, in 32 us units */
#define IWL_CSR_INT                 0x008       /* interrupt causes (write 1 to clear) */
#define IWL_CSR_INT_MASK            0x00c
#define IWL_CSR_FH_INT_STATUS       0x010       /* DMA causes (write 1 to clear) */
#define IWL_CSR_RESET               0x020
#define IWL_CSR_GP_CNTRL            0x024
#define IWL_CSR_HW_REV              0x028
#define IWL_CSR_GIO_REG             0x03c
#define IWL_CSR_CTXT_INFO_BA        0x040       /* the context info's address, low then high word */
#define IWL_CSR_UCODE_DRV_GP1_CLR   0x05c
#define IWL_CSR_MBOX_SET_REG        0x088
#define IWL_CSR_HW_RF_ID            0x09c
#define IWL_CSR_MAC_SHADOW_REG_CTRL 0x0a8
#define IWL_CSR_GIO_CHICKEN_BITS    0x100
#define IWL_CSR_MAC_ADDR0_OTP       0x380       /* the 22000 family's (if_iwx.c mac_addr_from_csr): */
#define IWL_CSR_MAC_ADDR1_OTP       0x384       /* the address in the card's OTP, */
#define IWL_CSR_MAC_ADDR0_STRAP     0x388       /* and one an OEM fused in, used when valid */
#define IWL_CSR_MAC_ADDR1_STRAP     0x38c
#define IWL_CSR_DBG_HPET_MEM_REG    0x240
#define IWL_CSR_DBG_LINK_PWR_MGMT_REG 0x250

/* CSR_HW_IF_CONFIG_REG */
#define IWL_HW_IF_CONFIG_MSK_MAC_DASH       0x00000003
#define IWL_HW_IF_CONFIG_MSK_MAC_STEP       0x0000000c
#define IWL_HW_IF_CONFIG_BIT_MAC_SI         0x00000100
#define IWL_HW_IF_CONFIG_BIT_RADIO_SI       0x00000200
#define IWL_HW_IF_CONFIG_MSK_PHY_TYPE       0x00000c00
#define IWL_HW_IF_CONFIG_MSK_PHY_DASH       0x00003000
#define IWL_HW_IF_CONFIG_MSK_PHY_STEP       0x0000c000
#define IWL_HW_IF_CONFIG_POS_MAC_DASH       0
#define IWL_HW_IF_CONFIG_POS_MAC_STEP       2
#define IWL_HW_IF_CONFIG_POS_PHY_TYPE       10
#define IWL_HW_IF_CONFIG_POS_PHY_DASH       12
#define IWL_HW_IF_CONFIG_POS_PHY_STEP       14
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
#define IWL_INT_BIT_FH_RX               (1u << 31)  /* received buffers, command responses */
#define IWL_INT_BIT_HW_ERR              (1u << 29)
#define IWL_INT_BIT_RX_PERIODIC         (1u << 28)
#define IWL_INT_BIT_FH_TX               (1u << 27)
#define IWL_INT_BIT_SW_ERR              (1u << 25)  /* the firmware stopped with an error */
#define IWL_INT_BIT_RF_KILL             (1u << 7)
#define IWL_INT_BIT_CT_KILL             (1u << 6)
#define IWL_INT_BIT_SW_RX               (1u << 3)
#define IWL_INT_BIT_WAKEUP              (1u << 1)
#define IWL_INT_BIT_ALIVE               (1u << 0)   /* the firmware runs (and has set up the receive DMA) */
#define IWL_INT_INI_SET_MASK            (IWL_INT_BIT_FH_RX | IWL_INT_BIT_HW_ERR | IWL_INT_BIT_FH_TX | IWL_INT_BIT_SW_ERR | \
                                         IWL_INT_BIT_RF_KILL | IWL_INT_BIT_SW_RX | IWL_INT_BIT_WAKEUP | \
                                         IWL_INT_BIT_ALIVE | IWL_INT_BIT_RX_PERIODIC)

/* CSR_UCODE_DRV_GP1 */
#define IWL_UCODE_SW_BIT_RFKILL         0x00000002
#define IWL_UCODE_DRV_GP1_BIT_CMD_BLOCKED 0x00000004

#define IWL_HOST_INT_TIMEOUT_DEF        0x40

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
#define IWL_HBUS_TARG_WRPTR         0x460       /* a transmit queue's write pointer: queue << 16 | index */
#define IWL_RFH_Q0_FRBDCB_WIDX_TRG  0x1c80      /* the receive queue's free buffers: how far they go */

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
#define IWL_UREG_CPU_INIT_RUN       0xa05c44    /* 1: the card loads its firmware from the context info */
#define IWL_HPM_MAC_LTR_CSR         0xa0348c
#define IWL_HPM_MAC_LTR_ENABLE_ALL  0xf
#define IWL_HPM_UMAC_LTR            0xa03480
#define IWL_RFH_GEN_STATUS          0xa09808
#define IWL_RXF_DMA_IDLE            (1u << 31)
#define IWL_RFH_RXF_DMA_CFG         0xa09820

/* CSR_LTR_LONG_VAL_AD's layout, the value written to HPM_UMAC_LTR: ~250 us, snoop and no snoop
   (if_iwx.c iwx_set_ltr()) */
#define IWL_LTR_VAL_250US           (0x80000000 | (2u << 26) | (250u << 16) | 0x00008000 | (2u << 10) | 250u)

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

/**** the firmware's transport (step 3): host memory the card reads and writes ****/

#define IWL_MAX_DRAM_ENTRY      64

/* the context info: what the card loads its firmware from and where its queues are
   (if_iwxreg.h struct iwx_context_info); 1792 bytes */
struct iwl_context_info {
    u16 mac_id;                     /* version: CSR_HW_REV's low half */
    u16 version;
    u16 size;                       /* in 32-bit words */
    u16 reserved_v;
    u32 control_flags;
    u32 reserved_c;
    u64 reserved0;
    u64 free_rbd_addr;              /* the receive queue: free buffers, used buffers, status */
    u64 used_rbd_addr;
    u64 status_wr_ptr;
    u64 cmd_queue_addr;             /* the command queue's TFDs */
    u8 cmd_queue_size;              /* log2(entries) - 3 */
    u8 reserved_h[7];
    u32 reserved1[4];
    u64 core_dump_addr;
    u32 core_dump_size;
    u32 reserved_d;
    u64 early_debug_addr;
    u32 early_debug_size;
    u32 reserved_e;
    u64 platform_nvm_addr;
    u32 platform_nvm_size;
    u32 reserved_p;
    u32 reserved2[16];
    u64 umac_img[IWL_MAX_DRAM_ENTRY];   /* the firmware's sections in host memory, up to 32 KB each */
    u64 lmac_img[IWL_MAX_DRAM_ENTRY];
    u64 virtual_img[IWL_MAX_DRAM_ENTRY];    /* the paged ones, which stay */
    u32 reserved3[16];
};                                  /* every field at its natural place: no packing needed, and none wanted
                                       (the address lists are handed around as u64 arrays) */
typedef char iwl_context_info_is_1792_bytes[sizeof(struct iwl_context_info) == 1792 ? 1 : -1];

#define IWL_CTXT_INFO_RB_CB_SIZE_POS    4       /* log2 of the receive queue's buffers */
#define IWL_CTXT_INFO_TFD_FORMAT_LONG   (1u << 8)
#define IWL_CTXT_INFO_RB_SIZE_POS       9
#define IWL_CTXT_INFO_RB_SIZE_4K        0x4

/* the receive queue's status, written by the card */
struct iwl_rb_status {
    u16 closed_rb_num;              /* low 12 bits: the buffers it has filled, counting on */
    u16 closed_fr_num;
    u16 finished_rb_num;
    u16 finished_fr_num;
    u32 unused;
} __attribute__((packed));

/* a transmit frame descriptor: up to 25 pieces of host memory (if_iwxreg.h struct iwx_tfh_tfd) */
#define IWL_TFH_NUM_TBS         25
struct iwl_tfh_tb {
    u16 tb_len;
    u64 addr;
} __attribute__((packed));
struct iwl_tfh_tfd {
    u16 num_tbs;
    struct iwl_tfh_tb tbs[IWL_TFH_NUM_TBS];
    u32 pad;
} __attribute__((packed));                  /* 256 bytes */

#define IWL_FIRST_TB_SIZE       20          /* the first piece of a command, which the card copies */

/* a command's header (if_iwxreg.h struct iwx_cmd_header_wide) and a received packet's */
struct iwl_cmd_header_wide {
    u8 opcode;
    u8 group_id;
    u8 idx;
    u8 qid;
    u16 length;
    u8 reserved;
    u8 version;
} __attribute__((packed));

struct iwl_cmd_header {
    u8 code;
    u8 flags;                       /* IWL_CMD_FAILED_MSK; a response's group */
    u8 idx;
    u8 qid;                         /* bit 7: a notification, not a response */
} __attribute__((packed));

#define IWL_DEF_CMD_PAYLOAD_SIZE 320
struct iwl_device_cmd {
    struct iwl_cmd_header_wide hdr;
    u8 data[IWL_DEF_CMD_PAYLOAD_SIZE - 4];
} __attribute__((packed));          /* 324 bytes, as if_iwxreg.h's union */

struct iwl_rx_packet {
    u32 len_n_flags;                /* bits 13-0: the length of what follows it */
    struct iwl_cmd_header hdr;
    u8 data[];
} __attribute__((packed));

#define IWL_RX_FRAME_SIZE_MSK   0x00003fff
#define IWL_RX_FRAME_INVALID    0x55550000
#define IWL_RX_FRAME_ALIGN      0x40
#define IWL_CMD_FAILED_MSK      0x40

/* command ids: group << 8 | opcode */
#define IWL_WIDE_ID(grp, op)    (((grp) << 8) | (op))
#define IWL_LEGACY_GROUP        0x0
#define IWL_LONG_GROUP          0x1
#define IWL_SYSTEM_GROUP        0x2
#define IWL_REGULATORY_AND_NVM_GROUP 0xc
#define IWL_ALIVE               0x01
#define IWL_REPLY_ERROR         0x02
#define IWL_INIT_COMPLETE_NOTIF 0x04
#define IWL_INIT_EXTENDED_CFG_CMD 0x03      /* the system group's */
#define IWL_NVM_ACCESS_COMPLETE 0x00        /* the regulatory and NVM group's */
#define IWL_NVM_GET_INFO        0x02

#define IWL_INIT_NVM            (1u << 1)   /* INIT_EXTENDED_CFG_CMD: NVM access commands follow */

/* the ALIVE notification (if_iwxreg.h struct iwx_alive_resp_v4 .. v6) */
#define IWL_ALIVE_STATUS_OK     0xcafe
struct iwl_lmac_alive {
    u32 ucode_major;
    u32 ucode_minor;
    u8 ver_subtype, ver_type, mac, opt;
    u32 timestamp;
    u32 error_event_table_ptr;      /* the debug addresses */
    u32 log_event_table_ptr;
    u32 cpu_register_ptr;
    u32 dbgm_config_ptr;
    u32 alive_counter_ptr;
    u32 scd_base_ptr;
    u32 st_fwrd_addr;
    u32 st_fwrd_size;
} __attribute__((packed));
struct iwl_umac_alive {
    u32 umac_major;
    u32 umac_minor;
    u32 error_info_addr;
    u32 dbg_print_buff_addr;
} __attribute__((packed));
struct iwl_alive_resp {             /* version 4; 5 adds the SKU id (12 bytes), 6 and 7 an IMR block (16 more) */
    u16 status;
    u16 flags;
    struct iwl_lmac_alive lmac_data[2];
    struct iwl_umac_alive umac_data;
} __attribute__((packed));

/* NVM_GET_INFO's response (if_iwxreg.h struct iwx_nvm_get_info_rsp, _v3) */
#define IWL_NVM_GENERAL_FLAGS_EMPTY_OTP     (1u << 0)
#define IWL_NVM_MAC_SKU_BAND_2_4            (1u << 0)
#define IWL_NVM_MAC_SKU_BAND_5_2            (1u << 1)
#define IWL_NVM_MAC_SKU_11N                 (1u << 2)
#define IWL_NVM_MAC_SKU_11AC                (1u << 3)
#define IWL_NVM_MAC_SKU_11AX                (1u << 4)
#define IWL_NVM_MAC_SKU_MIMO_DISABLED       (1u << 5)
#define IWL_NUM_CHANNELS_V1     51
#define IWL_NUM_CHANNELS        110
struct iwl_nvm_get_info_head {
    u32 flags;                      /* general */
    u16 nvm_version;
    u8 board_type;
    u8 n_hw_addrs;
    u32 mac_sku_flags;
    u32 tx_chains;
    u32 rx_chains;
    u32 lar_enabled;                /* regulatory */
} __attribute__((packed));
#define IWL_NVM_INFO_RSP_LEN_V3 (sizeof(struct iwl_nvm_get_info_head) + IWL_NUM_CHANNELS_V1 * 2 + 2)
#define IWL_NVM_INFO_RSP_LEN_V4 (sizeof(struct iwl_nvm_get_info_head) + 4 + IWL_NUM_CHANNELS * 4)
#define IWL_API_REGULATORY_NVM_INFO 48      /* the API bit for the second layout */

/* the firmware's error logs in its memory (if_iwx.c struct iwx_error_event_table, the first
   words; struct iwx_umac_error_event_table) */
struct iwl_error_table {
    u32 valid, error_id, trm_hw_status0, trm_hw_status1, blink2, ilink1, ilink2, data1, data2, data3;
    u32 bcon_time, tsf_low, tsf_hi, gp1, gp2, fw_rev_type, major, minor, hw_ver, brd_ver;
    u32 log_pc, frame_ptr, stack_ptr, hcmd, isr0, isr1, isr2, isr3, isr4, last_cmd_id;
};                                  /* 32-bit words only: read a word at a time */
struct iwl_umac_error_table {
    u32 valid, error_id, blink1, blink2, ilink1, ilink2, data1, data2, data3;
    u32 umac_major, umac_minor, frame_pointer, stack_pointer, cmd_header, nic_isr_pref;
};
#define IWL_ERROR_TABLE_MIN     0x400000    /* a smaller address is not one */

/**** step 4: the firmware's configuration and scanning (driver/wifi/iwl_mac.c) ****/

/* more command and notification ids (group << 8 | opcode; a legacy group 0 command goes out in
   the long group: firmware API 50 and later refuses group 0) */
#define IWL_PHY_OPS_GROUP           0x4
#define IWL_SOC_CONFIGURATION_CMD   0x01        /* the system group's */
#define IWL_TEMP_REPORTING_THRESHOLDS_CMD 0x04  /* the PHY operations group's */
#define IWL_CT_KILL_NOTIFICATION    0xfe
#define IWL_SCAN_CFG_CMD            0x0c        /* the long group's */
#define IWL_SCAN_REQ_UMAC           0x0d
#define IWL_SCAN_ABORT_UMAC         0x0e
#define IWL_SCAN_COMPLETE_UMAC      0x0f
#define IWL_SCAN_ITERATION_COMPLETE_UMAC 0xb5
#define IWL_POWER_TABLE_CMD         0x77
#define IWL_TX_ANT_CONFIGURATION_CMD 0x98
#define IWL_BT_CONFIG               0x9b
#define IWL_REPLY_RX_PHY_CMD        0xc0
#define IWL_REPLY_RX_MPDU_CMD       0xc1
#define IWL_MCC_UPDATE_CMD          0xc8
#define IWL_MCC_CHUB_UPDATE_CMD     0xc9
#define IWL_REPLY_BEACON_FILTERING_CMD 0xd2
#define IWL_LTR_CONFIG              0xee

/* API and capability bits used (if_iwxreg.h IWX_UCODE_TLV_API_*, _CAPA_*) */
#define IWL_API_WIFI_MCC_UPDATE     9
#define IWL_API_REDUCED_SCAN_CONFIG 56
#define IWL_API_SCAN_EXT_CHAN_VER   58
#define IWL_CAPA_DS_PARAM_SET_IE_SUPPORT 9
#define IWL_CAPA_LAR_MULTI_MCC      29
#define IWL_CAPA_CT_KILL_BY_FW      74
#define IWL_CAPA_MCC_UPDATE_11AX_SUPPORT 89

/* the firmware's PHY_SKU: valid antennas (if_iwxreg.h IWX_FW_PHY_CFG_TX_CHAIN, _RX_CHAIN) */
#define IWL_FW_PHY_CFG_TX_CHAIN(v)  (((v) >> 16) & 0xf)
#define IWL_FW_PHY_CFG_RX_CHAIN(v)  (((v) >> 20) & 0xf)

struct iwl_tx_ant_cfg_cmd { u32 valid; } __attribute__((packed));
#define IWL_BT_COEX_WIFI            0x3
struct iwl_bt_coex_cmd { u32 mode; u32 enabled_modules; } __attribute__((packed));
/* SOC_CONFIGURATION_CMD version 2: the platform's power stabilization (this card: an LTR delay of
   1820 us and a crystal latency of 1820 us, as Linux's iwl_qu_medium_latency_mac_cfg has it for
   8086:34f0; the delay's code goes in bits 3-2, as Linux encodes it) */
#define IWL_SOC_FLAGS_LTR_APPLY_DELAY_1820  3
#define IWL_SOC_FLAGS_LTR_APPLY_DELAY_SHIFT 2
#define IWL_SOC_XTAL_LATENCY_34F0           1820
struct iwl_soc_configuration_cmd { u32 flags; u32 latency; } __attribute__((packed));
#define IWL_LTR_CFG_FLAG_FEATURE_ENABLE 0x00000001
struct iwl_ltr_config_cmd {
    u32 flags, static_long, static_short, ltr_cfg_values[4], ltr_short_idle_timeout;
} __attribute__((packed));
struct iwl_temp_report_ths_cmd { u32 num_temps; u16 thresholds[8]; } __attribute__((packed));
struct iwl_device_power_cmd { u16 flags; u16 reserved; } __attribute__((packed));   /* 0: no power saving */
struct iwl_beacon_filter_cmd { u32 words[15]; } __attribute__((packed));            /* all 0: filtering off */

/* MCC_UPDATE_CMD: the regulatory domain ("ZZ": the NVM's own); its response (layout 4) lists the
   flags of every channel the card knows */
#define IWL_MCC_SOURCE_OLD_FW       0
#define IWL_MCC_SOURCE_GET_CURRENT  0x10
struct iwl_mcc_update_cmd {
    u16 mcc;
    u8 source_id;
    u8 reserved;
    u32 key;
    u32 reserved2[5];
} __attribute__((packed));
struct iwl_mcc_update_resp {
    u32 status;
    u16 mcc;
    u16 cap;
    u16 time;
    u16 geo_info;
    u8 source_id;
    u8 reserved[3];
    u32 n_channels;
    u32 channels[];                 /* the NVM's channel flags, below */
} __attribute__((packed));

/* the card's channels: the order of the NVM's and MCC's channel lists (if_iwx.c iwx_nvm_channels_8000) */
#define IWL_NUM_2GHZ_CHANNELS       14
#define IWL_NUM_5GHZ_CHANNELS       37
#define IWL_NUM_NVM_CHANNELS        (IWL_NUM_2GHZ_CHANNELS + IWL_NUM_5GHZ_CHANNELS)
#define IWL_NVM_CHANNEL_VALID       (1u << 0)
#define IWL_NVM_CHANNEL_ACTIVE      (1u << 3)   /* active scanning (probes) allowed */
#define IWL_NVM_CHANNEL_RADAR       (1u << 4)
#define IWL_NVM_CHANNEL_INDOOR_ONLY (1u << 5)
#define IWL_NVM_CHANNEL_DFS         (1u << 7)
#define IWL_NVM_CHANNEL_40MHZ       (1u << 9)
#define IWL_NVM_CHANNEL_80MHZ       (1u << 10)

/* SCAN_CFG_CMD, the reduced one (version 5: no broadcast station) */
struct iwl_scan_config {
    u8 enable_cam_mode;
    u8 enable_promiscuous_mode;
    u8 bcast_sta_id;
    u8 reserved;
    u32 tx_chains;
    u32 rx_chains;
} __attribute__((packed));

/* SCAN_REQ_UMAC versions 14 and 15 (if_iwxreg.h struct iwx_scan_req_umac_v14; version 15's general
   parameters, version 11, have the same layout as version 10's) */
#define IWL_SCAN_TWO_LMACS          2
#define IWL_SCAN_LB_LMAC_IDX        0           /* 2.4 GHz */
#define IWL_SCAN_HB_LMAC_IDX        1           /* 5 GHz */
#define IWL_SCAN_PRIORITY_EXT_6     6
#define IWL_PHY_BAND_5              0
#define IWL_PHY_BAND_24             1
#define IWL_UMAC_SCAN_GEN_FLAGS_V2_PASS_ALL           (1u << 1)
#define IWL_UMAC_SCAN_GEN_FLAGS_V2_NTFY_ITER_COMPLETE (1u << 2)
#define IWL_UMAC_SCAN_GEN_FLAGS_V2_ADAPTIVE_DWELL     (1u << 7)
#define IWL_UMAC_SCAN_GEN_FLAGS_V2_FORCE_PASSIVE      (1u << 11)
#define IWL_SCAN_CHANNEL_FLAG_ENABLE_CHAN_ORDER       (1u << 5)
#define IWL_SCAN_DWELL_ACTIVE       10
#define IWL_SCAN_DWELL_PASSIVE      110
#define IWL_SCAN_ADWELL_MAX_BUDGET_FULL_SCAN     300
#define IWL_SCAN_ADWELL_MAX_BUDGET_DIRECTED_SCAN 100
#define IWL_SCAN_ADWELL_DEFAULT_HB_N_APS         8
#define IWL_SCAN_ADWELL_DEFAULT_LB_N_APS         2
#define IWL_SCAN_ADWELL_DEFAULT_N_APS_SOCIAL     10
#define IWL_SCAN_ADWELL_N_APS_GO_FRIENDLY        10
#define IWL_SCAN_ADWELL_N_APS_SOCIAL_CHS         2
#define IWL_SCAN_MAX_CHANNELS       67
#define IWL_PROBE_OPTION_MAX        20
#define IWL_SCAN_OFFLOAD_PROBE_REQ_SIZE 512

struct iwl_scan_general_params_v10 {
    u16 flags;
    u8 reserved;
    u8 scan_start_mac_id;
    u8 active_dwell[IWL_SCAN_TWO_LMACS];
    u8 adwell_default_2g;
    u8 adwell_default_5g;
    u8 adwell_default_social_chn;
    u8 reserved1;
    u16 adwell_max_budget;
    u32 max_out_of_time[IWL_SCAN_TWO_LMACS];
    u32 suspend_time[IWL_SCAN_TWO_LMACS];
    u32 scan_priority;
    u8 passive_dwell[IWL_SCAN_TWO_LMACS];
    u8 num_of_fragments[IWL_SCAN_TWO_LMACS];
} __attribute__((packed));

struct iwl_scan_channel_cfg_umac {
    u32 flags;                      /* bits 0-19: the direct SSIDs probed on it */
    u8 channel_num;                 /* version 2 of the layout (API SCAN_EXT_CHAN_VER): with the band */
    u8 band;
    u8 iter_count;
    u8 iter_interval;
} __attribute__((packed));

struct iwl_scan_channel_params_v6 {
    u8 flags;
    u8 count;
    u8 n_aps_override[2];
    struct iwl_scan_channel_cfg_umac channel_config[IWL_SCAN_MAX_CHANNELS];
} __attribute__((packed));

struct iwl_scan_umac_schedule {
    u16 interval;
    u8 iter_count;
    u8 reserved;
} __attribute__((packed));

struct iwl_scan_periodic_parms_v1 {
    struct iwl_scan_umac_schedule schedule[2];
    u16 delay;
    u16 reserved;
} __attribute__((packed));

struct iwl_scan_probe_segment {
    u16 offset;
    u16 len;
} __attribute__((packed));

struct iwl_scan_probe_req {         /* the probe request the firmware sends, in pieces */
    struct iwl_scan_probe_segment mac_header;
    struct iwl_scan_probe_segment band_data[3];
    struct iwl_scan_probe_segment common_data;
    u8 buf[IWL_SCAN_OFFLOAD_PROBE_REQ_SIZE];
} __attribute__((packed));

struct iwl_ssid_ie {
    u8 id;
    u8 len;
    u8 ssid[32];
} __attribute__((packed));

struct iwl_scan_probe_params_v4 {
    struct iwl_scan_probe_req preq;
    u8 short_ssid_num;
    u8 bssid_num;
    u16 reserved;
    struct iwl_ssid_ie direct_scan[IWL_PROBE_OPTION_MAX];
    u32 short_ssid[8];
    u8 bssid_array[16][6];
} __attribute__((packed));

struct iwl_scan_req_umac_v14 {
    u32 uid;
    u32 ooc_priority;
    struct iwl_scan_general_params_v10 general_params;
    struct iwl_scan_channel_params_v6 channel_params;
    struct iwl_scan_periodic_parms_v1 periodic_params;
    struct iwl_scan_probe_params_v4 probe_params;
} __attribute__((packed));

struct iwl_umac_scan_abort { u32 uid; u32 flags; } __attribute__((packed));
struct iwl_umac_scan_complete {
    u32 uid;
    u8 last_schedule;
    u8 last_iter;
    u8 status;
    u8 ebs_status;
    u32 time_from_last_iter;
    u32 reserved;
} __attribute__((packed));
struct iwl_umac_scan_iter_complete {
    u32 uid;
    u8 scanned_channels;
    u8 status;
    u8 bt_status;
    u8 last_channel;
    u32 tsf_low;
    u32 tsf_high;
} __attribute__((packed));

/* a received frame's descriptor (if_iwxreg.h struct iwx_rx_mpdu_desc with its version 1 tail, the
   22000 family's: IWX_RX_DESC_SIZE_V1, 48 bytes); the frame follows it */
#define IWL_RX_MPDU_RES_STATUS_CRC_OK       (1u << 0)
#define IWL_RX_MPDU_RES_STATUS_OVERRUN_OK   (1u << 1)
#define IWL_RX_MPDU_RES_STATUS_SEC_ENC_MSK  (7u << 8)
#define IWL_RX_MPDU_RES_STATUS_SEC_NO_ENC   (0u << 8)
#define IWL_RX_MPDU_RES_STATUS_SEC_CCM_ENC  (2u << 8)
#define IWL_RX_MPDU_RES_STATUS_DEC_DONE     (1u << 11)
#define IWL_RX_MPDU_RES_STATUS_MIC_OK       (1u << 6)
#define IWL_RX_MPDU_MFLG2_PAD               0x20    /* 2 bytes of padding after the 802.11 header */
#define IWL_RX_MPDU_MFLG2_AMSDU             0x40
struct iwl_rx_mpdu_desc {
    u16 mpdu_len;
    u8 mac_flags1;
    u8 mac_flags2;
    u8 amsdu_info;
    u16 phy_info;
    u8 mac_phy_idx;
    u16 raw_csum;
    u16 l3l4_flags;
    u32 status;
    u32 reorder_data;
    u32 rss_hash;                   /* version 1 */
    u32 filter_match;
    u32 rate_n_flags;
    u8 energy_a;                    /* -dBm, 0 when nothing */
    u8 energy_b;
    u8 channel;
    u8 mac_context;
    u32 gp2_on_air_rise;
    u32 tsf_on_air_rise0;
    u32 tsf_on_air_rise1;
} __attribute__((packed));
typedef char iwl_rx_mpdu_desc_is_48_bytes[sizeof(struct iwl_rx_mpdu_desc) == 48 ? 1 : -1];
typedef char iwl_scan_req_umac_v14_size[sizeof(struct iwl_scan_req_umac_v14) == 1940 ? 1 : -1];

/**** W3: joining a network, sending and receiving (driver/wifi/iwl_mac.c, iwl_trans.c) ****/

#define IWL_MAC_CONF_GROUP          0x3
#define IWL_DATA_PATH_GROUP         0x5
#define IWL_PHY_CONTEXT_CMD         0x08        /* the long group's (from the legacy group) */
#define IWL_ADD_STA_KEY             0x17
#define IWL_ADD_STA                 0x18
#define IWL_REMOVE_STA              0x19
#define IWL_TX_CMD                  0x1c        /* a frame to send; its response says how it went */
#define IWL_TXPATH_FLUSH            0x1e
#define IWL_MAC_CONTEXT_CMD         0x28
#define IWL_BINDING_CONTEXT_CMD     0x2b
#define IWL_MAC_PM_POWER_TABLE      0xa9
#define IWL_MCAST_FILTER_CMD        0xd0
#define IWL_REPLY_SF_CFG_CMD        0xd1
#define IWL_SESSION_PROTECTION_CMD  0x05        /* the MAC configuration group's */
#define IWL_SESSION_PROTECTION_NOTIF 0xfb
#define IWL_RLC_CONFIG_CMD          0x08        /* the data path group's */
#define IWL_TLC_MNG_CONFIG_CMD      0x0f
#define IWL_SCD_QUEUE_CONFIG_CMD    0x17
#define IWL_TLC_MNG_UPDATE_NOTIF    0xf7

#define IWL_FW_CTXT_ACTION_ADD      1
#define IWL_FW_CTXT_ACTION_MODIFY   2
#define IWL_FW_CTXT_ACTION_REMOVE   3
#define IWL_FW_CTXT_INVALID         0xffffffff
#define IWL_FW_CMD_ID_AND_COLOR(id, color) ((u32)(id) | ((u32)(color) << 8))
#define IWL_CAPA_CDB_SUPPORT        40
#define IWL_CAPA_ULTRA_HB_CHANNELS  48          /* the PHY context's longer channel layout */
#define IWL_API_SMART_FIFO_OFFLOAD  68
#define IWL_LMAC_24G_INDEX          0
#define IWL_PHY_VHT_CHANNEL_MODE20  0
#define IWL_PHY_VHT_CTRL_POS_1_BELOW 0
#define IWL_PHY_RX_CHAIN_VALID_POS  1
#define IWL_PHY_RX_CHAIN_CNT_POS    10
#define IWL_PHY_RX_CHAIN_MIMO_CNT_POS 12

/* PHY_CONTEXT_CMD versions 3 and 4 with the 6-7 GHz channel layout (if_iwxreg.h struct
   iwx_phy_context_cmd_uhb: the firmware has the ULTRA_HB_CHANNELS capability) */
struct iwl_phy_context_cmd_uhb {
    u32 id_and_color;
    u32 action;
    u32 channel;                    /* struct iwx_fw_channel_info: the channel, band, width, position */
    u8 band;
    u8 width;
    u8 ctrl_pos;
    u8 reserved_ci;
    u32 lmac_id;
    u32 rxchain_info;               /* reserved in version 4: RLC_CONFIG_CMD has it */
    u32 dsp_cfg_flags;
    u32 reserved;
} __attribute__((packed));
/* the same with the 4-byte channel layout (no ULTRA_HB_CHANNELS) */
struct iwl_phy_context_cmd {
    u32 id_and_color;
    u32 action;
    u8 band;
    u8 channel;
    u8 width;
    u8 ctrl_pos;
    u32 lmac_id;
    u32 rxchain_info;
    u32 dsp_cfg_flags;
    u32 reserved;
} __attribute__((packed));
struct iwl_rlc_config_cmd {         /* version 2 */
    u32 phy_id;
    u32 rx_chain_info;
    u32 reserved_rlc;
    u32 chain_a_sad_mode, chain_b_sad_mode, sad_mac_id, sad_reserved;
    u8 flags;
    u8 reserved[3];
} __attribute__((packed));

/* MAC_CONTEXT_CMD (if_iwxreg.h struct iwx_mac_ctx_cmd with the station's data) */
#define IWL_FW_MAC_TYPE_BSS_STA     5
#define IWL_TSF_ID_A                0
#define IWL_MAC_FILTER_ACCEPT_GRP   (1u << 2)
#define IWL_MAC_FILTER_IN_BEACON    (1u << 6)
#define IWL_MAC_QOS_FLG_UPDATE_EDCA (1u << 0)
#define IWL_MAC_FLG_SHORT_SLOT      (1u << 4)
#define IWL_MAC_FLG_SHORT_PREAMBLE  (1u << 5)
#define IWL_AC_NUM                  4
#define IWL_TX_FIFO_BK              0           /* the 22000 family's EDCA FIFOs (IWX_GEN2_EDCA_TX_FIFO_*) */
#define IWL_TX_FIFO_BE              1
#define IWL_TX_FIFO_VI              2
#define IWL_TX_FIFO_VO              3
struct iwl_ac_qos {
    u16 cw_min;
    u16 cw_max;
    u8 aifsn;
    u8 fifos_mask;
    u16 edca_txop;                  /* 32 us units */
} __attribute__((packed));
struct iwl_mac_data_sta {
    u32 is_assoc;
    u32 dtim_time;
    u64 dtim_tsf;
    u32 bi;
    u32 reserved1;
    u32 dtim_interval;
    u32 data_policy;
    u32 listen_interval;
    u32 assoc_id;
    u32 assoc_beacon_arrive_time;
} __attribute__((packed));
struct iwl_mac_ctx_cmd {
    u32 id_and_color;
    u32 action;
    u32 mac_type;
    u32 tsf_id;
    u8 node_addr[6];
    u16 reserved_for_node_addr;
    u8 bssid_addr[6];
    u16 reserved_for_bssid_addr;
    u32 cck_rates;
    u32 ofdm_rates;
    u32 protection_flags;
    u32 cck_short_preamble;
    u32 short_slot;
    u32 filter_flags;
    u32 qos_flags;
    struct iwl_ac_qos ac[IWL_AC_NUM + 1];
    union {                         /* the largest of iwx's per-type data is 48 bytes (the AP's, the GO's) */
        struct iwl_mac_data_sta sta;
        u8 any[48];
    };
} __attribute__((packed));

#define IWL_MAX_MACS_IN_BINDING     3
struct iwl_binding_cmd {            /* version 2 */
    u32 id_and_color;
    u32 action;
    u32 macs[IWL_MAX_MACS_IN_BINDING];
    u32 phy;
    u32 lmac_id;
} __attribute__((packed));
struct iwl_cmd_response { u32 status; } __attribute__((packed));

/* ADD_STA (versions 10 to 12 share the layout) */
#define IWL_STATION_ID              0
#define IWL_STA_LINK                0
#define IWL_ADD_STA_SUCCESS         0x1
#define IWL_ADD_STA_STATUS_MASK     0xff
#define IWL_STA_FLG_FAT_EN_MSK      (3u << 26)
#define IWL_STA_FLG_MIMO_EN_MSK     (3u << 28)
struct iwl_add_sta_cmd {
    u8 add_modify;
    u8 awake_acs;
    u16 tid_disable_tx;
    u32 mac_id_n_color;
    u8 addr[6];
    u16 reserved2;
    u8 sta_id;
    u8 modify_mask;
    u16 reserved3;
    u32 station_flags;
    u32 station_flags_msk;
    u8 add_immediate_ba_tid;
    u8 remove_immediate_ba_tid;
    u16 add_immediate_ba_ssn;
    u16 sleep_tx_count;
    u8 sleep_state_flags;
    u8 station_type;
    u16 assoc_id;
    u16 beamform_flags;
    u32 tfd_queue_msk;
    u16 rx_ba_window;
    u8 sp_length;
    u8 uapsd_acs;
} __attribute__((packed));
struct iwl_rm_sta_cmd { u8 sta_id; u8 reserved[3]; } __attribute__((packed));

/* ADD_STA_KEY version 3 (if_iwxreg.h struct iwx_add_sta_key_cmd) */
#define IWL_STA_KEY_FLG_CCM         (2u << 0)
#define IWL_STA_KEY_FLG_WEP_KEY_MAP (1u << 3)
#define IWL_STA_KEY_FLG_KEYID_POS   8
#define IWL_STA_KEY_FLG_KEYID_MSK   (3u << 8)
#define IWL_STA_KEY_MULTICAST       (1u << 14)
struct iwl_add_sta_key_cmd {
    u8 sta_id;
    u8 key_offset;
    u16 key_flags;
    u8 key[32];
    u8 rx_secur_seq_cnt[16];
    u64 rx_mic_key;
    u64 tx_mic_key;
    u64 transmit_seq_cnt;
} __attribute__((packed));

/* SESSION_PROTECTION_CMD: the firmware stays on the channel while the station joins */
#define IWL_SESSION_PROTECT_CONF_ASSOC 0
struct iwl_session_prot_cmd {
    u32 id_and_color;
    u32 action;
    u32 conf_id;
    u32 duration_tu;
    u32 repetition_count;
    u32 interval;
} __attribute__((packed));

/* SCD_QUEUE_CONFIG_CMD version 3: a transmit queue for a station and TID; the response names it */
#define IWL_SCD_QUEUE_ADD           0
#define IWL_SCD_QUEUE_REMOVE        1
#define IWL_MGMT_TID                15          /* management and non-QoS data: one queue, as iwx */
struct iwl_scd_queue_cfg_cmd {
    u32 operation;
    union {
        struct {
            u32 sta_mask;
            u8 tid;
            u8 reserved[3];
            u32 flags;
            u32 cb_size;
            u64 bc_dram_addr;
            u64 tfdq_dram_addr;
        } __attribute__((packed)) add;
        struct {
            u32 sta_mask;
            u32 tid;
        } __attribute__((packed)) remove;
    } u;
} __attribute__((packed));
struct iwl_tx_queue_cfg_rsp {
    u16 queue_number;
    u16 flags;
    u16 write_pointer;
    u16 reserved;
} __attribute__((packed));

#define IWL_TFD_QUEUE_BC_SIZE       (256 + 64)  /* the byte count table's entries */
struct iwl_agn_scd_bc_tbl { u16 tfd_offset[IWL_TFD_QUEUE_BC_SIZE]; } __attribute__((packed));

/* TX_CMD for the 22000 family (if_iwxreg.h struct iwx_tx_cmd_gen2; versions 7 to 9), behind the
   4-byte command header; the 802.11 header follows it, then padding to 4 bytes */
#define IWL_TX_FLAGS_CMD_RATE       (1u << 0)   /* rate_n_flags is the rate (else the firmware's rate control) */
#define IWL_TX_FLAGS_ENCRYPT_DIS    (1u << 1)
#define IWL_TX_FLAGS_HIGH_PRI       (1u << 2)
#define IWL_TX_CMD_OFFLD_MH_SIZE(x) ((x) << 8)  /* the 802.11 header's length in 16-bit words */
#define IWL_TX_CMD_OFFLD_PAD        (1u << 13)
#define IWL_TX_CMD_OFFLD_MH_MASK    0x1f
struct iwl_tx_cmd_gen2 {
    u16 len;
    u16 offload_assist;
    u32 flags;
    u32 pn_low;                     /* struct iwx_dram_sec_info */
    u16 pn_high;
    u16 aux_info;
    u32 rate_n_flags;
    /* the 802.11 header */
} __attribute__((packed));
/* rate_n_flags, version 2 (the firmware's TX_CMD notification is version 7 or later) */
#define IWL_RATE_MCS_ANT_A_MSK      (1u << 14)
#define IWL_RATE_MCS_CCK_MSK        (0u << 8)
#define IWL_RATE_MCS_LEGACY_OFDM_MSK (1u << 8)
#define IWL_RATE_LEGACY_RATE_MSK    0x7

/* the response to a frame (if_iwxreg.h struct iwx_tx_resp, version 6 on); after the status of each
   frame, the queue's index up to which its frames are done */
#define IWL_TX_STATUS_MSK           0xff
#define IWL_TX_STATUS_SUCCESS       0x01
#define IWL_TX_STATUS_DIRECT_DONE   0x02
struct iwl_tx_resp {
    u8 frame_count;
    u8 bt_kill_count;
    u8 failure_rts;
    u8 failure_frame;
    u32 initial_rate;
    u16 wireless_media_time;
    u8 pa_status;
    u8 pa_integ_res_a[3];
    u8 pa_integ_res_b[3];
    u8 pa_integ_res_c[3];
    u16 measurement_req_id;
    u8 reduced_tpc;
    u8 reserved;
    u32 tfd_info;
    u16 seq_ctl;
    u16 byte_cnt;
    u8 tlc_info;
    u8 ra_tid;
    u16 frame_ctrl;
    u16 tx_queue;
    u16 reserved2;
    u16 status;                     /* struct iwx_agg_tx_status, one per frame */
    u16 sequence;
} __attribute__((packed));

struct iwl_tx_path_flush_cmd { u32 sta_id; u16 tid_mask; u16 reserved; } __attribute__((packed));

/* TLC_MNG_CONFIG_CMD version 4: the firmware's rate control for the station */
#define IWL_TLC_MNG_CH_WIDTH_20MHZ  0
#define IWL_TLC_MNG_MODE_NON_HT     0
#define IWL_TLC_MNG_CHAIN_A_MSK     (1u << 0)
struct iwl_tlc_config_cmd_v4 {
    u8 sta_id;
    u8 reserved1[3];
    u8 max_ch_width;
    u8 mode;
    u8 chains;
    u8 sgi_ch_width_supp;
    u16 flags;
    u16 non_ht_rates;               /* bit i: the i'th of 1 2 5.5 11 6 9 12 18 24 36 48 54 Mbps */
    u16 ht_rates[2][3];
    u16 max_mpdu_len;
    u16 max_tx_op;
} __attribute__((packed));

/* REPLY_SF_CFG_CMD (smart FIFO; if_iwxreg.h struct iwx_sf_cfg_cmd) */
#define IWL_SF_FULL_ON              1
#define IWL_SF_INIT_OFF             3
#define IWL_SF_W_MARK_LEGACY        4096
#define IWL_SF_W_MARK_SISO          4096
#define IWL_SF_W_MARK_MIMO2         8192
#define IWL_SF_W_MARK_SCAN          4096
#define IWL_SF_NUM_SCENARIO         5
#define IWL_SF_LONG_DELAY_AGING_TIMER 1000000
struct iwl_sf_cfg_cmd {
    u32 state;
    u32 watermark[2];
    u32 long_delay_timeouts[IWL_SF_NUM_SCENARIO][2];
    u32 full_on_timeouts[IWL_SF_NUM_SCENARIO][2];
} __attribute__((packed));

struct iwl_mcast_filter_cmd {
    u8 filter_own;
    u8 port_id;
    u8 count;
    u8 pass_all;
    u8 bssid[6];
    u8 reserved[2];
} __attribute__((packed));

#define IWL_POWER_KEEP_ALIVE_PERIOD_SEC 25
struct iwl_mac_power_cmd {
    u32 id_and_color;
    u16 flags;
    u16 keep_alive_seconds;
    u32 rx_data_timeout;
    u32 tx_data_timeout;
    u32 rx_data_timeout_uapsd;
    u32 tx_data_timeout_uapsd;
    u8 lprx_rssi_threshold;
    u8 skip_dtim_periods;
    u16 snooze_interval;
    u16 snooze_window;
    u8 snooze_step;
    u8 qndp_tid;
    u8 uapsd_ac_flags;
    u8 uapsd_max_sp;
    u8 heavy_tx_thld_packets;
    u8 heavy_rx_thld_packets;
    u8 heavy_tx_thld_percentage;
    u8 heavy_rx_thld_percentage;
    u8 limited_ps_threshold;
    u8 reserved;
} __attribute__((packed));

/* W3: an HT (802.11n) access point (if_iwxreg.h) */
#define IWL_PHY_VHT_CHANNEL_MODE40  1
#define IWL_PHY_VHT_CTRL_POS_1_ABOVE 4
#define IWL_MAC_QOS_FLG_TGN         (1u << 1)
#define IWL_MAC_PROT_FLG_HT_PROT    (1u << 23)
#define IWL_MAC_PROT_FLG_FAT_PROT   (1u << 24)
#define IWL_STA_FLG_MAX_AGG_SIZE_SHIFT 19
#define IWL_STA_FLG_MAX_AGG_SIZE_MSK (0xfu << 19)
#define IWL_STA_FLG_AGG_MPDU_DENS_SHIFT 23
#define IWL_STA_FLG_AGG_MPDU_DENS_MSK (7u << 23)
#define IWL_STA_FLG_FAT_EN_40MHZ    (1u << 26)
#define IWL_STA_FLG_MIMO_EN_MIMO2   (1u << 28)
#define IWL_TLC_MNG_MODE_HT         1
#define IWL_TLC_MNG_CH_WIDTH_40MHZ  1
#define IWL_TLC_MNG_CHAIN_B_MSK     (1u << 1)
#define IWL_TLC_MNG_CFG_FLAGS_STBC_MSK (1u << 0)

typedef char iwl_mac_ctx_cmd_size[sizeof(struct iwl_mac_ctx_cmd) == 4 * 4 + 16 + 7 * 4 + 5 * 8 + 48 ? 1 : -1];
typedef char iwl_add_sta_cmd_size[sizeof(struct iwl_add_sta_cmd) == 48 ? 1 : -1];
typedef char iwl_tx_resp_size[sizeof(struct iwl_tx_resp) == 44 ? 1 : -1];
typedef char iwl_tlc_config_size[sizeof(struct iwl_tlc_config_cmd_v4) == 28 ? 1 : -1];
typedef char iwl_sf_cfg_size[sizeof(struct iwl_sf_cfg_cmd) == 92 ? 1 : -1];

#endif
