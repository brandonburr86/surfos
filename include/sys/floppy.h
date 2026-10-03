/*
SurfOS Floppy driver Header
----------------------
File: driver.h  Date: 7/10/04
----------------------
(C)2004 Brandon Burr
*/

#ifndef _SYS_FLOPPY_H
#define _SYS_FLOPPY_H

#include <surfos/types.h>
#include <surfos/interrupt.h>

/*
Type of drive:      Number the CMOS gives it:
360kb 5.25in        1
1.2mb 5.25in        2
720kb 3.5in         3
1.44mb 3.5in        4
2.88mb 3.5in        5
No drive            0
*/
typedef enum {
    FD_NONE,
    FD_525_360,
    FD_525_12,
    FD_35_720,
    FD_35_144,
    FD_35_288
} FD_TYPE;

typedef enum {
    OFF,
    ON
} fd_motor_state;

/* drive geometry */
typedef struct DrvGeom {
   u_char heads;
   u_char tracks;
   u_char spt;     /* sectors per track */
} DrvGeom;

#define FD0 0
#define FD1 1

/* drive geometries */
#define DG144_HEADS       2     /* heads per drive (1.44M) */
#define DG144_TRACKS     80     /* number of tracks (1.44M) */
#define DG144_SPT        18     /* sectors per track (1.44M) */
#define DG144_GAP3FMT  0x54     /* gap3 while formatting (1.44M) */
#define DG144_GAP3RW   0x1b     /* gap3 while reading/writing (1.44M) */

#define DG168_HEADS       2     /* heads per drive (1.68M) */
#define DG168_TRACKS     80     /* number of tracks (1.68M) */
#define DG168_SPT        21     /* sectors per track (1.68M) */
#define DG168_GAP3FMT  0x0c     /* gap3 while formatting (1.68M) */
#define DG168_GAP3RW   0x1c     /* gap3 while reading/writing (1.68M) */

#define FD_BASEP (0x3f0) //base address for primary controller (r)
#define FD_DOR (0x3f2) //digital output register (w)
#define FD_DRS  (0x3f4)   // Data Rate Select Register (w)
#define FD_STATUS (0x3f4) //main status register (r)
#define FD_DATA (0x3f5) //main data register (r/w)
#define FD_DIR (0x3f7) //digital input register (r)
#define FD_CONFIG (0x3f7) //configration register (w)

//digital output register bits
/*
DRO-DR1: 00 = drive 0 (A)   01 = drive 1 (B)    10 = drive 2 (C)    11 = drive 3 (D)
*/
#define DOR_DR0 (0x1) //drive bit 0
#define DOR_DR1 (0x2) //drive bit 1
#define DOR_REST (0x4) //set to enable controller, 0 to reset
#define DOR_DMA (0x8) //DMA and IRQ enabled
#define DOR_MOTA (0x10) //motor A enabled
#define DOR_MOTB (0x20) //motor B enabled
#define DOR_MOTC (0x40) //motor C enabled
#define DOR_MOTD (0x80) //motor D enabled

//digital input register
#define DIR_RESV0 (0x1)
#define DIR_RESV1 (0x2)
#define DIR_RESV2 (0x4)
#define DIR_RESV3 (0x8)
#define DIR_RESV4 (0x10)
#define DIR_RESV5 (0x20)
#define DIR_RESV6 (0x40)
#define DIR_CHANGED (0x80)

/* command bytes (these are 765 commands + options such as MFM, etc) */
#define CMD_SPECIFY (0x03)  /* specify drive timings */
#define CMD_WRITE   (0xc5)  /* write data (+ MT,MFM) */
#define CMD_READ    (0xe6)  /* read data (+ MT,MFM,SK) */
#define CMD_RECAL   (0x07)  /* recalibrate */
#define CMD_SENSEI  (0x08)  /* sense interrupt status */
#define CMD_FORMAT  (0x4d)  /* format track (+ MFM) */
#define CMD_SEEK    (0x0f)  /* seek track */
#define CMD_VERSION (0x10)  /* FDC version */


typedef struct {
    bool mrq:1;
    bool dio:1;
    bool ndma:1;
    bool busy:1;
    bool actd:1;
    bool actc:1;
    bool actb:1;
    bool acta:1;
} msr_byte __attribute((packed));

//main status register
#define MSR_ACTA (0x1) //A active
#define MSR_ACTB (0x2) //B active
#define MSR_ACTC (0x4) //C active
#define MSR_ACTD (0x8) //D active
#define MSR_BUSY (0x10) //device busy
#define MSR_NDMA (0x20) //controller in DMA mode
#define MSR_DIO (0x40) //data I/O (1 = controller ? CPU     0 = CPU ? controller)
#define MSR_MRQ (0x80) //main request..  data register ready?

//rates for the configuration control register

/*
Rate    DiskCapacity    Size    DriveCapacity
250Kb/s  360K           5.25"   360K
         720K           3.5"    1.44MB
         720K           3.5"    720K
300Kb/s  720K           3.5"    720K
         360K           5.25"   1.2M
500Kb/s  1.2M           5.25"   1.2Mb
         1.44M          3.5"    1.44Mb
*/

#define RT_500K 0x0
#define RT_300K 0x1
#define RT_250K 0x2
#define RT_1M 0x3


bool fd_read_block(int block,u_char *blockbuff);
bool fd_write_block(int block,u_char *blockbuff);
bool fd_rw(u_int drive, int block,u_char *blockbuff,bool read);
bool fd_seek(u_int drive, int track);
void fd_recalibrate(u_int drive);

void block2hts(int block,int *head,int *track,int *sector);

void fd_sendbyte(u_char byte);
u_char fd_getbyte();

u_char fd_cmd_readtrack();
u_char fd_cmd_status();
void fd_set_rate(u_char rate);
bool fd_ismodified();
u_char fd_direg();
bool fd_wait(bool sensei);
u_char fd_get_status();
void fd_reset();
void fd_set_motor(u_int drive, fd_motor_state state);
FuncIRQHandler floppyISR();
char *getFDStr(FD_TYPE drive);
void detectFloppy();
void init_floppy();
#endif
