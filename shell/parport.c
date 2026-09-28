//parport stuff

#include <blibc_common.h>

#include <surfos/types.h>
#include <surfos/kernel.h>
#include <sys/parport.h>

void runParPortCmd() {
    //parSetSelect(PAR0,1);
    //while(1) {
        //tmp=parGetByte(PAR0);
        //while(1)  {
        parSendByte(PAR0,'A');
        //}
        /*if(tmp!=0) {
        putch(tmp);
        if(tmp=='q') return;
        }
    tmp=0;*/
    //}
}
bool parIsBusy(PARPORT port);
bool parHasAck(PARPORT port);
bool parHasPaper(PARPORT port);
bool parIsSelected(PARPORT port);
bool parHasError(PARPORT port);
void printParStatus() {
    int busy,ack,paper,select,error;
    //while(code!=0) code=parGetByte(PAR0),printf("\n%i\n",code);
    busy=parIsBusy(PAR0);
    ack=parHasAck(PAR0);
    paper=parHasPaper(PAR0);
    select=parIsSelected(PAR0);
    error=parHasError(PAR0);
    printf("\nParallel Port 0 Status:\n Busy = %i\n Ack = %i\n Paper = %i\n Select = %i\n Error = %i\n\n",busy,ack,paper,select,error);
}
