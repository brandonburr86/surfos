/* DesignWare register-level model: real engine, finite FIFOs and delayed bus
   progress. These tests catch excess read requests, lost abort diagnostics,
   early STOP completion, and waits that do not terminate. */
#include <sys/i2cdw.h>

int printf(const char *format, ...);
void exit(int status);

enum {
    CON=0x00, TAR=0x04, DATA=0x10, SS_HCNT=0x14, SS_LCNT=0x18,
    FS_HCNT=0x1c, FS_LCNT=0x20, MASK=0x30, RAW=0x34,
    RX_TL=0x38, TX_TL=0x3c, CLR_INTR=0x40, CLR_ABORT=0x54,
    CLR_STOP=0x60, ENABLE=0x6c, STATUS=0x70, TXFLR=0x74,
    RXFLR=0x78, SDA_HOLD=0x7c, ABORT_SOURCE=0x80, DMA_CR=0x88,
    ENABLE_STATUS=0x9c, PARAM=0xf4, VERSION=0xf8, TYPE=0xfc
};
struct fake {
    u32 reg[64], tx[256], log[1024];
    u8 rx[256];
    unsigned tx_count, rx_count, log_count, executed, generated;
    unsigned tx_depth, rx_depth, outstanding, max_outstanding;
    unsigned ticks, elapsed, writes, violations, config_writes;
    unsigned nack_at, drop_at, burst;
    u32 inject_raw;
    bool stalled, disable_stuck, enable_stuck, abort_stuck;
    bool external_busy, hold_busy, omit_stop;
};
static unsigned checks, failures;
static void check(bool ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}
static void setup(struct fake *f, unsigned tx_depth, unsigned rx_depth) {
    *f = (struct fake){0};
    f->tx_depth = tx_depth; f->rx_depth = rx_depth;
    f->burst = 1;
    f->reg[TYPE/4] = 0x44570140;
    f->reg[VERSION/4] = 0x3131312a;
    f->reg[PARAM/4] = ((tx_depth-1)<<16) | ((rx_depth-1)<<8) | 0x8c;
}
static u32 read_reg(void *ctx, u32 reg) {
    struct fake *f = ctx;
    unsigned i;
    u32 value;
    if(reg == TXFLR) return f->tx_count;
    if(reg == RXFLR) return f->rx_count;
    if(reg == STATUS) return f->reg[STATUS/4] | (f->external_busy ? 1 : 0);
    if(reg == DATA) {
        if(!f->rx_count) { f->violations++; return 0; }
        value = f->rx[0];
        for(i=1; i<f->rx_count; i++) f->rx[i-1] = f->rx[i];
        f->rx_count--; f->outstanding--;
        return value;
    }
    if(reg == CLR_INTR) {
        f->reg[RAW/4] = f->reg[ABORT_SOURCE/4] = 0;
        return 0;
    }
    if(reg == CLR_ABORT) {
        f->reg[RAW/4] &= ~0x40;
        f->reg[ABORT_SOURCE/4] = 0;
        return 0;
    }
    if(reg == CLR_STOP) { f->reg[RAW/4] &= ~0x200; return 0; }
    return f->reg[reg/4];
}
static void write_reg(void *ctx, u32 reg, u32 value) {
    struct fake *f = ctx;
    f->writes++;
    if(reg == MASK && value) f->violations++;
    if(reg == CON || reg == TAR || reg == SS_HCNT || reg == SS_LCNT ||
       reg == FS_HCNT || reg == FS_LCNT || reg == SDA_HOLD) {
        f->config_writes++;
        if(f->reg[ENABLE_STATUS/4]) f->violations++;
    }
    if(reg == DATA) {
        if(!f->reg[ENABLE_STATUS/4] || f->tx_count == f->tx_depth) {
            f->violations++; return;
        }
        if(f->log_count == 1024) { printf("FAIL: runaway command loop\n"); exit(1); }
        f->tx[f->tx_count++] = value;
        f->log[f->log_count++] = value;
        f->reg[STATUS/4] = 0x21;
        if(value & 0x100) {
            f->outstanding++;
            if(f->outstanding > f->max_outstanding) f->max_outstanding = f->outstanding;
        }
        return;
    }
    f->reg[reg/4] = value;
    if(reg == ENABLE) {
        if(value & 2) {
            if(!f->abort_stuck) {
                f->reg[ENABLE/4] &= ~2;
                f->tx_count = f->rx_count = f->outstanding = 0;
                f->reg[STATUS/4] = 0;
                f->reg[RAW/4] |= 0x40;
                f->reg[ABORT_SOURCE/4] = 0x10000;
            }
        } else if(value & 1) {
            if(!f->enable_stuck) f->reg[ENABLE_STATUS/4] = 1;
        } else if(!f->disable_stuck) {
            f->reg[ENABLE_STATUS/4] = 0;
            f->tx_count = f->rx_count = f->outstanding = 0;
            f->reg[STATUS/4] = 0;
        }
    }
}
static void advance(struct fake *f) {
    u32 cmd;
    unsigned i;
    if(f->stalled || !f->tx_count || !f->reg[ENABLE_STATUS/4]) return;
    cmd = f->tx[0];
    for(i=1; i<f->tx_count; i++) f->tx[i-1] = f->tx[i];
    f->tx_count--; f->executed++;
    f->reg[RAW/4] |= f->inject_raw;
    if(f->nack_at == f->executed) {
        f->reg[RAW/4] |= 0x40; f->reg[ABORT_SOURCE/4] = 1;
        f->tx_count = 0; f->reg[STATUS/4] = 0;
        return;
    }
    if(cmd & 0x100) {
        f->generated++;
        if(f->generated != f->drop_at) {
            if(f->rx_count == f->rx_depth) f->violations++;
            else f->rx[f->rx_count++] = (u8)f->generated;
        }
    }
    if(cmd & 0x200) {
        if(!f->omit_stop) f->reg[RAW/4] |= 0x200;
        f->reg[STATUS/4] = f->hold_busy ? 0x21 : 0;
    }
}
static void delay(void *ctx, u32 us) {
    struct fake *f = ctx;
    unsigned i;
    f->ticks++; f->elapsed += us;
    if(f->ticks > 10000) { printf("FAIL: unbounded wait\n"); exit(1); }
    for(i=0; i<f->burst; i++) advance(f);
}
static struct i2cdw_io io_for(struct fake *f) {
    struct i2cdw_io io = { f, read_reg, write_reg, delay };
    return io;
}
static struct i2cdw_config config(void) {
    struct i2cdw_config cfg = { I2CDW_FAST, 120, 160, 0x10006, 5, 250 };
    return cfg;
}
static enum i2cdw_error init(struct fake *f, struct i2cdw *d) {
    struct i2cdw_io io = io_for(f);
    struct i2cdw_config cfg = config();
    return i2cdw_init(d, &io, &cfg);
}
static void test_init(void) {
    struct fake f;
    struct i2cdw d = {0};
    struct i2cdw_io io;
    struct i2cdw_config cfg = config();
    setup(&f, 4, 2);
    f.reg[DMA_CR/4] = 3;
    check(init(&f, &d) == I2CDW_OK && d.ready, "supported controller initializes");
    check(d.tx_depth == 4 && d.rx_depth == 2, "independent FIFO depths discovered");
    check(f.reg[CON/4] == 0x65 && f.reg[FS_HCNT/4] == 120 &&
          f.reg[FS_LCNT/4] == 160 && f.reg[SDA_HOLD/4] == 0x10006,
          "master/restart/7-bit mode and supplied fast timing programmed");
    check(!f.reg[ENABLE_STATUS/4] && !f.reg[MASK/4] && !f.violations,
          "initialization leaves disabled with interrupts masked");
    check(!f.reg[DMA_CR/4], "PIO initialization disables leftover DMA requests");
    setup(&f, 1, 256); io = io_for(&f); cfg.speed = I2CDW_STANDARD;
    check(i2cdw_init(&d, &io, &cfg) == I2CDW_OK && d.tx_depth == 1 && d.rx_depth == 256 &&
          f.reg[CON/4] == 0x63 && f.reg[SS_HCNT/4] == 120 && f.reg[SS_LCNT/4] == 160,
          "FIFO depth extrema and standard-mode timing supported");
    setup(&f, 4, 2); f.reg[TYPE/4] = 0xffffffff;
    check(init(&f, &d) == I2CDW_UNSUPPORTED && !d.ready && !f.writes,
          "unmapped or foreign register block rejected before writes");
    setup(&f, 4, 2); f.disable_stuck = true; f.reg[ENABLE_STATUS/4] = 1;
    check(init(&f, &d) == I2CDW_DISABLE_TIMEOUT && !d.ready && !f.config_writes && f.elapsed <= 250,
          "stuck disable bounded and configuration not changed live");
}
static void test_transfers(void) {
    struct fake f;
    struct i2cdw d;
    const u8 tx[] = { 0x20, 0, 3, 4, 5, 6, 7, 8, 9 };
    u8 rx[13] = {0};
    unsigned i;
    setup(&f, 4, 2); init(&f, &d);
    check(i2cdw_xfer(&d, 0x2c, tx, 2, rx, sizeof(rx)) == I2CDW_OK,
          "combined transaction larger than both FIFOs completes");
    check(f.log_count == 15 && f.log[0] == 0x20 && f.log[1] == 0 &&
          f.log[2] == 0x500 && f.log[13] == 0x100 && f.log[14] == 0x300,
          "one repeated START at read boundary and one final STOP");
    for(i=0; i<sizeof(rx); i++) check(rx[i] == i+1, "combined read preserves byte order");
    check(f.max_outstanding == 2 && !f.violations,
          "outstanding reads never exceed receive capacity");
    check(d.last.tx_queued == 2 && d.last.rx_done == 13 && !d.last.abort_source &&
          !f.reg[ENABLE_STATUS/4] && !f.reg[MASK/4] && d.ready && f.reg[TAR/4] == 0x2c,
          "success reports counts and leaves disabled reusable controller");
    setup(&f, 2, 4); init(&f, &d);
    check(i2cdw_xfer(&d, 0x31, tx, sizeof(tx), 0, 0) == I2CDW_OK &&
          f.log_count == 9 && f.log[0] == 0x20 && f.log[8] == 0x209 && !f.violations,
          "write-only transfer larger than transmit FIFO puts STOP on final byte");
    setup(&f, 1, 1); init(&f, &d);
    check(i2cdw_xfer(&d, 0x12, 0, 0, rx, 5) == I2CDW_OK &&
          f.log_count == 5 && f.log[0] == 0x100 && f.log[4] == 0x300 &&
          f.max_outstanding == 1 && !f.violations,
          "read-only transfer works with one-entry FIFOs");
    setup(&f, 2, 2); init(&f, &d);
    check(i2cdw_xfer(&d, 0x12, tx, 1, rx, 1) == I2CDW_OK && f.log[1] == 0x700,
          "single combined read carries both RESTART and STOP");
    setup(&f, 4, 2); init(&f, &d); f.burst = 8;
    check(i2cdw_xfer(&d, 0x2c, tx, 2, rx, sizeof(rx)) == I2CDW_OK &&
          rx[12] == 13 && !f.violations && f.max_outstanding == 2,
          "whole receive FIFO drains when the bus completes a batch between polls");
    setup(&f, 4, 2); init(&f, &d);
    check(i2cdw_xfer(&d, 0x2c, tx, sizeof(tx), rx, sizeof(rx)) == I2CDW_OK &&
          f.log_count == 22 && f.log[8] == 9 && f.log[9] == 0x500 && f.log[21] == 0x300 &&
          !f.violations, "long combined prefix survives transmit FIFO refill before restarting");
    setup(&f, 2, 2); init(&f, &d);
    f.reg[RAW/4] = 0x240; f.reg[ABORT_SOURCE/4] = 1;
    check(i2cdw_xfer(&d, 0x2c, tx, 1, 0, 0) == I2CDW_OK && !d.last.abort_source,
          "stale events are cleared while idle before starting a new transaction");
}
static void test_failures(void) {
    struct fake f;
    struct i2cdw d;
    const u8 tx[] = { 0x20, 0 };
    u8 rx[4] = { 0xaa, 0xaa, 0xaa, 0xaa };
    unsigned writes;
    setup(&f, 4, 2); init(&f, &d); f.nack_at = 1;
    check(i2cdw_xfer(&d, 0x2c, tx, 2, rx, 4) == I2CDW_ABORT &&
          d.last.abort_source == 1 && d.last.recovered && d.ready &&
          !f.reg[ENABLE_STATUS/4], "NACK reason survives recovery");
    /* 10/15/26: the master is idle after a NACK; recovery must not depend on an
       abort request completing (the laptop's touchpad NACKs a GET_REPORT). */
    setup(&f, 4, 2); init(&f, &d); f.nack_at = 2; f.abort_stuck = true;
    writes = f.log_count;
    check(i2cdw_xfer(&d, 0x2c, tx, 2, rx, 4) == I2CDW_ABORT && d.last.abort_source == 1 &&
          d.last.recovered && d.ready && !(f.reg[ENABLE/4] & 2) && f.elapsed < 200,
          "a NACK needs no abort request: recovery is immediate");
    f.nack_at = 0;
    check(i2cdw_xfer(&d, 0x2c, tx, 1, 0, 0) == I2CDW_OK && f.log_count > writes,
          "the controller works again after a NACK");
    setup(&f, 2, 2); init(&f, &d); f.drop_at = 4;
    check(i2cdw_xfer(&d, 0x2c, 0, 0, rx, 4) == I2CDW_SHORT_TRANSFER &&
          d.last.rx_done == 3 && rx[3] == 0xaa && d.last.recovered,
          "STOP with missing data fails and exposes only actual received bytes");
    setup(&f, 2, 2); init(&f, &d); f.stalled = true;
    check(i2cdw_xfer(&d, 0x2c, tx, 2, 0, 0) == I2CDW_TIMEOUT &&
          f.elapsed <= 750 && d.last.recovered && d.ready,
          "stuck transfer times out and aborts within bounded cleanup");
    f.stalled = false;
    check(i2cdw_xfer(&d, 0x2c, tx, 1, 0, 0) == I2CDW_OK,
          "successful recovery permits a later independent transaction");
    setup(&f, 2, 2); init(&f, &d); f.stalled = f.abort_stuck = true;
    check(i2cdw_xfer(&d, 0x2c, tx, 2, 0, 0) == I2CDW_TIMEOUT &&
          !d.last.recovered && !d.ready && f.elapsed <= 750,
          "failed abort recovery retires controller without an unbounded wait");
    writes = f.writes;
    check(i2cdw_xfer(&d, 0x2c, tx, 1, 0, 0) == I2CDW_NOT_READY && f.writes == writes,
          "retired controller cannot start a new transaction");
    setup(&f, 2, 2); init(&f, &d); f.enable_stuck = true;
    check(i2cdw_xfer(&d, 0x2c, tx, 2, 0, 0) == I2CDW_ENABLE_TIMEOUT &&
          !f.log_count && f.elapsed <= 750, "enable timeout cannot queue data");
    setup(&f, 2, 2); init(&f, &d); f.external_busy = true;
    check(i2cdw_xfer(&d, 0x2c, tx, 2, 0, 0) == I2CDW_BUS_BUSY &&
          !f.log_count && f.elapsed <= 750, "busy bus bounded before addressing device");
    setup(&f, 2, 2); init(&f, &d); f.omit_stop = true;
    check(i2cdw_xfer(&d, 0x2c, 0, 0, rx, 4) == I2CDW_TIMEOUT && d.last.rx_done == 4,
          "receiving requested bytes without STOP is not success");
    setup(&f, 2, 2); init(&f, &d); f.hold_busy = true;
    check(i2cdw_xfer(&d, 0x2c, tx, 2, 0, 0) == I2CDW_TIMEOUT,
          "STOP without idle is not success");
    setup(&f, 2, 2); init(&f, &d); f.inject_raw = 2;
    check(i2cdw_xfer(&d, 0x2c, 0, 0, rx, 4) == I2CDW_FIFO_ERROR && d.last.recovered,
          "receive overflow is an error instead of silently returning corrupt bytes");
    setup(&f, 2, 2); init(&f, &d); f.nack_at = 2;
    check(i2cdw_xfer(&d, 0x2c, tx, 2, rx, 4) == I2CDW_ABORT &&
          d.last.abort_source == 1 && !d.last.rx_done,
          "abort after transmission begins is not mistaken for a short success");
}
static void test_bad_args(void) {
    struct fake f;
    struct i2cdw d = {0};
    struct i2cdw_io io;
    struct i2cdw_config cfg;
    u8 byte = 0;
    unsigned writes, i;
    setup(&f, 2, 2); init(&f, &d); writes = f.writes;
    check(i2cdw_xfer(0, 0x20, &byte, 1, 0, 0) == I2CDW_BAD_ARG, "null engine rejected");
    check(i2cdw_xfer(&d, 0x80, &byte, 1, 0, 0) == I2CDW_BAD_ARG, "10-bit address rejected");
    check(i2cdw_xfer(&d, 0x20, 0, 1, 0, 0) == I2CDW_BAD_ARG, "null write buffer rejected");
    check(i2cdw_xfer(&d, 0x20, 0, 0, 0, 1) == I2CDW_BAD_ARG, "null read buffer rejected");
    check(i2cdw_xfer(&d, 0x20, 0, 0, 0, 0) == I2CDW_BAD_ARG, "empty transaction rejected");
    check(f.writes == writes, "invalid transfers make no hardware writes");
    for(i=0; i<7; i++) {
        setup(&f, 2, 2); io = io_for(&f); cfg = config();
        if(i == 0) cfg.hcnt = 0;
        if(i == 1) cfg.lcnt = 0x10000;
        if(i == 2) cfg.speed = 3;
        if(i == 3) cfg.poll_us = 0;
        if(i == 4) cfg.timeout_us = 0;
        if(i == 5) cfg.poll_us = cfg.timeout_us+1;
        if(i == 6) io.delay_us = 0;
        check(i2cdw_init(&d, &io, &cfg) == I2CDW_BAD_ARG && !f.writes,
              "invalid configuration rejected before touching hardware");
    }
    setup(&f, 2, 2); f.reg[VERSION/4] = 0x3131302a;
    check(init(&f, &d) == I2CDW_UNSUPPORTED && !f.writes,
          "revision without programmable SDA hold is not misconfigured");
}
int main(void) {
    test_init();
    test_transfers();
    test_failures();
    test_bad_args();
    printf("i2cdw: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
