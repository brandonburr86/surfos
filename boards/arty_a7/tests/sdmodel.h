/*
SurfOS host test: a simulated SD card in SPI mode
----------------------
File: sdmodel.h Date: 10/15/26 (SurfCPU)
----------------------
The card side of the byte exchange, over a disk image in memory, for testing
boards/arty_a7/sdcard.c on the host: commands with their CRC7 (always for CMD0 and CMD8,
for all once CMD59 turned checking on), R1/R2/R3/R7, the idle state until ACMD41,
version 1 and version 2 cards, standard (byte-addressed) and high capacity (block-addressed)
ones, CSD and CID, single and multiple block reads (CMD18 streams until CMD12) and writes
(CMD24, CMD25 with 0xFC/0xFD tokens) with data responses and busy time, CMD13, address
errors (R1 0x40 past the end), random response delays, data responses as the io track's card
model sends them (0xE5, 0xEB, 0xED, busy after each), and faults on request: a corrupted
read CRC, a data token that never comes, a rejected write CRC, no card. Like that model it
counts the host's mistakes: a command while the card is busy (ignored, as a card would), and
chip select released inside a written block.

10/16/26 (milestone 8): the io track's write faults as well (sim/models/sdcard_model.h,
SdFaults.write_error and no_response): a block answered "write error" (0xED, and an error in
CMD13's status after it) and commands that get no answer; and a card taken out and put back
(sdmodel_set_present(), as its set_present()): out, MISO stays high; back in, the card has
lost its power, so it is in SD mode and answers nothing but CMD0, which with CS low puts it
in SPI mode. pull_after takes it out right after the nth block it accepts, in the middle of
whatever the host was doing, which is how the kernel's tests pull it at a known point.
10/16/26 (the board): a command that starts in the byte right after a response, with no 8 clocks
in between (N_RC in the specification's SPI timing), is a host mistake too. The owner's SanDisk
card answered such an ACMD41 (sent straight after CMD55's R1) with a parameter error, which no
model had shown.
*/

#ifndef _SDMODEL_H
#define _SDMODEL_H

#include <stdint.h>

struct sdmodel {
    uint8_t *img;
    uint32_t blocks;
    int present, v2, high_capacity;
    unsigned seed;                      /* the delays (NCR, NAC, busy) */
    /* faults */
    int corrupt_reads;                  /* the next n data blocks sent have a wrong CRC */
    int no_token;                       /* reads never send their data token */
    int reject_writes;                  /* the next n written blocks get a CRC error response */
    int no_multi;                       /* CMD18 and CMD25 are illegal commands (as in a card without them) */
    int write_errors;                   /* the next n written blocks are answered "write error" and not kept */
    int mute;                           /* the next n commands get no response at all */
    int pull_after;                     /* > 0: the card comes out right after the nth block it accepts from now */
    /* state */
    int spi;                            /* in SPI mode: CMD0 with CS low after the card got power */
    int selected, idle, crc_on, app, acmd41_left;
    uint8_t status2;                    /* CMD13's second byte: errors since the last CMD13 (0x04 a write error) */
    int mode;                           /* receiving a command, a write token, write data */
    int multi;                          /* CMD18 streaming, or CMD25 */
    uint32_t block;                     /* the next block of a transfer */
    uint8_t cmd[6];
    int cmd_len;
    uint8_t out[1100];
    int out_len, out_pos;
    int busy;                           /* bytes of busy (0x00) still to send */
    uint8_t wbuf[514];
    int wpos;
    unsigned long commands, crc_errors_seen, bytes;
    unsigned long blocks_written, write_errors_sent, pulls;
    unsigned long violations;           /* the host's mistakes (above): the tests want none */
    int resp_end;                       /* the last byte out ended a response: the next byte must not start a command */
    unsigned long nrc_violations;       /* commands sent without 8 clocks after the last response (N_RC), also in violations */
};

void sdmodel_init(struct sdmodel *m, uint8_t *img, uint32_t blocks, int v2, int high_capacity, unsigned seed);
uint8_t sdmodel_xfer(struct sdmodel *m, uint8_t in);
void sdmodel_select(struct sdmodel *m, int on);
void sdmodel_set_present(struct sdmodel *m, int present);   /* taken out (0) or put back (1): power lost either way */

#endif
