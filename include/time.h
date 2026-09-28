/*
BLIBC Time Functions
----------------------
File: time.h    Date: Prior to 4/23/04
----------------------
(C)2004 Brandon Burr
*/
//Rebuilt 9/28/2026 - not in the printout, see REBUILD-NOTES.md

#ifndef _TIME_H
#define _TIME_H

#include <surfos/types.h>

//the time as the CMOS clock keeps it
typedef struct {
    u_int second;
    u_int minute;
    u_int hour;
    u_int day; //day of the week
    u_int date; //day of the month
    u_int month;
    u_int year; //last two digits
} time_t;

int cmos_read(u_char reg);
bool cmos_busy();
int bcd2bin(u_char bcd);

void get_time(time_t *time);

#endif
