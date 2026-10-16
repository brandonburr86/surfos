/*
SurfBoot: SURFOS.BIN packages
--------------------
File: image.c Date: 10/15/26 (SurfCPU)
--------------------
See image.h. The payload is copied one sector at a time through a 512-byte buffer in
scratch RAM (it starts 64 bytes into the file, so sectors do not line up with the load
address), with the CRC-32 updated as it goes; the whole payload is then read back from DDR
and checked again before anything may run.
*/
#include <string.h>
#include "image.h"
#include "crc32.h"
#include "port.h"
#include "sberr.h"

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void img_isa_string(uint32_t isa, char *buf, unsigned size)
{
    fmt_snprintf(buf, size, "%s%s%s%s%s", (isa & SURF_ISA_I) ? "I" : "", (isa & SURF_ISA_M) ? "M" : "",
                 (isa & SURF_ISA_ZICSR) ? "_Zicsr" : "", (isa & SURF_ISA_ZIFENCEI) ? "_Zifencei" : "",
                 (isa & ~SURFBOOT_ISA) ? "+?" : "");
}

static const char *magic_hint(const uint8_t *b)
{
    if (b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F')
        return ": an ELF file (install the packed SURFOS.BIN, not surfos.elf or the x86 kernel)";
    if (b[0] == 0xEB || b[0] == 0xFA || b[0] == 0x33)
        return ": it looks like x86 boot code (the x86 surfos.bin or a disk image is not a RISC-V kernel)";
    return " (pack the kernel with tools/surfimg.py)";
}

int img_check_header(const uint8_t *b, uint32_t file_size, uint32_t ram_base, uint32_t ram_limit,
                     struct surf_img_header *h)
{
    if (file_size < SURF_IMAGE_HEADER_SIZE)
        return sb_fail(SB_E_IMG_SHORT, "the file is %u bytes, shorter than the %u-byte header", file_size,
                       (unsigned)SURF_IMAGE_HEADER_SIZE);
    h->magic = rd32(b + 0x00);
    h->version = (uint16_t)rd16(b + 0x04);
    h->header_size = (uint16_t)rd16(b + 0x06);
    h->isa = rd32(b + 0x08);
    h->platform = rd32(b + 0x0C);
    h->load_addr = rd32(b + 0x10);
    h->payload_size = rd32(b + 0x14);
    h->mem_size = rd32(b + 0x18);
    h->entry = rd32(b + 0x1C);
    h->payload_crc32 = rd32(b + 0x20);
    h->flags = rd32(b + 0x24);
    h->image_version = rd32(b + 0x28);
    h->build_id = rd32(b + 0x2C);
    memcpy(h->name, b + 0x30, sizeof h->name);
    h->header_crc32 = rd32(b + 0x3C);

    /* 1 */
    if (h->magic != SURF_IMAGE_MAGIC)
        return sb_fail(SB_E_IMG_MAGIC, "no SIMG magic: found %02x %02x %02x %02x%s", b[0], b[1], b[2], b[3],
                       magic_hint(b));
    if (h->version != SURF_IMAGE_HEADER_VERSION || h->header_size != SURF_IMAGE_HEADER_SIZE)
        return sb_fail(SB_E_IMG_VERSION, "header version %u, size %u: this SurfBoot reads version %u, size %u",
                       h->version, h->header_size, (unsigned)SURF_IMAGE_HEADER_VERSION,
                       (unsigned)SURF_IMAGE_HEADER_SIZE);
    uint32_t hcrc = crc32(b, 0x3C);
    if (hcrc != h->header_crc32)
        return sb_fail(SB_E_IMG_HCRC, "the header CRC is 0x%08x but the header says 0x%08x (a damaged header)",
                       hcrc, h->header_crc32);
    /* 2 */
    if (!(h->isa & SURF_ISA_I))
        return sb_fail(SB_E_IMG_ISA, "the ISA bits 0x%08x do not include I (SURF_ISA_I): every SurfCPU program "
                       "needs the base ISA", h->isa);
    if (h->isa & ~SURFBOOT_ISA)
        return sb_fail(SB_E_IMG_ISA, "the payload needs ISA bits 0x%08x; SurfCPU has I, M, Zicsr and "
                       "Zifencei (0x%02x)", h->isa & ~SURFBOOT_ISA, (unsigned)SURFBOOT_ISA);
    if (h->platform != SURF_PLATFORM_ABI)
        return sb_fail(SB_E_IMG_PLATFORM, "platform ABI %u is not this board's (%u)", h->platform,
                       (unsigned)SURF_PLATFORM_ABI);
    /* 3 */
    if (h->payload_size == 0)
        return sb_fail(SB_E_IMG_SIZE, "the payload size is 0");
    if (h->payload_size > 0xFFFFFFFFu - SURF_IMAGE_HEADER_SIZE ||
        file_size != SURF_IMAGE_HEADER_SIZE + h->payload_size)
        return sb_fail(SB_E_IMG_SIZE, "the file is %u bytes but the header says 64 + %u: %s", file_size,
                       h->payload_size,
                       file_size < SURF_IMAGE_HEADER_SIZE + h->payload_size ? "truncated" : "extra bytes");
    /* 4 */
    if (h->load_addr & 3u)
        return sb_fail(SB_E_IMG_ALIGN, "the load address 0x%08x is not 4-byte aligned", h->load_addr);
    if (h->mem_size < h->payload_size)
        return sb_fail(SB_E_IMG_RANGE, "mem_size %u is smaller than payload_size %u", h->mem_size,
                       h->payload_size);
    uint64_t end = (uint64_t)h->load_addr + h->mem_size;
    if (h->load_addr < ram_base || end > ram_limit)
        return sb_fail(SB_E_IMG_RANGE, "the image would occupy [0x%08x, 0x%08x%s), outside usable DDR "
                       "[0x%08x, 0x%08x) (the top 16 MiB of DDR, ddr_top, is reserved)", h->load_addr,
                       (uint32_t)end, end > 0xFFFFFFFFull ? "+4G" : "", ram_base, ram_limit);
    /* 5 */
    if (h->entry & 3u)
        return sb_fail(SB_E_IMG_ENTRY, "the entry point 0x%08x is not 4-byte aligned", h->entry);
    if (h->entry < h->load_addr || h->entry - h->load_addr >= h->payload_size)
        return sb_fail(SB_E_IMG_ENTRY, "the entry point 0x%08x is outside the payload [0x%08x, 0x%08x)",
                       h->entry, h->load_addr, h->load_addr + h->payload_size);
    return SB_OK;
}

int img_load(struct fat_vol *v, const struct fat_dirent *d, uint32_t ram_base, uint32_t ram_limit,
             struct img_info *out, img_progress_fn progress)
{
    memset(out, 0, sizeof *out);
    out->file_size = d->size;
    struct fat_file f;
    int e = fat_open(v, d, &f);
    if (e)
        return e;
    if (d->size < SURF_IMAGE_HEADER_SIZE)
        return sb_fail(SB_E_IMG_SHORT, "the file is %u bytes, shorter than the %u-byte header", d->size,
                       (unsigned)SURF_IMAGE_HEADER_SIZE);
    static uint8_t buf[512] __attribute__((aligned(4)));
    uint32_t len;
    uint32_t t0 = port_now_us();
    e = fat_read_sector(v, &f, buf, &len);
    if (e)
        return e;
    if (len < SURF_IMAGE_HEADER_SIZE)
        return sb_fail(SB_E_IMG_SHORT, "only %u bytes could be read", len);
    e = img_check_header(buf, d->size, ram_base, ram_limit, &out->h);
    if (e)
        return e;

    const struct surf_img_header *h = &out->h;
    uint8_t *dst = port_mem(h->load_addr, h->payload_size);
    uint32_t done = 0, crc = 0, next_report = 65536u;
    const uint8_t *p = buf + SURF_IMAGE_HEADER_SIZE;
    uint32_t n = len - SURF_IMAGE_HEADER_SIZE;
    for (;;) {
        if (n > h->payload_size - done)
            n = h->payload_size - done;
        memcpy(dst + done, p, n);
        crc = crc32_update(crc, p, n);
        done += n;
        if (done == h->payload_size)
            break;
        if (progress && done >= next_report) {
            progress(done, h->payload_size);
            next_report += 65536u;
        }
        e = fat_read_sector(v, &f, buf, &len);
        if (e)
            return e;
        if (len == 0)
            return sb_fail(SB_E_IMG_SHORT, "the file ended after %u of %u payload bytes", done,
                           h->payload_size);
        p = buf;
        n = len;
    }
    out->fragments = f.fragments;
    out->crc_card = crc;
    if (crc != h->payload_crc32) {
        out->load_us = port_now_us() - t0;
        return sb_fail(SB_E_IMG_CRC, "the payload CRC as read from the card is 0x%08x, the header says "
                       "0x%08x (a damaged file)", crc, h->payload_crc32);
    }
    port_mem_written(h->load_addr, h->payload_size);
    out->crc_ddr = crc32(dst, h->payload_size);
    out->load_us = port_now_us() - t0;
    if (out->crc_ddr != h->payload_crc32)
        return sb_fail(SB_E_IMG_VERIFY, "the payload read back from DDR has CRC 0x%08x, not 0x%08x: DDR "
                       "lost or changed data", out->crc_ddr, h->payload_crc32);
    return SB_OK;
}
