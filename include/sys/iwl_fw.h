/*
SurfOS Wi-Fi: Intel firmware files
----------------------
File: iwl_fw.h  Date: 10/10/26
----------------------
The container of Intel's Wi-Fi firmware files (iwlwifi-*.ucode, fetched by
tools/fetch-firmware.py): a header, then type-length-value records. driver/wifi/iwl_fw.c reads
it into struct iwl_fw (no copy: pointers into the file), as the open-source drivers do, to hand
the firmware's sections to the card. Nothing looks inside the sections themselves: Intel's
licence (LICENCE.iwlwifi_firmware) forbids reverse engineering the firmware.

The record types, the header's layout, the section separators and the command version entry
are those of OpenBSD's sys/dev/pci/if_iwxreg.h, which takes them from Linux's iwlwifi; that file
carries the following notice, kept here as its BSD licence asks:

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

#ifndef _SURFOS_IWL_FW_H
#define _SURFOS_IWL_FW_H

#include <surfos/types.h>

#define IWL_TLV_UCODE_MAGIC     0x0a4c5749      /* "IWL\n" */
#define IWL_UCODE_HEADER_LEN    88              /* zero, magic, human_readable[64], ver, build, 8 ignored */

/* the record types this parser uses or accepts (if_iwxreg.h) */
#define IWL_UCODE_TLV_PROBE_MAX_LEN          6
#define IWL_UCODE_TLV_PAN                    7
#define IWL_UCODE_TLV_FLAGS                  18
#define IWL_UCODE_TLV_SEC_RT                 19
#define IWL_UCODE_TLV_SEC_INIT               20
#define IWL_UCODE_TLV_SEC_WOWLAN             21
#define IWL_UCODE_TLV_DEF_CALIB              22
#define IWL_UCODE_TLV_PHY_SKU                23
#define IWL_UCODE_TLV_NUM_OF_CPU             27
#define IWL_UCODE_TLV_CSCHEME                28
#define IWL_UCODE_TLV_API_CHANGES_SET        29
#define IWL_UCODE_TLV_ENABLED_CAPABILITIES   30
#define IWL_UCODE_TLV_N_SCAN_CHANNELS        31
#define IWL_UCODE_TLV_PAGING                 32
#define IWL_UCODE_TLV_SEC_RT_USNIFFER        34
#define IWL_UCODE_TLV_FW_VERSION             36
#define IWL_UCODE_TLV_CMD_VERSIONS           48
#define IWL_UCODE_TLV_IML                    52
#define IWL_UCODE_TLV_UMAC_DEBUG_ADDRS       54
#define IWL_UCODE_TLV_LMAC_DEBUG_ADDRS       55

/* separators among a runtime image's sections: the second CPU's sections, then the paged ones */
#define IWL_CPU1_CPU2_SEPARATOR  0xFFFFCCCC
#define IWL_PAGING_SEPARATOR     0xAAAABBBB

#define IWL_FW_ADDR_CACHE_CONTROL 0xC0000000   /* masked off the debug table addresses */

#define IWL_UCODE_MAJOR(v) (((v) >> 24) & 0xFF)
#define IWL_UCODE_MINOR(v) (((v) >> 16) & 0xFF)
#define IWL_UCODE_API(v)   (((v) >> 8) & 0xFF)

/* one entry of IWL_UCODE_TLV_CMD_VERSIONS (if_iwxreg.h struct iwx_fw_cmd_version) */
struct iwl_fw_cmd_version {
    u8 cmd;
    u8 group;
    u8 cmd_ver;
    u8 notif_ver;
};

#define IWL_FW_MAX_SECTIONS 64          /* the runtime image's sections, separators included */
#define IWL_FW_API_WORDS    4           /* 128 API flags, 128 capabilities */

struct iwl_fw_section {
    u32 offset;                         /* where the card puts it (its own address space) */
    const u8 *data;                     /* in the file */
    u32 len;
};

/* a parsed firmware file: everything points into the file, which must stay */
struct iwl_fw {
    char name[65];                      /* the header's human-readable text */
    u32 ver;                            /* the header's major/minor/API/serial */
    char version[32];                   /* major.minor.api (from FW_VERSION when present) */
    struct iwl_fw_section sec[IWL_FW_MAX_SECTIONS];   /* the runtime image (SEC_RT), in file order */
    u_int nsec;
    u_int lmac_first, lmac_count;       /* the first CPU's sections (up to the CPU separator) */
    u_int umac_first, umac_count;       /* the second CPU's (up to the paging separator) */
    u_int paging_first, paging_count;   /* the paged ones (the card fetches them from host memory) */
    u32 total_len;                      /* bytes in the runtime sections */
    const u8 *iml;                      /* the image loader, when the file has one */
    u32 iml_len;
    u32 api[IWL_FW_API_WORDS];          /* API_CHANGES_SET bits */
    u32 capa[IWL_FW_API_WORDS];         /* ENABLED_CAPABILITIES bits */
    u32 num_cpu;
    u32 n_scan_channels;
    u32 max_probe_len;
    u32 phy_sku;
    u32 flags;
    u32 umac_error_table;               /* SRAM addresses of the error logs (for a crash report) */
    u32 lmac_error_table;
    const struct iwl_fw_cmd_version *cmd_versions;
    u_int n_cmd_versions;
    u_int ntlv, nignored;               /* records read, records of other types skipped */
};

/* capability bits used by the driver (if_iwxreg.h IWX_UCODE_TLV_CAPA_*) */
#define IWL_CAPA_LAR_SUPPORT        1
#define IWL_CAPA_UMAC_SCAN          2
#define IWL_CAPA_DQA_SUPPORT        12
#define IWL_CAPA_TLC_OFFLOAD        43
#define IWL_CAPA_SESSION_PROT_CMD   54

/* 0, or a negative errno with *why saying what is wrong with the file */
int iwl_fw_parse(const u8 *file, u_int len, struct iwl_fw *fw, const char **why);
bool iwl_fw_has_capa(const struct iwl_fw *fw, u_int bit);
bool iwl_fw_has_api(const struct iwl_fw *fw, u_int bit);
/* the version the firmware wants of a command, IWL_FW_CMD_VER_UNKNOWN when it does not say */
#define IWL_FW_CMD_VER_UNKNOWN 99
u_int iwl_fw_cmd_version(const struct iwl_fw *fw, u8 group, u8 cmd);
u_int iwl_fw_notif_version(const struct iwl_fw *fw, u8 group, u8 cmd);

#endif
