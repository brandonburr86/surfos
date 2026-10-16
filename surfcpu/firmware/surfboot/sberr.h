/*
SurfBoot: errors and logging
--------------------
File: sberr.h Date: 10/15/26 (SurfCPU)
--------------------
Every SurfBoot function that can fail returns 0 (SB_OK) or one of these codes, after
recording a sentence that says what was wrong with the numbers that matter
(sb_fail()). The boot log, the monitor and the LEDs report them; the host tests check
both the code and the sentence. Names are stable: the tests and docs/FIRMWARE.md use them.
*/
#ifndef SURFBOOT_SBERR_H
#define SURFBOOT_SBERR_H

#include <stdarg.h>
#include "fmt.h"

#define SB_ERRORS(X) \
    X(OK,              0, "no error") \
    X(DDR_ABSENT,      1, "no DDR controller") \
    X(DDR_CALIB,       1, "DDR calibration failed") \
    X(DDR_TIMEOUT,     1, "DDR calibration timed out") \
    X(DDR_TEST,        1, "DDR test failed") \
    X(NO_CARD,         2, "no card") \
    X(SD_NORESP,       3, "card does not answer") \
    X(SD_INIT,         3, "card initialization failed") \
    X(SD_UNSUPPORTED,  3, "card not supported") \
    X(SD_READ,         3, "card read failed") \
    X(MBR,             4, "bad partition table") \
    X(GPT,             4, "GPT partition table") \
    X(SUPERFLOPPY,     4, "no partition table") \
    X(NO_FAT32_PART,   4, "no FAT32 partition") \
    X(PART_RANGE,      4, "partition outside the card") \
    X(EXFAT,           5, "exFAT file system") \
    X(NTFS,            5, "NTFS file system") \
    X(FAT1216,         5, "FAT12/FAT16 file system") \
    X(BPB,             5, "bad FAT32 boot sector") \
    X(FAT_CHAIN,       5, "bad cluster chain") \
    X(FAT_CYCLE,       5, "cluster chain cycle") \
    X(DIR,             5, "bad directory") \
    X(NOT_FOUND,       6, "file not found") \
    X(NOT_FILE,        6, "not a file") \
    X(NOT_DIR,         6, "not a directory") \
    X(BAD_PATH,        6, "not an 8.3 path") \
    X(IMG_SHORT,       7, "package too short") \
    X(IMG_MAGIC,       7, "not a SURFOS.BIN package") \
    X(IMG_VERSION,     7, "unsupported package version") \
    X(IMG_HCRC,        7, "package header checksum") \
    X(IMG_ISA,         7, "package needs a missing ISA feature") \
    X(IMG_PLATFORM,    7, "package for another platform") \
    X(IMG_SIZE,        7, "package size mismatch") \
    X(IMG_ALIGN,       7, "misaligned load address") \
    X(IMG_RANGE,       7, "load range outside usable DDR") \
    X(IMG_ENTRY,       7, "bad entry point") \
    X(IMG_CRC,         7, "payload checksum") \
    X(IMG_VERIFY,      7, "DDR read-back checksum") \
    X(TRAP,            8, "unexpected trap") \
    X(STOPPED,         9, "stopped by request") \
    X(CRC_SELFTEST,    8, "CRC self-test failed")

enum sb_err {
#define SB_ENUM(name, led, text) SB_E_##name,
    SB_ERRORS(SB_ENUM)
#undef SB_ENUM
    SB_E_COUNT
};
#define SB_OK SB_E_OK

const char *sb_err_name(int e);     /* "FAT_CYCLE" */
const char *sb_err_text(int e);     /* "cluster chain cycle" */
unsigned sb_err_led(int e);         /* LED code (fwdefs.h) */

/* Records code e and the formatted sentence; returns e. */
int sb_fail(int e, const char *fmt, ...) FMT_PRINTF(2, 3);
extern int sb_last_err;
extern char sb_last_msg[200];

/* Boot log: to the console. */
void sb_log(const char *fmt, ...) FMT_PRINTF(1, 2);
void sb_vlog(const char *fmt, va_list ap);

#endif /* SURFBOOT_SBERR_H */
