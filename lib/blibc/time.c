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
    int status_b, century;
    if(!time) return;
    while(cmos_busy()); //wait for the update to finish

    status_b = cmos_read(STATUS_B);
    time->second = cmos_read(SECOND);
    time->minute = cmos_read(MINUTE);
    time->hour   = cmos_read(HOUR);
    time->day    = cmos_read(DAY);
    time->date   = cmos_read(DATE);
    time->month  = cmos_read(MONTH);
    time->year   = cmos_read(YEAR);
    century      = cmos_read(0x32); /* the ACPI century byte; 0 when the firmware has none */

    if(!(status_b & 0x04)) { /* BCD unless bit 2 says binary */
        int pm = time->hour & 0x80;
        time->second = bcd2bin(time->second);
        time->minute = bcd2bin(time->minute);
        time->hour   = bcd2bin(time->hour & 0x7F) | pm;
        time->day    = bcd2bin(time->day);
        time->date   = bcd2bin(time->date);
        time->month  = bcd2bin(time->month);
        time->year   = bcd2bin(time->year);
        century      = bcd2bin(century);
    }
    if(!(status_b & 0x02) && (time->hour & 0x80)) { /* 12-hour clock with PM set */
        time->hour = ((time->hour & 0x7F) + 12) % 24;
    }
    time->year += (century >= 19 && century <= 21) ? century * 100 : 2000;
}
