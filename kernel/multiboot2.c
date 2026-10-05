/*
SurfOS Multiboot 2 Information
--------------------
File: multiboot2.c  Date: 10/5/26 (USB boot)
--------------------
GRUB on UEFI hands the kernel a Multiboot 2 information block: its total size, a
reserved word, then tags (type, size, body) padded to 8 bytes and closed by an end
tag. Every read here is checked against the byte count the caller passes, so a
truncated or corrupt block is rejected instead of being read past its end. This is
pure code with no console or memory side effects: tests/host/test_bootinfo.c runs
it on the host.
*/

#include <surfos/types.h>
#include <surfos/multiboot.h>
#include <blibc_common.h>

static u32 rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static u64 rd64(const u8 *p) { u64 v; memcpy(&v, p, 8); return v; }

static u8 sum8(const u8 *p, u32 n) {
    u8 s = 0;
    while(n--) s += *p++;
    return s;
}

/* a NUL-terminated string that has to end inside its tag */
static int take_string(char *dst, size_t size, const u8 *s, u32 room) {
    u32 n;
    for(n = 0; n < room; n++) if(!s[n]) break;
    if(n == room) return MB2_E_STRING;
    if(dst) strlcpy(dst, (const char *)s, size);
    return MB2_OK;
}

/* the loader's copy of the ACPI root pointer; a bad copy is ignored, not fatal */
static void take_rsdp(struct boot_info *bi, const u8 *p, u32 len, bool v2) {
    struct boot_acpi a;
    if(len < 20 || memcmp(p, "RSD PTR ", 8) || sum8(p, 20)) return;
    memset(&a, 0, sizeof(a));
    a.revision = p[15];
    memcpy(a.oem, p + 9, 6);
    a.rsdt = rd32(p + 16);
    if(v2) {
        u32 length;
        if(len < 36) return;
        length = rd32(p + 20);
        if(length < 36 || length > len || sum8(p, length)) return;
        if(a.revision >= 2) a.xsdt = rd64(p + 24);
    } else if(bi->acpi.present && bi->acpi.xsdt) {
        return;                                      /* keep the ACPI 2.0 pointer already taken */
    }
    a.present = true;
    bi->acpi = a;
}

static int take_mmap(struct boot_info *bi, const u8 *t, u32 size) {
    u32 esize, off;
    if(size < 16) return MB2_E_TAGSIZE;
    esize = rd32(t + 8);
    if(esize < 24 || (esize & 7) || (size - 16) % esize) return MB2_E_MMAP;
    for(off = 16; off < size; off += esize) {
        u64 base = rd64(t + off), len = rd64(t + off + 8);
        u32 type = rd32(t + off + 16);
        if(!len) continue;
        if(base + len < base) return MB2_E_MMAP;     /* wraps past 2^64 */
        if(bi->nregions >= MB_MAX_REGIONS) return MB2_E_TOOMANY;
        bi->regions[bi->nregions].base = base;
        bi->regions[bi->nregions].len = len;
        bi->regions[bi->nregions].type = type;
        bi->nregions++;
    }
    return MB2_OK;
}

