/*
SurfOS board: SurfCPU on the Arty A7 -- what its files share
----------------------
File: arty.h    Date: 10/15/26 (SurfCPU)
----------------------
*/

#ifndef _ARTY_H
#define _ARTY_H

#include <surfos/types.h>

/* board.c */
u32 arty_sys_hz(void);                      /* sysinfo.SYS_HZ (25 MHz) */
bool arty_has(u32 feature_mask);            /* sysinfo.FEATURES & SURF_SYSINFO_FEATURES_*_MASK */

/* ps2kbd.c: the PS/2 keyboard */
int arty_ps2_init(void);
const char *arty_ps2_describe(char *buf, size_t size);
int arty_cmd_kbdinfo(int argc, char **argv);

/* sd.c: the microSD card as the block device sd0 */
int arty_sd_init(void);
const char *arty_sd_describe(char *buf, size_t size);
int arty_cmd_sdinfo(int argc, char **argv);
int arty_cmd_sdmode(int argc, char **argv);

#endif
