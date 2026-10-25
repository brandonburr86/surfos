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

/* textcon.c: CTRL.MODE for the GPU driver (10/25/26), through the text console's copy of CTRL */
int arty_text_lock(void);                   /* the console lock: what arty_text_unlock() takes */
void arty_text_unlock(int how);
u32 arty_text_mode(void);                   /* 0 the test pattern, 1 text, 2 graphics, 3 graphics + text */
void arty_text_set_mode(u32 mode);          /* with the console lock held */

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
