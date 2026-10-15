/*
SurfOS DesignWare I2C
--------------------
File: i2cdw.h Date: 10/12/26
--------------------
Polled PIO transport. The caller owns power, clocks, mapping and serialization.
*/
#ifndef _SYS_I2CDW_H
#define _SYS_I2CDW_H

#include <surfos/types.h>

struct i2cdw_io {
    void *ctx;
    u32 (*read32)(void *ctx, u32 reg);
    void (*write32)(void *ctx, u32 reg, u32 value);
    void (*delay_us)(void *ctx, u32 us);
};

enum i2cdw_speed { I2CDW_STANDARD = 1, I2CDW_FAST = 2 };
struct i2cdw_config {
    enum i2cdw_speed speed;
    u32 hcnt, lcnt, sda_hold;       /* Supplied platform timing, in input clocks. */
    u32 poll_us, timeout_us;        /* Nonzero; poll_us <= timeout_us. */
};

enum i2cdw_error {
    I2CDW_OK, I2CDW_BAD_ARG, I2CDW_UNSUPPORTED, I2CDW_NOT_READY,
    I2CDW_DISABLE_TIMEOUT, I2CDW_ENABLE_TIMEOUT, I2CDW_BUS_BUSY,
    I2CDW_TIMEOUT, I2CDW_ABORT, I2CDW_SHORT_TRANSFER, I2CDW_FIFO_ERROR
};

struct i2cdw_status {
    enum i2cdw_error error;
    u32 abort_source;               /* Captured before any abort-clear/recovery. */
    u_int tx_queued, rx_done;       /* Queued is not a count of ACKed bytes. */
    bool recovered;                 /* An error was followed by successful cleanup. */
};

struct i2cdw {
    struct i2cdw_io io;
    struct i2cdw_config config;
    u_int tx_depth, rx_depth;       /* Discovered from IC_COMP_PARAM_1. */
    bool ready;
    struct i2cdw_status last;
};

/* Init and successful transfers leave the controller disabled, interrupts masked.
   Error cleanup is bounded; !ready requires reinitialization by the caller.
   Callbacks must themselves return; delay_us must advance time by at least us. */
enum i2cdw_error i2cdw_init(struct i2cdw *d, const struct i2cdw_io *io,
                           const struct i2cdw_config *config);

/* One 7-bit transaction: write, read, or write then repeated-start read. No empty
   transaction. On error rx[0..last.rx_done) contains partial input; tx may have
   had side effects, so the caller must decide whether retrying is appropriate. */
enum i2cdw_error i2cdw_xfer(struct i2cdw *d, u16 address,
                           const u8 *tx, u_int txlen, u8 *rx, u_int rxlen);

#endif
