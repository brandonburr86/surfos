/*
SurfOS Parallel Port Driver
--------------------
File: parport.c Date: 5/22/03
--------------------
(C)2004 Brandon Burr
*/

#include <surfos/types.h>
#include <asm/io.h>
#include <sys/parport.h>
#include <surfos/system.h>
#include <surfos/console.h>
#include <surfos/interrupt.h>

u_char parStatusByte,parControlByte;

u_int PAR0_BASE;

u_char parGetByte(PARPORT port) {
    u_char ret=inb(port);
    return ret;
}

void parSendByte(PARPORT port, u_char data) {
    outb(port,data);
    sleep(10);
    parSetStrobe(port,true); //tell that byte exists
    sleep(10);
    parSetStrobe(port,false);
    sleep(10);
    //while(!parHasAck(port));
}


bool parIsBusy(PARPORT port) {
    parStatusByte=GET_STATUS_BYTE(port);
    return CHECK_BIT(parStatusByte,PAR_STAT_BUSY);
}

bool parHasAck(PARPORT port) {
    parStatusByte=GET_STATUS_BYTE(port);
    return CHECK_BIT(parStatusByte,PAR_STAT_ACK);
}

bool parHasPaper(PARPORT port) {
    parStatusByte=GET_STATUS_BYTE(port);
    return CHECK_BIT(parStatusByte,PAR_STAT_PAPER);
}

bool parIsSelected(PARPORT port) {
    parStatusByte=GET_STATUS_BYTE(port);
    return CHECK_BIT(parStatusByte,PAR_STAT_SELECT);
}

bool parHasError(PARPORT port) {
    parStatusByte=GET_STATUS_BYTE(port);
    return CHECK_BIT(parStatusByte,PAR_STAT_ERROR);
}


void parSetInterrupt(PARPORT port, bool value) {
    parControlByte = GET_CONTROL_BYTE(port);
    parControlByte = CONTROL_SWITCH(value,parControlByte,PAR_CONTROL_INT);
    SET_CONTROL_BYTE(PAR0, parControlByte);
}
bool parGetInterrupt(PARPORT port) {
    return CHECK_BIT(parControlByte,PAR_CONTROL_INT);
}


void parSetSelect(PARPORT port, bool value) {
    parControlByte = GET_CONTROL_BYTE(port);
    parControlByte = CONTROL_SWITCH(value,parControlByte,PAR_CONTROL_SELECT);
    SET_CONTROL_BYTE(PAR0, parControlByte);
}
bool parGetSelect(PARPORT port) {
    return CHECK_BIT(parControlByte,PAR_CONTROL_SELECT);
}


void parSetInit(PARPORT port,bool value) {
    parControlByte = GET_CONTROL_BYTE(port);
    parControlByte = CONTROL_SWITCH(value,parControlByte,PAR_CONTROL_INIT);
    SET_CONTROL_BYTE(PAR0, parControlByte);
}
bool parGetInit(PARPORT port) {
    return CHECK_BIT(parControlByte,PAR_CONTROL_INIT);
}


void parSetAutoFeed(PARPORT port, bool value) {
    parControlByte = GET_CONTROL_BYTE(port);
    parControlByte = CONTROL_SWITCH(value,parControlByte,PAR_CONTROL_AUTOFEED);
    SET_CONTROL_BYTE(PAR0, parControlByte);
}
bool parGetAutoFeed(PARPORT port) {
    return CHECK_BIT(parControlByte,PAR_CONTROL_AUTOFEED);
}


void parSetStrobe(PARPORT port,bool value) {
    parControlByte = GET_CONTROL_BYTE(port);
    parControlByte = CONTROL_SWITCH(value,parControlByte,PAR_CONTROL_STROBE);
    SET_CONTROL_BYTE(PAR0, parControlByte);
}
bool parGetStrobe(PARPORT port) {
    return CHECK_BIT(parControlByte,PAR_CONTROL_STROBE);
}

u_int getParPortAddr() {
    u_int *addr;
    u_int port;
    int i;

    addr = (u_int *)0x00000408;
    for(i=0;i<1;i++) { //just one for now
        port=*addr;
        if(port==0) {
            kprintf("*No Par%i Detected\n",i);
        } else {
            kprintf("*Par%i Detected At Address: 0x%x\n",i,port);
        }
        *addr++;
    }
    return port;
}

FuncIRQHandler parPortISR() {
    reboot();
}

void init_parport() {
    kprintf("\nInitializing Parallel Port\n");
    parStatusByte=parControlByte=0;
    PAR0_BASE = getParPortAddr();
    if(PAR0) parSetInit(PAR0,true);
    kprintf("*DONE\n");
    add_irq_handler(IRQ_PRINTER,(FuncIRQHandler)parPortISR,0);
}
