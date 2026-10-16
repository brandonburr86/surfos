/*
SurfOS board: QEMU virt -- the RAM disk through the SD card driver (the kernel word sdsim)
--------------------
File: sdsim.c   Date: 10/15/26 (SurfCPU)
--------------------
QEMU has no SPI controller with an SD card that SurfCPU's driver could talk to. With the
word sdsim on the kernel command line (the test harness passes it in the boot information)
the RAM disk is not rd0 but sd0: the Arty's SD protocol driver (boards/arty_a7/sdcard.c,
the same file the Arty kernel links) drives a simulated SDHC card (boards/arty_a7/tests/
sdmodel.c, byte by byte, CRCs, data tokens, busy time) whose blocks are the RAM disk. So the
partition code, the block cache (with its runs of up to 128 blocks: CMD18 and CMD25),
the VFS and fs/fat.c run over the real SD code path, under the kernel, at -O2 and -O0. What
this cannot test is the Arty's SPI controller and a real card: that is the board's job.

10/16/26 (milestone 8): the block device and the card-detect watch are the Arty's too
(boards/arty_a7/sdblk.c), and the sdsim command works the card model's faults, which is how
boards/qemu_virt/test_os.py makes a write fail or pulls the card out under the kernel:

  sdsim                         the card model's state and counters
  sdsim fault write-error [n]   the next n written blocks are answered "write error" (1)
  sdsim fault no-response [n]   the next n commands get no answer at all (1)
  sdsim fault clear             no more of either
  sdsim pull [n]                the card comes out now, or right after the nth block it accepts
  sdsim insert                  the card goes back in (it lost its power: SD mode until CMD0)

The faults are those of the io track's card model (sim/models/sdcard_model.h, SdFaults
write_error and no_response, set_present()), which surfsim cannot arm yet.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/sync.h>
#include <sys/bdev.h>
#include <mm/kalloc.h>
#include <riscv/board.h>
#include "../arty_a7/sdcard.h"
#include "../arty_a7/sdblk.h"
#include "../arty_a7/tests/sdmodel.h"
#include "../../shell/shell.h"
#include <blibc_common.h>

static struct sdmodel model;
static struct sdblk sd;

static u8 m_xfer(void *ctx, u8 out) { (void)ctx; return sdmodel_xfer(&model, out); }
static void m_select(void *ctx, bool on) { (void)ctx; sdmodel_select(&model, on); }
static void m_slow(void *ctx, bool slow) { (void)ctx; (void)slow; }
static bool m_present(void *ctx) { (void)ctx; return model.present; }
static u32 m_ms(void *ctx) { (void)ctx; return (u32)(board_mtime() / (board_timebase_hz() / 1000)); }
static const struct sd_host host = { NULL, m_xfer, m_select, m_slow, m_present, m_ms, NULL, NULL };

static void announce(struct sdblk *s) {
    kprintf("+SDSIM: %s is the RAM disk through the SD driver: %lu blocks, %s, %s addressing\n", s->disk->name,
            (u_long)s->card.blocks, s->card.crc_on ? "CRC on" : "CRC off", s->card.block_addr ? "block" : "byte");
}

/* the RAM disk image at base, blocks long (a multiple of 1024: an SDHC card's size) */
int qv_sdsim_init(u8 *base, u32 blocks) {
    int r;
    sdmodel_init(&model, base, blocks, 1, 1, 12345);
    sd.tag = "SDSIM";
    sd.host = &host;
    sd.model = "simulated SDHC card (sdsim)";
    sd.announce = announce;
    r = sdblk_attach(&sd);
    sdblk_watch(&sd);
    return r ? -1 : 0;
}

void qv_sdsim_stats(void) {
    printf("    sdsim     %lu reads, %lu writes, %lu CRC retries, %lu timeouts, %lu errors; card saw %lu commands\n",
           sd.card.reads, sd.card.writes, sd.card.crc_retries, sd.card.timeouts, sd.card.errors, model.commands);
}

bool qv_sdsim_active(void) {
    return sd.host != NULL;
}

/**** the sdsim command ****/

