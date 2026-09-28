/*
SurfOS Parallel Port Driver
----------------------
File: parport.h Date: 5/22/04
----------------------
(C)2004 Brandon Burr
*/

#ifndef _SYS_PARPORT_H
#define _SYS_PARPORT_H

#include <surfos/types.h>

typedef u_int PARPORT;

//parallel port definitions
//#define PAR0 BASE 0x3BC//0x378
extern u_int PAR0_BASE;

#define PAR0 (PAR0_BASE)

#define PAR_DATA (0x0)
#define PAR_STATUS (0x1)
#define PAR_CONTROL (0x2)

//status definitions
#define PAR_STAT_BUSY 0x80
#define PAR_STAT_ACK 0x40
#define PAR_STAT_PAPER 0x20
#define PAR_STAT_SELECT 0x10
#define PAR_STAT_ERROR 0x8

//control register definitions
#define PAR_CONTROL_INT 0x8
#define PAR_CONTROL_SELECT 0x10
#define PAR_CONTROL_INIT 0x20
#define PAR_CONTROL_AUTOFEED 0x40
#define PAR_CONTROL_STROBE 0x80


#define GET_CONTROL_BYTE(port) (inb(port+PAR_CONTROL))
#define GET_STATUS_BYTE(port) (inb(port+PAR_STATUS))

#define SET_CONTROL_BYTE(port,data) (outb(port+PAR_CONTROL,data))

#define CONTROL_SWITCH(state,byte,bit) ((state) ? (byte) | (bit) : (byte) & ~(bit))

#define CHECK_BIT(byte,bit) ((byte) & (bit) ? 0 : 1)
// interact w/ the data port
u_char parGetByte(PARPORT port);
void parSendByte(PARPORT port, u_char data);

// read the status register
bool parIsBusy(PARPORT port);
bool parHasAck(PARPORT port);
bool parHasPaper(PARPORT port);
bool parIsSelected(PARPORT port);
bool parHasError(PARPORT port);

// should the port use interrupts?
void parSetInterrupt(PARPORT port, bool value);
bool parGetInterrupt(PARPORT port);

// should selecting be enabled?
void parSetSelect(PARPORT port, bool value);
bool parGetSelect(PARPORT port);

// need to init the port
void parSetInit(PARPORT port,bool value);
bool parGetInit(PARPORT port);

// auto feed for printers
void parSetAutoFeed(PARPORT port, bool value);
bool parGetAutoFeed(PARPORT port);

// should enable strobing?
void parSetStrobe(PARPORT port,bool value);
bool parGetStrobe(PARPORT port);

void printParStatus();
void runParPortCmd();

void init_parport();
#endif
