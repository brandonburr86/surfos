/*
SurfOS Wi-Fi: Intel firmware files
--------------------
File: iwl_fw.c  Date: 10/10/26 (the Wi-Fi brief: docs/superpowers/plans/2026-10-09-wifi.md)
--------------------
Reads the container of an Intel Wi-Fi firmware file (include/sys/iwl_fw.h): the header's
name and version, then the type-length-value records the driver needs: the runtime image's
sections (each its load address and bytes), the separators that split them into the first
CPU's, the second CPU's and the paged ones, the API and capability bits, the versions of the
firmware's commands, the number of scan channels, and where the firmware keeps its error logs.
Everything else is skipped. As the open-source drivers do, and no further: the sections are
handed to the card as they are, never looked into (Intel's licence forbids it).

No I/O and no allocation: the parse points into the file, which the caller keeps.
tests/host/test_iwlfw.c runs it on the host against the real files (tools/fetch-firmware.py)
and broken ones. Every length is checked against what is left of the file before it is used.

The layout and the record types follow OpenBSD's if_iwx.c (iwx_read_firmware()) and
if_iwxreg.h, which follow Linux's iwlwifi; see the notice in include/sys/iwl_fw.h.
*/

#include <surfos/types.h>
#include <sys/iwl_fw.h>
#include <fs/vfs.h>
#include <blibc_common.h>

#define DEFAULT_SCAN_CHANNELS 40        /* if_iwx.c IWX_DEFAULT_SCAN_CHANNELS, when the file does not say */

static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static bool is_separator(u32 offset) { return offset == IWL_CPU1_CPU2_SEPARATOR || offset == IWL_PAGING_SEPARATOR; }

/* how many sections from first on, up to a separator or the end (if_iwx.c iwx_get_num_sections()) */
static u_int run_of(const struct iwl_fw *fw, u_int first) {
    u_int n = 0;
    while(first + n < fw->nsec && !is_separator(fw->sec[first + n].offset)) n++;
    return n;
}

static int fail(const char **why, const char *what) {
    if(why) *why = what;
    return -EINVAL;
}

/* a 32-bit value record: exactly 4 bytes */
static int value(const u8 *data, u32 len, u32 *out, const char **why, const char *what) {
    if(len != 4) return fail(why, what);
    *out = rd32(data);
    return 0;
}

