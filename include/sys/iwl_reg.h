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

#endif
