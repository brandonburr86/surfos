/*
SurfBoot: the boot sequence
--------------------
File: boot.h Date: 10/15/26 (SurfCPU)
--------------------
docs/BOOT-ABI.md end to end, without the final jump (which only the device can do): wait
for DDR calibration (with a time limit) and test it quickly, initialize the card, mount
FAT32, load /BOOT/SURFOS.BIN (or /BOOT/BACKUP.BIN when SURFOS.BIN fails a check, or first
when BTN0 is held), read /BOOT/CMDLINE.TXT and write the boot information. Portable: the
host tests run exactly this code against card images.
*/
#ifndef SURFBOOT_BOOT_H
#define SURFBOOT_BOOT_H

#include <stdint.h>
#include "fat32.h"
#include "image.h"
#include "sd.h"

#define SB_PATH_MAIN     "/BOOT/SURFOS.BIN"
#define SB_PATH_BACKUP   "/BOOT/BACKUP.BIN"
#define SB_PATH_CMDLINE  "/BOOT/CMDLINE.TXT"

#define SB_DDR_WAIT_US   2000000u   /* DDR calibration time limit */
#define SB_SPI_FAST_HZ   6250000u   /* SPI clock after card initialization */

struct sb_state {
    int ddr_ok;
    uint32_t ram_base, ram_size, ram_limit;     /* usable DDR for images: [base, limit) */
    uint32_t ddr_wait_us;
    int card_ok, vol_ok;
    struct sd_card card;
    struct fat_vol vol;
    uint32_t spi_fast_div;                      /* 0 = from SB_SPI_FAST_HZ */
    struct img_info img;
    const char *image_path;
    int used_backup;
    char cmdline[SURF_BOOTINFO_CMDLINE_LEN];
    int cmdline_trunc;
    int loaded;                                 /* an image is in DDR and verified */
};

enum sb_choice { SB_BOOT_NORMAL = 0, SB_BOOT_BACKUP = 1 };

int sb_ddr_init(struct sb_state *s);
int sb_card_init(struct sb_state *s);
int sb_load_path(struct sb_state *s, const char *path);
int sb_read_cmdline(struct sb_state *s);
void sb_fill_bootinfo(struct sb_state *s);
/* The whole sequence. path = NULL boots SURFOS.BIN (with BACKUP.BIN as the fallback), or
 * BACKUP.BIN alone when choice is SB_BOOT_BACKUP. On SB_OK the image is in DDR, verified,
 * and the boot information is written: the caller hands over. */
int sb_boot(struct sb_state *s, const char *path, int choice);

/* The card as a sector source for fat32.c. */
int sb_card_read(void *ctx, uint32_t lba, void *buf);

/* SurfBoot's own version (boot info). */
uint32_t sb_version(void);
uint32_t sb_build_id(void);

/* The monitor (monitor.c). */
enum { MON_CONTINUE = 0, MON_BOOT, MON_RESET };
int sb_monitor_line(struct sb_state *s, char *line);
void sb_monitor_help(void);

#endif /* SURFBOOT_BOOT_H */
