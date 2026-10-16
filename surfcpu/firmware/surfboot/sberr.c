/*
SurfBoot: errors and logging
--------------------
File: sberr.c Date: 10/15/26 (SurfCPU)
--------------------
See sberr.h.
*/
#include "sberr.h"
#include "fwdefs.h"
#include "port.h"

int sb_last_err;
char sb_last_msg[200];

static const char *const names[] = {
#define SB_NAME(name, led, text) #name,
    SB_ERRORS(SB_NAME)
#undef SB_NAME
};
static const char *const texts[] = {
#define SB_TEXT(name, led, text) text,
    SB_ERRORS(SB_TEXT)
#undef SB_TEXT
};
static const unsigned char leds[] = {
#define SB_LED(name, led, text) led,
    SB_ERRORS(SB_LED)
#undef SB_LED
};

const char *sb_err_name(int e)
{
    return (e >= 0 && e < SB_E_COUNT) ? names[e] : "?";
}

const char *sb_err_text(int e)
{
    return (e >= 0 && e < SB_E_COUNT) ? texts[e] : "unknown error";
}

unsigned sb_err_led(int e)
{
    return (e >= 0 && e < SB_E_COUNT) ? leds[e] : LED_CODE_TRAP;
}

int sb_fail(int e, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fmt_vsnprintf(sb_last_msg, sizeof sb_last_msg, fmt, ap);
    va_end(ap);
    sb_last_err = e;
    return e;
}

static void log_out(void *ctx, char c)
{
    (void)ctx;
    port_putc(c);
}

void sb_vlog(const char *fmt, va_list ap)
{
    fmt_vformat(log_out, 0, fmt, ap);
}

void sb_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sb_vlog(fmt, ap);
    va_end(ap);
}
