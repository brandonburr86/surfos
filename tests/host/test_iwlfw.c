/* Host-side unit tests for driver/wifi/iwl_fw.c (10/10/26): the container of Intel's Wi-Fi
   firmware files. With the real files in firmware/ (tools/fetch-firmware.py) it parses both
   9560 images and prints what the driver will use; without them those checks are skipped.
   Broken files are built by hand and cut from the real one: a wrong magic, a short header, a
   record past the end, a section with no load address, too many sections, two command tables,
   and the real file cut at many lengths, none of which may point outside the file.
   Built like the kernel objects and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <surfos/types.h>
#include <sys/iwl_fw.h>

void exit(int status);
void *malloc(size_t size);
void free(void *p);
/* the host's file functions: SurfOS's stdio.h, which these tests see, has no FILE */
struct host_file;
struct host_file *fopen(const char *path, const char *mode);
int fseek(struct host_file *f, long off, int whence);
long ftell(struct host_file *f);
size_t fread(void *p, size_t size, size_t n, struct host_file *f);
int fclose(struct host_file *f);
#define HOST_SEEK_SET 0
#define HOST_SEEK_END 2

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

static u8 *load(const char *path, u_int *len) {
    struct host_file *f = fopen(path, "rb");
    u8 *buf;
    long n;
    if(!f) return NULL;
    fseek(f, 0, HOST_SEEK_END);
    n = ftell(f);
    fseek(f, 0, HOST_SEEK_SET);
    buf = (u8 *)malloc((size_t)n);
    if(buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *len = (u_int)n;
    return buf;
}

/* every section, and the image loader and the command table, inside [file, file + len) */
static int inside(const struct iwl_fw *fw, const u8 *file, u_int len) {
    u_int i;
    for(i = 0; i < fw->nsec; i++)
        if(fw->sec[i].data < file || fw->sec[i].data + fw->sec[i].len > file + len) return 0;
    if(fw->iml && (fw->iml < file || fw->iml + fw->iml_len > file + len)) return 0;
    if(fw->cmd_versions && ((const u8 *)fw->cmd_versions < file ||
                            (const u8 *)(fw->cmd_versions + fw->n_cmd_versions) > file + len)) return 0;
    return 1;
}

static u32 run_len(const struct iwl_fw *fw, u_int first, u_int count) {
    u32 n = 0;
    u_int i;
    for(i = first; i < first + count; i++) n += fw->sec[i].len;
    return n;
}

static void real_file(const char *path) {
    struct iwl_fw fw;
    const char *why = NULL;
    u_int len, i;
    u8 *file = load(path, &len);
    char what[160];
    int r;
    if(!file) {
        printf("skipped: %s (run tools/fetch-firmware.py)\n", path);
        return;
    }
    r = iwl_fw_parse(file, len, &fw, &why);
    snprintf(what, sizeof(what), "%s parses (%s)", path, why ? why : "ok");
    check(r == 0, what);
    if(r == 0) {
        printf("%s: %u bytes\n", path, len);
        printf("  name \"%s\", version %s\n", fw.name, fw.version);
        printf("  %u records (%u of other kinds skipped); %u runtime sections, %u bytes:\n", fw.ntlv, fw.nignored,
               fw.nsec, fw.total_len);
        printf("    first CPU (LMAC): %u sections, %u bytes\n", fw.lmac_count, run_len(&fw, fw.lmac_first, fw.lmac_count));
        printf("    second CPU (UMAC): %u sections, %u bytes\n", fw.umac_count, run_len(&fw, fw.umac_first, fw.umac_count));
        printf("    paged: %u sections, %u bytes\n", fw.paging_count, run_len(&fw, fw.paging_first, fw.paging_count));
        printf("  image loader: %u bytes; CPUs %u; scan channels %u; probe up to %u bytes\n", fw.iml_len, fw.num_cpu,
               fw.n_scan_channels, fw.max_probe_len);
        printf("  capabilities: LAR %d, UMAC scan %d, DQA %d, TLC offload %d, session protection %d\n",
               iwl_fw_has_capa(&fw, IWL_CAPA_LAR_SUPPORT), iwl_fw_has_capa(&fw, IWL_CAPA_UMAC_SCAN),
               iwl_fw_has_capa(&fw, IWL_CAPA_DQA_SUPPORT), iwl_fw_has_capa(&fw, IWL_CAPA_TLC_OFFLOAD),
               iwl_fw_has_capa(&fw, IWL_CAPA_SESSION_PROT_CMD));
        printf("  %u command versions; error logs at 0x%08x (UMAC), 0x%08x (LMAC)\n", fw.n_cmd_versions,
               fw.umac_error_table, fw.lmac_error_table);
        check(!strncmp(fw.version, "77.", 3), "firmware API 77, the version the driver is written for");
        check(fw.lmac_count > 0 && fw.umac_count > 0, "sections for both CPUs");
        check(fw.lmac_count + fw.umac_count + fw.paging_count + (fw.umac_count ? 1 : 0) + (fw.paging_count ? 1 : 0)
              == fw.nsec, "every section accounted for by the separators");
        check(fw.total_len < len, "the sections fit in the file");
        check(inside(&fw, file, len), "everything parsed points inside the file");
        check(iwl_fw_has_capa(&fw, IWL_CAPA_UMAC_SCAN), "the firmware scans itself (UMAC scan)");
        check(iwl_fw_has_capa(&fw, IWL_CAPA_TLC_OFFLOAD), "the firmware chooses the rates (TLC offload)");
        check(fw.n_cmd_versions > 0, "a table of command versions");
        for(i = 0; i < fw.nsec; i++) if(!fw.sec[i].len && fw.sec[i].offset != IWL_CPU1_CPU2_SEPARATOR &&
                                         fw.sec[i].offset != IWL_PAGING_SEPARATOR) break;
        check(i == fw.nsec, "no empty section other than a separator");
    }
    /* the real file cut short: an error, or a parse that stays inside what is left */
    for(i = 0; i < len; i += 997) {
        struct iwl_fw cut;
        r = iwl_fw_parse(file, i, &cut, &why);
        if(r == 0 && !inside(&cut, file, i)) { check(0, "a cut file's parse stays inside it"); break; }
    }
    check(1, "the real file cut at every 997th byte: no read past the end");
    free(file);
}

/**** hand-built files ****/

static u8 buf[4096];
static u_int used;

static void put32(u32 v) { buf[used++] = (u8)v; buf[used++] = (u8)(v >> 8); buf[used++] = (u8)(v >> 16); buf[used++] = (u8)(v >> 24); }

static void header(void) {
    memset(buf, 0, sizeof(buf));
    used = 0;
    put32(0);
    put32(IWL_TLV_UCODE_MAGIC);
    memcpy(buf + used, "SurfOS test image", 17);
    used += 64;
    put32(0x4d000100);                      /* 77.0.1 */
    put32(1);
    put32(0);
    put32(0);
}

static void tlv(u32 type, const u8 *data, u32 len) {
    put32(type);
    put32(len);
    if(data) memcpy(buf + used, data, len);
    used += (len + 3) & ~3u;
}

static void section(u32 offset, u32 len) {
    u8 d[64];
    memset(d, 0x5a, sizeof(d));
    d[0] = (u8)offset; d[1] = (u8)(offset >> 8); d[2] = (u8)(offset >> 16); d[3] = (u8)(offset >> 24);
    tlv(IWL_UCODE_TLV_SEC_RT, d, len + 4);
}

static void hand_built(void) {
    struct iwl_fw fw;
    const char *why;
    u8 v[12], cmds[8] = { 0x09, 0x00, 3, 0, 0x0c, 0x01, 15, 1 };   /* (group 0, 0x09) v3; (group 1, 0x0c) v15 */
    int i;

    header();
    section(0x00404000, 16);
    section(0x00406000, 8);
    section(IWL_CPU1_CPU2_SEPARATOR, 0);
    section(0x00456000, 32);
    section(IWL_PAGING_SEPARATOR, 0);
    section(0x00000000, 40);
    section(0x00001000, 40);
    v[0] = 1; v[1] = v[2] = v[3] = 0; v[4] = 0x04; v[5] = v[6] = v[7] = 0;   /* capability word 1: bit 2 (34) */
    tlv(IWL_UCODE_TLV_ENABLED_CAPABILITIES, v, 8);
    tlv(IWL_UCODE_TLV_CMD_VERSIONS, cmds, 8);
    tlv(0x1000005, v, 3);                   /* a debugging record, unpadded length: skipped */
    check(iwl_fw_parse(buf, used, &fw, &why) == 0, "a hand-built file parses");
    check(fw.nsec == 7 && fw.lmac_count == 2 && fw.umac_first == 3 && fw.umac_count == 1 &&
          fw.paging_first == 5 && fw.paging_count == 2, "the separators split the sections 2, 1, 2");
    check(fw.total_len == 16 + 8 + 32 + 40 + 40, "the runtime bytes, without the separators");
    check(!strcmp(fw.name, "SurfOS test image") && !strcmp(fw.version, "77.0.1"), "the name and the version");
    check(iwl_fw_has_capa(&fw, 34) && !iwl_fw_has_capa(&fw, 2) && !iwl_fw_has_capa(&fw, 1000), "capability bits");
    check(iwl_fw_cmd_version(&fw, 1, 0x0c) == 15 && iwl_fw_notif_version(&fw, 1, 0x0c) == 1 &&
          iwl_fw_cmd_version(&fw, 0, 0x09) == 3 && iwl_fw_cmd_version(&fw, 0, 0x0c) == IWL_FW_CMD_VER_UNKNOWN,
          "command versions by group and id");
    check(fw.nignored == 1, "the debugging record skipped");

    check(iwl_fw_parse(buf, 87, &fw, &why) != 0 && why, "a header cut short: an error with a reason");
    buf[4] ^= 1;
    check(iwl_fw_parse(buf, used, &fw, &why) != 0, "a wrong magic: an error");
    buf[4] ^= 1;
    buf[0] = 1;
    check(iwl_fw_parse(buf, used, &fw, &why) != 0, "a header of the old format: an error");
    buf[0] = 0;

    header();
    section(0x00404000, 16);
    put32(IWL_UCODE_TLV_SEC_RT);
    put32(4000);                            /* longer than what is left */
    check(iwl_fw_parse(buf, used, &fw, &why) != 0, "a record running past the end: an error");

    header();
    tlv(IWL_UCODE_TLV_SEC_RT, v, 2);        /* no room for the load address */
    check(iwl_fw_parse(buf, used, &fw, &why) != 0, "a section without its load address: an error");

    header();
    for(i = 0; i < IWL_FW_MAX_SECTIONS + 1; i++) section(0x1000 * (u32)i, 4);
    check(iwl_fw_parse(buf, used, &fw, &why) != 0, "more sections than the driver holds: an error");

    header();
    section(0x00404000, 16);
    tlv(IWL_UCODE_TLV_CMD_VERSIONS, cmds, 8);
    tlv(IWL_UCODE_TLV_CMD_VERSIONS, cmds, 8);
    check(iwl_fw_parse(buf, used, &fw, &why) != 0, "two command tables: an error");

    header();
    tlv(IWL_UCODE_TLV_N_SCAN_CHANNELS, v, 4);
    check(iwl_fw_parse(buf, used, &fw, &why) != 0, "no runtime section: an error");

    header();
    section(0x00404000, 16);
    section(IWL_PAGING_SEPARATOR, 0);
    section(0x0, 8);
    section(IWL_CPU1_CPU2_SEPARATOR, 0);    /* separators in the wrong order */
    section(0x0, 8);
    check(iwl_fw_parse(buf, used, &fw, &why) != 0, "sections after the paged ones: an error");
}

int main(void) {
    hand_built();
    real_file("firmware/iwlwifi-Qu-b0-jf-b0-77.ucode");
    real_file("firmware/iwlwifi-Qu-c0-jf-b0-77.ucode");
    printf("IWLFW UNIT TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    exit(failures ? 1 : 0);
    return 0;
}
