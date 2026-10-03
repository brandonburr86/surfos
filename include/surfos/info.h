/*
SurfOS System Information
----------------------
File: info.h    Date: 4/23/04
----------------------
(C)2004 Brandon Burr
*/

#ifndef _SURFOS_INFO_H
#define _SURFOS_INFO_H

#include <surfos/types.h>

#define CMOS_OUT_PORT 0x70
#define CMOS_IN_PORT 0x71

#define SECOND 0x00
#define MINUTE 0x02
#define HOUR 0x04
#define DAY 0x06
#define DATE 0x07
#define MONTH 0x08
#define YEAR 0x09
#define STATUS_A 0x0A
#define STATUS_B 0x0B
#define STATUS_C 0x0C
#define STATUS_D 0x0D

/* cmos_read(), cmos_busy() and bcd2bin() live in lib/blibc/time.c (time.h) */

#endif