int iwl_fw_parse(const u8 *file, u_int len, struct iwl_fw *fw, const char **why) {
    const u8 *p;
    u_int left, i, n;
    u32 major, minor, api;

    memset(fw, 0, sizeof(*fw));
    if(why) *why = NULL;
    if(!file || len < IWL_UCODE_HEADER_LEN) return fail(why, "shorter than a firmware file's header");
    if(rd32(file) != 0 || rd32(file + 4) != IWL_TLV_UCODE_MAGIC)
        return fail(why, "not an Intel firmware file in the TLV format");
    for(i = 0; i < 64 && file[8 + i]; i++)
        fw->name[i] = (file[8 + i] >= 32 && file[8 + i] < 127) ? (char)file[8 + i] : '?';
    fw->name[i] = 0;
    fw->ver = rd32(file + 72);
    snprintf(fw->version, sizeof(fw->version), "%u.%u.%u", IWL_UCODE_MAJOR(fw->ver), IWL_UCODE_MINOR(fw->ver),
             IWL_UCODE_API(fw->ver));
    fw->n_scan_channels = DEFAULT_SCAN_CHANNELS;

    p = file + IWL_UCODE_HEADER_LEN;
    left = len - IWL_UCODE_HEADER_LEN;
    while(left >= 8) {
        u32 type = rd32(p), tlen = rd32(p + 4), padded;
        const u8 *data = p + 8;
        int r = 0;
        left -= 8;
        if(tlen > left) return fail(why, "a record runs past the end of the file");
        switch(type) {
        case IWL_UCODE_TLV_SEC_RT:
            if(tlen < 4) return fail(why, "a section without its load address");
            if(fw->nsec >= IWL_FW_MAX_SECTIONS) return fail(why, "more sections than the driver can hold");
            fw->sec[fw->nsec].offset = rd32(data);
            fw->sec[fw->nsec].data = data + 4;
            fw->sec[fw->nsec].len = tlen - 4;
            if(!is_separator(fw->sec[fw->nsec].offset)) fw->total_len += tlen - 4;
            fw->nsec++;
            break;
        case IWL_UCODE_TLV_API_CHANGES_SET:
        case IWL_UCODE_TLV_ENABLED_CAPABILITIES:
            if(tlen != 8) return fail(why, "an API or capability record of the wrong size");
            n = rd32(data);
            if(n >= IWL_FW_API_WORDS) return fail(why, "an API or capability record past the bits known");
            if(type == IWL_UCODE_TLV_API_CHANGES_SET) fw->api[n] |= rd32(data + 4);
            else fw->capa[n] |= rd32(data + 4);
            break;
        case IWL_UCODE_TLV_CMD_VERSIONS:
            if(fw->cmd_versions) return fail(why, "two tables of command versions");
            fw->cmd_versions = (const struct iwl_fw_cmd_version *)data;   /* bytes only: no alignment needed */
            fw->n_cmd_versions = tlen / sizeof(struct iwl_fw_cmd_version);
            break;
        case IWL_UCODE_TLV_FW_VERSION:
            if(tlen != 12) return fail(why, "a version record of the wrong size");
            major = rd32(data);
            minor = rd32(data + 4);
            api = rd32(data + 8);
            if(major >= 35) snprintf(fw->version, sizeof(fw->version), "%u.%08x.%u", major, minor, api);
            else snprintf(fw->version, sizeof(fw->version), "%u.%u.%u", major, minor, api);
            break;
        case IWL_UCODE_TLV_NUM_OF_CPU:
            r = value(data, tlen, &fw->num_cpu, why, "a CPU count of the wrong size");
            if(!r && (fw->num_cpu < 1 || fw->num_cpu > 2)) return fail(why, "a CPU count other than 1 or 2");
            break;
        case IWL_UCODE_TLV_N_SCAN_CHANNELS:
            r = value(data, tlen, &fw->n_scan_channels, why, "a scan channel count of the wrong size");
            break;
        case IWL_UCODE_TLV_PROBE_MAX_LEN:
            r = value(data, tlen, &fw->max_probe_len, why, "a probe length of the wrong size");
            break;
        case IWL_UCODE_TLV_PHY_SKU:
            r = value(data, tlen, &fw->phy_sku, why, "a PHY SKU of the wrong size");
            break;
        case IWL_UCODE_TLV_FLAGS:
            if(tlen < 4) return fail(why, "a flags record too short");
            fw->flags = rd32(data);                 /* the first word only, as the other drivers read it */
            break;
        case IWL_UCODE_TLV_IML:
            fw->iml = data;
            fw->iml_len = tlen;
            break;
        case IWL_UCODE_TLV_UMAC_DEBUG_ADDRS:
            if(tlen < 4) return fail(why, "a debug address record too short");
            fw->umac_error_table = rd32(data) & ~IWL_FW_ADDR_CACHE_CONTROL;
            break;
        case IWL_UCODE_TLV_LMAC_DEBUG_ADDRS:
            if(tlen < 4) return fail(why, "a debug address record too short");
            fw->lmac_error_table = rd32(data) & ~IWL_FW_ADDR_CACHE_CONTROL;
            break;
        default:                                    /* other images, debugging, records of newer files */
            fw->nignored++;
            break;
        }
        if(r) return r;
        fw->ntlv++;
        padded = (tlen + 3) & ~3u;
        if(padded < tlen || padded > left) padded = left;   /* the last record need not be padded */
        p += 8 + padded;
        left -= padded;
    }

    /* the runtime image: the first CPU's sections, a separator, the second CPU's, a separator,
       the paged ones (if_iwx.c iwx_ctxt_info_init_fw_sec()) */
    fw->lmac_first = 0;
    fw->lmac_count = run_of(fw, 0);
    i = fw->lmac_count;
    if(i < fw->nsec && fw->sec[i].offset == IWL_CPU1_CPU2_SEPARATOR) {
        fw->umac_first = i + 1;
        fw->umac_count = run_of(fw, i + 1);
        i = fw->umac_first + fw->umac_count;
    } else {
        fw->umac_first = i;
    }
    if(i < fw->nsec && fw->sec[i].offset == IWL_PAGING_SEPARATOR) {
        fw->paging_first = i + 1;
        fw->paging_count = run_of(fw, i + 1);
        i = fw->paging_first + fw->paging_count;
    } else {
        fw->paging_first = i;
    }
    if(!fw->lmac_count) return fail(why, "no runtime sections");
    if(i != fw->nsec) return fail(why, "runtime sections after the paged ones");
    return 0;
}

bool iwl_fw_has_capa(const struct iwl_fw *fw, u_int bit) {
    return bit < 32 * IWL_FW_API_WORDS && (fw->capa[bit / 32] & (1u << (bit % 32)));
}

bool iwl_fw_has_api(const struct iwl_fw *fw, u_int bit) {
    return bit < 32 * IWL_FW_API_WORDS && (fw->api[bit / 32] & (1u << (bit % 32)));
}

u_int iwl_fw_cmd_version(const struct iwl_fw *fw, u8 group, u8 cmd) {
    u_int i;
    for(i = 0; i < fw->n_cmd_versions; i++)
        if(fw->cmd_versions[i].group == group && fw->cmd_versions[i].cmd == cmd) return fw->cmd_versions[i].cmd_ver;
    return IWL_FW_CMD_VER_UNKNOWN;
}

u_int iwl_fw_notif_version(const struct iwl_fw *fw, u8 group, u8 cmd) {
    u_int i;
    for(i = 0; i < fw->n_cmd_versions; i++)
        if(fw->cmd_versions[i].group == group && fw->cmd_versions[i].cmd == cmd) return fw->cmd_versions[i].notif_ver;
    return IWL_FW_CMD_VER_UNKNOWN;
}
