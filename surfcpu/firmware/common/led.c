/*
SurfCPU firmware: LEDs
--------------------
File: led.c Date: 10/15/26 (SurfCPU)
--------------------
The four green LEDs LD4..LD7 (bit 0 = LD4) and the four RGB LEDs LD0..LD3 (LED n: bits 3n
red, 3n+1 green, 3n+2 blue). A failure code is shown in binary on the green LEDs with LD0
red; the codes are in fwdefs.h and docs/FIRMWARE.md.
*/
#include "fw.h"

void led_set(unsigned green4, unsigned rgb12)
{
    REG(SURF_GPIO_BASE, SURF_GPIO_LED) = green4 & SURF_GPIO_LED_LED_MASK;
    REG(SURF_GPIO_BASE, SURF_GPIO_RGB) = rgb12 & SURF_GPIO_RGB_RGB_MASK;
}

void led_code(unsigned code)
{
    led_set(code, 0x001);           /* LD0 red */
}
