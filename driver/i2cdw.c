/*
SurfOS DesignWare I2C
--------------------
File: i2cdw.c Date: 10/12/26
--------------------
Polled PIO engine; platform discovery and power sequencing are external.
Register/sequence references: Intel 495 PCH datasheet 341081, chapter 14;
Linux v6.12 i2c-designware-{core.h,common.c,master.c}. No DMA or IRQ ownership.
*/
#include <sys/i2cdw.h>

enum {
    IC_CON=0x00, IC_TAR=0x04, IC_DATA_CMD=0x10,
    IC_SS_HCNT=0x14, IC_SS_LCNT=0x18, IC_FS_HCNT=0x1c, IC_FS_LCNT=0x20,
    IC_INTR_MASK=0x30, IC_RAW_INTR_STAT=0x34, IC_RX_TL=0x38, IC_TX_TL=0x3c,
    IC_CLR_INTR=0x40, IC_CLR_TX_ABRT=0x54, IC_CLR_STOP_DET=0x60,
    IC_ENABLE=0x6c, IC_STATUS=0x70, IC_TXFLR=0x74, IC_RXFLR=0x78,
    IC_SDA_HOLD=0x7c, IC_TX_ABRT_SOURCE=0x80, IC_DMA_CR=0x88, IC_ENABLE_STATUS=0x9c,
    IC_COMP_PARAM_1=0xf4, IC_COMP_VERSION=0xf8, IC_COMP_TYPE=0xfc
};
enum {
    CMD_READ=0x100, CMD_STOP=0x200, CMD_RESTART=0x400,
    INTR_ABRT=0x40, INTR_STOP=0x200, INTR_FIFO_ERROR=0x0b,
    STATUS_ACTIVE=0x21
};
static u32 rd(struct i2cdw *d, u32 reg) { return d->io.read32(d->io.ctx, reg); }
static void wr(struct i2cdw *d, u32 reg, u32 value) { d->io.write32(d->io.ctx, reg, value); }
static void clear_status(struct i2cdw *d) { d->last = (struct i2cdw_status){0}; }

/* A decreasing delay budget bounds every wait independently of timer wrap.
   Register callbacks must be finite; the platform supplies the delay itself. */
static bool pause_poll(struct i2cdw *d, u32 *remaining) {
    u32 us;
    if(!*remaining) return false;
    us = d->config.poll_us < *remaining ? d->config.poll_us : *remaining;
    d->io.delay_us(d->io.ctx, us);
    *remaining -= us;
    return true;
}
static bool wait_bits(struct i2cdw *d, u32 reg, u32 mask, u32 wanted) {
    u32 remaining = d->config.timeout_us;
    do {
        if((rd(d, reg) & mask) == wanted) return true;
    } while(pause_poll(d, &remaining));
    return false;
}
static bool disable(struct i2cdw *d) {
    wr(d, IC_INTR_MASK, 0);
    wr(d, IC_ENABLE, 0);
    return wait_bits(d, IC_ENABLE_STATUS, 1, 0);
}
static void configure(struct i2cdw *d) {
    u32 high = d->config.speed == I2CDW_STANDARD ? IC_SS_HCNT : IC_FS_HCNT;
    /* Master, selected speed, restart enabled, slave disabled; 7-bit target. */
    wr(d, IC_CON, 1 | ((u32)d->config.speed << 1) | 0x20 | 0x40);
    wr(d, high, d->config.hcnt);
    wr(d, high + 4, d->config.lcnt);
    wr(d, IC_SDA_HOLD, d->config.sda_hold);
    wr(d, IC_DMA_CR, 0);
    wr(d, IC_RX_TL, 0);
    wr(d, IC_TX_TL, 0);
    (void)rd(d, IC_CLR_INTR);
}

static enum i2cdw_error fail(struct i2cdw *d, enum i2cdw_error error) {
    bool aborted = true, disabled;
    d->last.error = error;
    wr(d, IC_INTR_MASK, 0);
    /* 10/15/26: after TX_ABRT (a NACK) the controller has already flushed its
       FIFO and sent STOP, so there is nothing to abort; Linux only aborts a
       master on hold. Asking an idle master to abort may never complete. */
    if(error != I2CDW_ABORT && (rd(d, IC_ENABLE_STATUS) & 1)) {
        /* A timed-out master may be holding SCL with an empty FIFO. Request a
           STOP/abort before disabling. There is no platform reset in this layer. */
        wr(d, IC_ENABLE, 3);
        aborted = wait_bits(d, IC_ENABLE, 2, 0);
    }
    disabled = disable(d);
    d->last.recovered = aborted && disabled;
    d->ready = d->last.recovered;
    if(d->ready) configure(d);
    return error;
}

