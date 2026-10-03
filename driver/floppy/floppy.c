/*
SurfOS Parallel Port Driver
--------------------
File: parport.c Date: 7/10/04
--------------------
(C)2004 Brandon Burr
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/interrupt.h>
#include <surfos/timer.h>
#include <surfos/task.h>
#include <surfos/system.h>
#include <asm/io.h>
#include <sys/floppy.h>
#include <sys/dma.h>
#include <blibc_common.h>

FD_TYPE fd[2];

bool done;
u_char status[7];
u_char statsz;
bool dchange;
u_char sr0;
u_char fdc_track;


/*
 * converts linear block address to head/track/sector
 *
 * blocks are numbered 0..heads*tracks*spt-1
 * blocks 0..spt-1 are serviced by head #0
 * blocks spt..spt*2-1 are serviced by head 1
 *
 * WARNING: garbage in == garbage out
 */
void block2hts(int block,int *head,int *track,int *sector) {
   DrvGeom geometry = { DG144_HEADS,DG144_TRACKS,DG144_SPT };
   *head = (block % (geometry.spt * geometry.heads)) / (geometry.spt);
   *track = block / (geometry.spt * geometry.heads);
   *sector = block % geometry.spt + 1;
}

/* read block (blockbuff is 512 byte buffer) */
bool fd_read_block(int block,u_char *blockbuff) {
   return fd_rw(FD0,block,blockbuff,true);
}

/* write block (blockbuff is 512 byte buffer) */
bool fd_write_block(int block,u_char *blockbuff) {
   return fd_rw(FD0,block,blockbuff,false);
}

/*
 * since reads and writes differ only by a few lines, this handles both.  This
 * function is called by read_block() and write_block()
 */
bool fd_rw(u_int drive, int block,u_char *blockbuff,bool read) {
   DrvGeom geometry = { DG144_HEADS,DG144_TRACKS,DG144_SPT };
   int head,track,sector,tries,i;
   u_char *tbaddr=0;
   /* convert logical address into physical address */
   block2hts(block,&head,&track,&sector);
//   printf("block %d = %d:%02d:%02d\n",block,head,track,sector);

   /* spin up the disk */
   fd_set_motor(drive,ON);

   if (!read && blockbuff) {
      /* copy data from data buffer into track buffer */
      tbaddr = dma_alloc();
      memcpy(tbaddr,blockbuff,512);
   }

   for(tries = 0;tries < 3;tries++) {
      /* check for diskchange */
      if (inb(FD_DIR) & 0x80) {
         dchange = true;
         fd_seek(drive,1);  /* clear "disk change" status */
         fd_recalibrate(drive);
         fd_set_motor(drive,OFF);
         return false;
      }

      /* move head to right track */
      if (!fd_seek(drive,track)) {
         fd_set_motor(drive,OFF);
         return false;
      }

      /* program data rate (500K/s) */
      outb(FD_CONFIG,0);

      /* send command */
      if (read) {
          dma_xfer(2,tbaddr,512,false);
          fd_sendbyte(CMD_READ);
      } else {
          dma_xfer(2,tbaddr,512,true);
          fd_sendbyte(CMD_WRITE);
      }

      fd_sendbyte(head << 2);
      fd_sendbyte(track);
      fd_sendbyte(head);
      fd_sendbyte(sector);
      fd_sendbyte(2);               /* 512 bytes/sector */
      fd_sendbyte(geometry.spt);
      if (geometry.spt == DG144_SPT) fd_sendbyte(DG144_GAP3RW);  /* gap 3 size for 1.44M read/write */
      else fd_sendbyte(DG168_GAP3RW);  /* gap 3 size for 1.68M read/write */
      fd_sendbyte(0xff);            /* DTL = unused */

      /* wait for command completion */
      /* read/write don't need "sense interrupt status" */
      if (!fd_wait(false)) return false;   /* timed out! */

      if ((status[0] & 0xc0) == 0) break;   /* worked! outta here! */

      fd_recalibrate(drive);  /* oops, try again... */
   }

   /* stop the motor */
   fd_set_motor(drive,ON);

   if (read && blockbuff) { /* copy data from track buffer into data buffer */
      memcpy(blockbuff,tbaddr,512);
   }
   if(tbaddr) dma_free(tbaddr);

   kprintf("status bytes: ");
   for (i = 0;i < statsz;i++)  kprintf("%02x ",status[i]);
   kprintf("\n");

   return (tries != 3);
}

/* fd sendbyte() routine from intel manual */
void fd_sendbyte(u_char byte) {
   volatile int msr;
   int tmo;
   KCRIT_ENTER
   for (tmo = 0;tmo < 128;tmo++) {
      msr = inb(FD_STATUS);
      if ((msr & 0xc0) == 0x80) {
         outb(FD_DATA,byte);
         KCRIT_LEAVE
         return;
      }
      inb(0x80);   /* delay */
   }
   KCRIT_LEAVE
}

/* fd getbyte() routine from intel manual */
u_char fd_getbyte() {
   volatile int msr;
   int tmo;
   KCRIT_ENTER
   for (tmo = 0;tmo < 128;tmo++) {
      msr = inb(FD_STATUS);
      if ((msr & 0xd0) == 0xd0) {
         KCRIT_LEAVE
         return inb(FD_DATA);
      }
      inb(0x80);   /* delay */
   }
   KCRIT_LEAVE
   return -1;   /* read timeout */
}

//This function retrieves the status byte after a command has been executed.
u_char fd_cmd_status() {
    return inb(FD_DATA);
}

