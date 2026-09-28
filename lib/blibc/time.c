/*
SurfOS blibc - time functions get_time()
(C)2004 Brandon Burr
*/
//Rebuilt 9/28/2026 - not in the printout, see REBUILD-NOTES.md

#include <asm/io.h>
#include <surfos/types.h>
#include <surfos/info.h>
#include <surfos/task.h>
#include <blibc_common.h>

//read a register out of the CMOS (the RTC lives here)
int cmos_read(u_char reg) {
    int ret;
    KCRIT_ENTER
    outb(CMOS_OUT_PORT, reg);
    ret = inb(CMOS_IN_PORT);
    KCRIT_LEAVE
    return ret;
}

//true while the RTC is updating its registers (status A, bit 7)
bool cmos_busy() {
    return (cmos_read(STATUS_A) & 0x80) ? true : false;
}

//the RTC keeps everything in BCD
int bcd2bin(u_char bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

void get_time(time_t *time) {
    if(!time) return;
    while(cmos_busy()); //wait for the update to finish

    time->second = bcd2bin(cmos_read(SECOND));
    time->minute = bcd2bin(cmos_read(MINUTE));
    time->hour   = bcd2bin(cmos_read(HOUR));
    time->day    = bcd2bin(cmos_read(DAY));
    time->date   = bcd2bin(cmos_read(DATE));
    time->month  = bcd2bin(cmos_read(MONTH));
    time->year   = bcd2bin(cmos_read(YEAR));
}
