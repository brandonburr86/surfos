/* SurfOS native IdeaPad I2C touchpad, 10/12/26. */
#ifndef _SYS_TOUCHPAD_H
#define _SYS_TOUCHPAD_H
int init_touchpad(void);
int touchpad_start(void);
void touchpad_print(void);
void touchpad_report_print(void);
void touchpad_descriptor_print(void);
#endif
