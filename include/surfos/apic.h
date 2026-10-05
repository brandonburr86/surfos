/*
SurfOS Local APIC
----------------------
File: apic.h    Date: 10/5/26 (USB boot)
----------------------
SurfOS keeps its 8259 PICs. On a PC that a UEFI firmware handed over, the 8259s only
reach the CPU through the local APIC's LINT0 pin, so the laptop profile puts the local
APIC into "virtual wire" mode (LINT0 = ExtINT, LINT1 = NMI) and masks every I/O APIC
input, whatever the firmware left there. The local APIC timer is the fallback tick
when the PIT does not deliver one. Only the boot CPU is used; xAPIC (memory-mapped)
and x2APIC (MSR) register access both work.
*/

#ifndef _SURFOS_APIC_H
#define _SURFOS_APIC_H

#include <surfos/types.h>

#define T_APIC_TIMER    0x30    /* the local APIC timer's vector */
#define T_APIC_SPURIOUS 0xFF

int init_apic(void);                    /* 0 when the local APIC is in virtual wire mode, 1 when there is none */
bool apic_active(void);
void apic_eoi(void);
u32 apic_timer_calibrate(void (*wait_10ms)(void));  /* timer counts (divide by 16) in 10 ms */
void apic_timer_start(u32 count_per_tick);          /* periodic, vector T_APIC_TIMER */
void apic_timer_stop(void);
u_long apic_timer_irqs(void);
const char *apic_describe(char *buf, size_t size);

#endif
