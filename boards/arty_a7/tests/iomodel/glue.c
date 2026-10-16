/*
SurfOS host test: the SD driver against the io track's card model -- the C side
--------------------
File: glue.c    Date: 10/15/26 (SurfCPU)
--------------------
boards/arty_a7/sdcard.c is C with the kernel's types (bool is a typedef there, a keyword in
C++), so the C++ test (test_sd_iomodel.cpp) reaches it through these plain functions, and
the driver reaches the C++ SPI master through the io_* functions the test defines.
*/

#include "sdcard.h"

unsigned char io_xfer(unsigned char out);      /* test_sd_iomodel.cpp */
void io_select(int on);
void io_slow(int slow);
int io_present(void);
unsigned int io_ms(void);

static u8 h_xfer(void *ctx, u8 out) { (void)ctx; return io_xfer(out); }
static void h_select(void *ctx, bool on) { (void)ctx; io_select(on != 0); }
static void h_slow(void *ctx, bool slow) { (void)ctx; io_slow(slow != 0); }
static bool h_present(void *ctx) { (void)ctx; return io_present() != 0; }
static u32 h_ms(void *ctx) { (void)ctx; return io_ms(); }

static const struct sd_host host = { 0, h_xfer, h_select, h_slow, h_present, h_ms, 0, 0 };
static struct sdcard card;

int glue_init(void) { return sd_init(&card, &host); }
int glue_read(unsigned int lba, unsigned int count, unsigned char *buf) { return sd_read(&card, lba, count, buf); }
int glue_write(unsigned int lba, unsigned int count, const unsigned char *buf) { return sd_write(&card, lba, count, buf); }
unsigned long long glue_blocks(void) { return card.blocks; }
int glue_block_addr(void) { return card.block_addr != 0; }
int glue_v2(void) { return card.v2 != 0; }
int glue_crc_on(void) { return card.crc_on != 0; }
unsigned long glue_crc_retries(void) { return card.crc_retries; }
const char *glue_why(void) { return card.why ? card.why : "-"; }
const char *glue_strerror(int err) { return sd_strerror(err); }