static enum i2cdw_error fault(struct i2cdw *d, u32 raw) {
    if(raw & INTR_ABRT) {
        d->last.abort_source = rd(d, IC_TX_ABRT_SOURCE);
        (void)rd(d, IC_CLR_TX_ABRT);
        return I2CDW_ABORT;
    }
    if(raw & INTR_FIFO_ERROR) return I2CDW_FIFO_ERROR;
    return I2CDW_OK;
}

enum i2cdw_error i2cdw_init(struct i2cdw *d, const struct i2cdw_io *io,
                           const struct i2cdw_config *config) {
    u32 param;
    if(!d) return I2CDW_BAD_ARG;
    d->ready = false;
    clear_status(d);
    if(!io || !io->read32 || !io->write32 || !io->delay_us || !config ||
       (config->speed != I2CDW_STANDARD && config->speed != I2CDW_FAST) ||
       !config->hcnt || config->hcnt > 0xffff || !config->lcnt || config->lcnt > 0xffff ||
       !config->poll_us || config->poll_us > config->timeout_us)
        return d->last.error = I2CDW_BAD_ARG;
    d->io = *io;
    d->config = *config;
    param = rd(d, IC_COMP_PARAM_1);
    if(rd(d, IC_COMP_TYPE) != 0x44570140 || param == 0xffffffff ||
       rd(d, IC_COMP_VERSION) < 0x3131312a)
        return d->last.error = I2CDW_UNSUPPORTED;
    d->tx_depth = ((param >> 16) & 0xff) + 1;
    d->rx_depth = ((param >> 8) & 0xff) + 1;
    if(!disable(d)) return d->last.error = I2CDW_DISABLE_TIMEOUT;
    configure(d);
    d->ready = true;
    return I2CDW_OK;
}
enum i2cdw_error i2cdw_xfer(struct i2cdw *d, u16 address,
                           const u8 *tx, u_int txlen, u8 *rx, u_int rxlen) {
    u_int read_queued = 0;
    u32 remaining;
    bool stopped = false;
    if(!d) return I2CDW_BAD_ARG;
    clear_status(d);
    if(address > 0x7f || (txlen && !tx) || (rxlen && !rx) || (!txlen && !rxlen))
        return d->last.error = I2CDW_BAD_ARG;
    if(!d->ready) return d->last.error = I2CDW_NOT_READY;
    if(!disable(d)) return fail(d, I2CDW_DISABLE_TIMEOUT);
    if(!wait_bits(d, IC_STATUS, STATUS_ACTIVE, 0)) return fail(d, I2CDW_BUS_BUSY);
    wr(d, IC_TAR, address);
    (void)rd(d, IC_CLR_INTR);
    wr(d, IC_ENABLE, 1);
    if(!wait_bits(d, IC_ENABLE_STATUS, 1, 1)) return fail(d, I2CDW_ENABLE_TIMEOUT);

    remaining = d->config.timeout_us;
    do {
        u32 raw = rd(d, IC_RAW_INTR_STAT), n, space;
        enum i2cdw_error error = fault(d, raw);
        if(error) return fail(d, error);
        n = rd(d, IC_RXFLR);
        if(n > d->rx_depth || n > read_queued - d->last.rx_done)
            return fail(d, I2CDW_FIFO_ERROR);
        while(n--) rx[d->last.rx_done++] = (u8)rd(d, IC_DATA_CMD);
        if(raw & INTR_STOP) {
            stopped = true;
            (void)rd(d, IC_CLR_STOP_DET);
        }
        n = rd(d, IC_TXFLR);
        if(n > d->tx_depth) return fail(d, I2CDW_FIFO_ERROR);
        if(stopped && !(rd(d, IC_STATUS) & STATUS_ACTIVE) && !n) {
            /* Recheck for a late abort before reporting completed traffic. */
            error = fault(d, rd(d, IC_RAW_INTR_STAT));
            if(error) return fail(d, error);
            if(d->last.tx_queued != txlen || read_queued != rxlen || d->last.rx_done != rxlen)
                return fail(d, I2CDW_SHORT_TRANSFER);
            if(!disable(d)) return fail(d, I2CDW_DISABLE_TIMEOUT);
            return I2CDW_OK;
        }
        space = d->tx_depth - n;
        while(!stopped && space) {
            u32 cmd;
            if(d->last.tx_queued < txlen) {
                cmd = tx[d->last.tx_queued++];
                if(d->last.tx_queued == txlen && !rxlen) cmd |= CMD_STOP;
            } else if(read_queued < rxlen && read_queued - d->last.rx_done < d->rx_depth) {
                cmd = CMD_READ;
                if(!read_queued && txlen) cmd |= CMD_RESTART;
                if(++read_queued == rxlen) cmd |= CMD_STOP;
            } else break;
            wr(d, IC_DATA_CMD, cmd);
            space--;
        }
    } while(pause_poll(d, &remaining));
    return fail(d, I2CDW_TIMEOUT);
}