//This function sets the transfer rate for the floppy controller
void fd_set_rate(u_char rate) {
    outb(FD_CONFIG,rate);
}

//Returns true if disk contents were modified on the last command.
bool fd_ismodified() {
    return (bool)(inb(FD_DIR) & DIR_CHANGED);
}

//This returns the Digital Input Register from the floppy controller
u_char fd_direg() {
    return inb(FD_DIR);
}

//Block until the floppy controller is ready to send/recieve data.
bool fd_wait(bool sensei) {
    u_long tmout = getticks()+1000;

   /* wait for IRQ6 handler to signal command finished */
   while (!done && tmout < getticks()) ;

   /* read in command result bytes */
   KCRIT_ENTER
   statsz = 0;
   while ((statsz < 7) && (inb(FD_STATUS) & (1<<4))) {
      status[statsz++] = fd_getbyte();
   }

   if (sensei) {
      /* send a "sense interrupt status" command */
      fd_sendbyte(CMD_SENSEI);
      sr0 = fd_getbyte();
      fdc_track = fd_getbyte();
   }

   done = false;
   if (!tmout) {
      /* timed out! */
      if (inb(FD_DIR) & 0x80)  /* check for diskchange */
      dchange = true;
      KCRIT_LEAVE
      return false;
   } else {
      KCRIT_LEAVE
     return true;
   }
   KCRIT_LEAVE
}

/* recalibrate the drive */
void fd_recalibrate(u_int drive) {
   /* turn the motor on */
   fd_set_motor(drive,ON);

   /* send actual command bytes */
   fd_sendbyte(CMD_RECAL);
   fd_sendbyte(0);

   /* wait until seek finished */
   fd_wait(true);

   /* turn the motor off */
   fd_set_motor(drive,OFF);
}

//This gets the status of the drive.
u_char fd_get_status() {
    return inb(FD_STATUS);
}
bool fd_seek(u_int drive, int track) {
    int i=0;
   if (fdc_track == track) return true; /* already there? */


   fd_set_motor(drive,ON);

   /* send actual command bytes */
   fd_sendbyte(CMD_SEEK);
   fd_sendbyte(0);
   fd_sendbyte(track);

   /* wait until seek finished */
   if (!fd_wait(true)) return false;     /* timeout! */

   /* now let head settle for 15ms */
   for(i=0;i<1500;i++) inb(0x80);

   fd_set_motor(drive,OFF);

   /* check that seek worked */
   if ((sr0 != 0x20) || (fdc_track != track))
     return false;
   else
     return false;
}

//Hardware Reset the floppy controller
void fd_reset() {
    outb(FD_DOR,0x0);


   outb(FD_DRS,RT_500K); /* program data rate (500K/s) */
   outb(FD_DOR,0x0c); /* re-enable interrupts */

   /* resetting triggered an interrupt - handle it */
   done = true;
   fd_wait(true);

   /* specify drive timings (got these off the BIOS) */
   fd_sendbyte(CMD_SPECIFY);
   fd_sendbyte(0xdf);  /* SRT = 3ms, HUT = 240ms */
   fd_sendbyte(0x02);  /* HLT = 16ms, ND = 0 */

   /* clear "disk change" status */
   if(fd[0]) {
      fd_seek(FD0,1);
      fd_recalibrate(FD0);
   }

   if(fd[1]) {
      fd_seek(FD1,1);
      fd_recalibrate(FD1);
   }

   dchange = false;
}

//Turn on or off the motor to a drive
void fd_set_motor(u_int drive, fd_motor_state state) {
    u_char byte;

    switch(drive) {
    case FD0:
        if(state==ON) byte = 0x1C;
        else byte = 0x0c;
        break;
    case FD1:
        if(state==ON) byte = 0x1D;
        else byte = 0x0D;
        break;
    }
    outb(FD_DOR,byte);
}

FuncIRQHandler floppyISR() {
    kprintf("floppy!\n");
    done = true;
    return 0;
}

char *getFDStr(FD_TYPE drive) {
    char *drive_type[6] = { "no floppy drive", "360kb 5.25in floppy drive", "1.2mb 5.25in floppy drive", "720kb 3.5in floppy drive", "1.44mb 3.5in floppy drive", "2.88mb 3.5in floppy drive"};
    if(drive>=0 && drive <6) {
        return drive_type[drive];
    }
    return 0;
}


void detectFloppy() {
    u_char c;
    outb(0x70, 0x10);
    c = inb(0x71);

    fd[0] = (FD_TYPE)(c >> 4); // get the high nibble
    fd[1] = (FD_TYPE)(c & 0xF); // get the low nibble by ANDing out the high nibble
}

void init_floppy() {
    int i=0;
    kprintf("\nInitializing Floppy Drive\n");

    //default to no floppy drives
    fd[0] = FD_NONE;
    fd[1] = FD_NONE;

    done =false;
    for(i=0;i<7;i++) status[i] = 0;
    statsz = 0;
    dchange = false;
    sr0 = 0;
    fdc_track = 0xff;

    detectFloppy(); //fill the fd[] array

    if(fd[0]) kprintf("Floppy drive 0 is a: %s\n",getFDStr(fd[0]));
    if(fd[1]) kprintf("Floppy drive 1 is a: %s\n",getFDStr(fd[1]));

    if(!fd[0] && !fd[1]) return;

    add_irq_handler(IRQ_FLOPPY,(FuncIRQHandler)floppyISR,0);

    fd_reset();

    kprintf("*DONE\n");
}