/* the model is changed under the card's lock: between two transfers, never inside one (pull n is
   for that) */
int qv_cmd_sdsim(int argc, char **argv) {
    u_long n = 1;
    char *end;
    if(!sd.host) { printf("    sdsim is off (the kernel word sdsim makes the RAM disk a simulated card)\n"); return CMD_FAIL; }
    if(argc == 3 && !strcmp(argv[1], "fault") && !strcmp(argv[2], "clear")) {
        mutex_lock(&sd.lock);
        model.write_errors = model.mute = 0;
        mutex_unlock(&sd.lock);
        printf("    sdsim: no faults armed\n");
        return CMD_OK;
    }
    if(argc >= 3 && !strcmp(argv[1], "fault") && argc <= 4) {
        if(argc == 4) {
            n = strtoul(argv[3], &end, 0);
            if(*end || !n || n > 1000000) { printf("    usage: sdsim fault write-error|no-response [n]   (n 1 to 1000000)\n"); return CMD_USAGE; }
        }
        mutex_lock(&sd.lock);
        if(!strcmp(argv[2], "write-error")) model.write_errors = (int)n;
        else if(!strcmp(argv[2], "no-response")) model.mute = (int)n;
        else { mutex_unlock(&sd.lock); printf("    sdsim: no fault '%s' (write-error, no-response)\n", argv[2]); return CMD_USAGE; }
        mutex_unlock(&sd.lock);
        printf("    sdsim: the next %lu %s %s\n", n, !strcmp(argv[2], "write-error") ? (n == 1 ? "written block is" : "written blocks are")
               : (n == 1 ? "command gets" : "commands get"), !strcmp(argv[2], "write-error") ? "answered \"write error\"" : "no answer");
        return CMD_OK;
    }
    if(argc >= 2 && argc <= 3 && !strcmp(argv[1], "pull")) {
        if(argc == 3) {
            n = strtoul(argv[2], &end, 0);
            if(*end || !n || n > 1000000) { printf("    usage: sdsim pull [n]   (out after the nth block written from now)\n"); return CMD_USAGE; }
            mutex_lock(&sd.lock);
            model.pull_after = (int)n;
            mutex_unlock(&sd.lock);
            printf("    sdsim: the card comes out right after the %lu%s block written from now\n", n,
                   n % 10 == 1 && n % 100 != 11 ? "st" : n % 10 == 2 && n % 100 != 12 ? "nd" : n % 10 == 3 && n % 100 != 13 ? "rd" : "th");
            return CMD_OK;
        }
        mutex_lock(&sd.lock);
        sdmodel_set_present(&model, 0);
        mutex_unlock(&sd.lock);
        printf("    sdsim: the card is out\n");
        return CMD_OK;
    }
    if(argc == 2 && !strcmp(argv[1], "insert")) {
        if(model.present) { printf("    sdsim: the card is in already\n"); return CMD_FAIL; }
        mutex_lock(&sd.lock);
        sdmodel_set_present(&model, 1);
        mutex_unlock(&sd.lock);
        printf("    sdsim: the card is back in\n");
        return CMD_OK;
    }
    if(argc != 1) {
        printf("    usage: sdsim [fault write-error|no-response [n] | fault clear | pull [n] | insert]\n");
        return CMD_USAGE;
    }
    printf("    card %s%s; %lu blocks written, %lu write errors sent, %lu commands, %lu times taken out\n",
           model.present ? "in" : "out", model.present && !model.spi ? " (SD mode: not initialized)" : "",
           model.blocks_written, model.write_errors_sent, model.commands, model.pulls);
    if(model.write_errors || model.mute || model.pull_after)
        printf("    armed: %d write errors, %d commands without an answer, out after %d more blocks\n",
               model.write_errors, model.mute, model.pull_after);
    printf("    driver: %lu reads, %lu writes, %lu CRC retries, %lu timeouts, %lu errors%s%s\n", sd.card.reads,
           sd.card.writes, sd.card.crc_retries, sd.card.timeouts, sd.card.errors, sd.card.why ? "; last problem: " : "",
           sd.card.why ? sd.card.why : "");
    return CMD_OK;
}