static int take_tag(struct boot_info *bi, u32 type, const u8 *t, u32 size) {
    int r;
    switch(type) {
    case MB2_TAG_CMDLINE:
        return take_string(bi->cmdline, sizeof(bi->cmdline), t + 8, size - 8);
    case MB2_TAG_LOADER_NAME:
        return take_string(bi->loader, sizeof(bi->loader), t + 8, size - 8);
    case MB2_TAG_MODULE: {
        u32 start, end;
        if(size < 17) return MB2_E_TAGSIZE;          /* start, end and at least a terminator */
        start = rd32(t + 8);
        end = rd32(t + 12);
        if(end < start) return MB2_E_MODULE;
        r = take_string(NULL, 0, t + 16, size - 16);
        if(r) return r;
        if(bi->nmods >= MB_MAX_MODULES) {            /* reported by mb_print(), never used */
            bi->mods_dropped++;
            return MB2_OK;
        }
        bi->mods[bi->nmods].start = start;
        bi->mods[bi->nmods].end = end;
        strlcpy(bi->mods[bi->nmods].name, (const char *)(t + 16), sizeof(bi->mods[0].name));
        bi->nmods++;
        return MB2_OK;
    }
    case MB2_TAG_BASIC_MEMINFO:
        if(size < 16) return MB2_E_TAGSIZE;
        bi->mem_lower_kb = rd32(t + 8);
        bi->mem_upper_kb = rd32(t + 12);
        return MB2_OK;
    case MB2_TAG_MMAP:
        return take_mmap(bi, t, size);
    case MB2_TAG_FRAMEBUFFER:
        if(size < 32) return MB2_E_TAGSIZE;
        memset(&bi->fb, 0, sizeof(bi->fb));
        bi->fb.addr = rd64(t + 8);
        bi->fb.pitch = rd32(t + 16);
        bi->fb.width = rd32(t + 20);
        bi->fb.height = rd32(t + 24);
        bi->fb.bpp = t[28];
        bi->fb.type = t[29];
        if(bi->fb.type == MB_FB_RGB) {               /* GRUB: the colour fields follow a 16-bit reserved word */
            if(size < 38) return MB2_E_TAGSIZE;
            bi->fb.red_pos = t[32];   bi->fb.red_size = t[33];
            bi->fb.green_pos = t[34]; bi->fb.green_size = t[35];
            bi->fb.blue_pos = t[36];  bi->fb.blue_size = t[37];
        }
        bi->fb.present = true;
        return MB2_OK;
    case MB2_TAG_EFI32:
        if(size < 12) return MB2_E_TAGSIZE;
        bi->efi = true;
        bi->efi_systab = rd32(t + 8);
        return MB2_OK;
    case MB2_TAG_EFI64:
        if(size < 16) return MB2_E_TAGSIZE;
        bi->efi = true;
        bi->efi_systab = rd64(t + 8);
        return MB2_OK;
    case MB2_TAG_ACPI_OLD:
        take_rsdp(bi, t + 8, size - 8, false);
        return MB2_OK;
    case MB2_TAG_ACPI_NEW:
        take_rsdp(bi, t + 8, size - 8, true);
        return MB2_OK;
    case MB2_TAG_EFI_BS:
        bi->efi_bs = true;
        return MB2_OK;
    default:
        return MB2_OK;                               /* optional tags SurfOS does not use */
    }
}

static int parse(const u8 *p, u32 bytes, struct boot_info *out) {
    u32 total;
    u64 off;
    if(!p || bytes < 16) return MB2_E_ENVELOPE;      /* size, reserved word and an end tag */
    total = rd32(p);
    if(total < 16 || total > bytes) return MB2_E_ENVELOPE;
    off = 8;
    while(off < total) {                             /* tags start 8-byte aligned by construction */
        const u8 *t = p + off;
        u32 type, size;
        int r;
        if(total - off < 8) return MB2_E_TAG;
        type = rd32(t);
        size = rd32(t + 4);
        if(size < 8) return MB2_E_TAGSIZE;
        if(size > total - off) return MB2_E_TAG;
        if(type == MB2_TAG_END) return size == 8 ? MB2_OK : MB2_E_TAGSIZE;
        r = take_tag(out, type, t, size);
        if(r) return r;
        off += ((u64)size + 7) & ~7ULL;
    }
    return MB2_E_NOEND;
}

int mb2_parse(const void *data, u32 bytes, struct boot_info *out) {
    int r;
    memset(out, 0, sizeof(*out));
    r = parse((const u8 *)data, bytes, out);
    if(r) {                                          /* nothing half-parsed survives */
        memset(out, 0, sizeof(*out));
        out->error = r;
        return r;
    }
    out->valid = true;
    out->protocol = 2;
    return MB2_OK;
}

const char *mb2_strerror(int err) {
    switch(err) {
    case MB2_OK: return "ok";
    case MB2_E_ENVELOPE: return "bad total size";
    case MB2_E_TAG: return "a tag runs past the end of the block";
    case MB2_E_TAGSIZE: return "a tag has an impossible size";
    case MB2_E_NOEND: return "no end tag";
    case MB2_E_STRING: return "an unterminated string";
    case MB2_E_MMAP: return "malformed memory map";
    case MB2_E_TOOMANY: return "more memory map entries than the kernel holds";
    case MB2_E_MODULE: return "a module ends before it starts";
    default: return "unknown error";
    }
}

/* exact word match: "profile=laptop" in "profile=laptop kbd=poll" */
bool bootinfo_has_arg(const struct boot_info *bi, const char *arg) {
    const char *s = bi->cmdline;
    size_t n = strlen(arg);
    while(*s) {
        const char *w;
        while(*s == ' ') s++;
        w = s;
        while(*s && *s != ' ') s++;
        if((size_t)(s - w) == n && n && !strncmp(w, arg, n)) return true;
    }
    return false;
}

const char *mb_region_type(u32 type) {
    switch(type) {
    case MB_MEMORY_AVAILABLE: return "available";
    case MB_MEMORY_ACPI_RECLAIMABLE: return "ACPI tables";
    case MB_MEMORY_NVS: return "ACPI NVS";
    case MB_MEMORY_BADRAM: return "bad RAM";
    default: return "reserved";
    }
}
