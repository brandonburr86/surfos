#ifndef _NET_WNET_H
#define _NET_WNET_H

/*
    WNET.H
    Written by Martin McCormick (surfos@wazer.net) (c) 2004
    for the Surf Operating System
*/

#include <surfos/types.h>

/* These are the main transmit and receive rings for the network card
   They cannot exceed 256 (sizeof(unsigned char))
   RX_PADDING_SIZE must be less than RX_RING_SIZE (considerably)
*/
#define TX_RING_SIZE    64 //< 256
#define TX_MAXI_SIZE    60 //<= TX_RING_SIZE
#define RX_RING_SIZE    64 //< 256
#define RX_PADDING_SIZE 5  //much < RX RING SIZE
#define TX_INT_FREQ     3  /* default: every third packet */
/* pick a number... any number */
#define AN_ARBITRARY_NUMBER 170 //<256 :-)

struct up_desc {
       unsigned long next;
       unsigned long start_header;
       unsigned char buf[1536]; /* using an implied buffer */
};

struct down_desc {
       unsigned long next;
       unsigned long start_header;
       unsigned long addr;
       unsigned long len;
};


struct sAlphaCard
{
    /* IMPORTANT!!! The Rx and Tx rings MUST be %4-word-aligned- results in PCI Aborts (HOST ERRORS)*/
    struct up_desc   * rx_ring;             /* ! The upload descriptors (for RX) */
    struct down_desc * tx_ring;             /* ! The download descriptors (for TX) */

    struct sPacket * tx_packets[TX_RING_SIZE];  /* ! Array of buffers on TX queue */
    struct sPacket * rx_packets[RX_RING_SIZE];

    /* tx counters */
    unsigned char current_tx_index;
    unsigned char sent_tx_index;

    /* rx counters */
    unsigned char rx_oldest;
    unsigned char rx_newest;

    bool enabled;

    struct pci_dev * pci;

    unsigned char MacAddress[6];
    unsigned char MacMask[6];

    unsigned char if_port;
    unsigned short available_media;             /* From Wn3 Options. */
    unsigned short wn3_mac_ctrl;                /* Current settings. */
    unsigned short capabilities, info1, info2;  /* Various, from EEPROM. */
    unsigned short advertising;                 /* NWay media advertisement */
    unsigned char phys[2];                      /* MII device addresses. */
    int options;                          /* User-settable misc. driver options. */
    unsigned int media_override:4,        /* Passed-in media type. */
        default_media:4,                  /* Read from the EEPROM/Wn3_Config. */
        full_duplex:1, medialock:1, autoselect:1,
        bus_master:1,                     /* Vortex can only do a fragment bus-m. */
        full_bus_master_tx:1, full_bus_master_rx:2, /* Boomerang  */
        hw_csums:1,                       /* Has hardware checksums. */
        restore_intr_mask:1,
        polling:1;

};

/* The following are all commands or offsets for various registers on the network card */

enum Command_Register {
    CommandReg = 0x0e,
    /* commands */
    GlobalReset = (0<<11), SelectWindow = (1<<11), StartCoax = 2<<11,
    RxDisable = 3<<11, RxEnable = 4<<11, RxReset = 5<<11,
    UpStall = 6<<11, UpUnstall = (6<<11)+1,
    DnStall = (6<<11)+2, DnUnstall = (6<<11)+3,
    RxDiscard = 8<<11, TxEnable = 9<<11, TxDisable = 10<<11, TxReset = 11<<11,
    FakeIntr = 12<<11, AckIntr = 13<<11, SetIntrEnb = 14<<11,
    SetStatusEnb = 15<<11, SetRxFilter = 16<<11, SetRxThreshold = 17<<11,
    SetTxThreshold = 18<<11, SetTxStart = 19<<11,
    StartDMAUp = 20<<11, StartDMADown = (20<<11)+1, StatsEnable = 21<<11,
    StatsDisable = 22<<11, StopCoax = 23<<11, SetFilterBit = 25<<11,
};

enum TX_FrameStartHeader_Bits { /* page 94 in manual */
    crcAppendDisable = (1<<13), txIndicate = (1<<15), sh_dnComplete = (1<<16),
    addIpChecksum = (1<<25), addTcpChecksum = (1<<26), addUdpChecksum = (1<<27),
    rndupDefeat = (1<<28), dpdEmpty = (1<<29), dnIndicate = (1<<31)
};

enum RX_FrameStartHeader_Bits { /* page 117 in manual */
    upPktLen = (0x1fff),
    upError = (1<<14), sh_upComplete = (1<<15), upOverrun = (1<<16), runtFrame = (1<<17),
    alignmentError = (1<<18), crcError = (1<<19), oversizedFrame = (1<<20), dribbleBits = (1<<23),
    upOverflow = (1<<24), ipChecksumError = (1<<25), tcpChecksumError = (1<<26),
    udpChecksumError = (1<<27), impliedBufferEnable = (1<<28), ipChecksumChecked = (1<<29),
    tcpChecksumChecked = (1<<30), udpChecksumChecked = (1<<31)
};

enum Master_Control {
    TxPktId = 0x18, PktStatus = 0x20, DnListPtr = 0x24, FragAddr = 0x28, FragLen = 0x2c,
    DnPoll = 0x2d, UpPoll = 0x3d, TxFreeThreshold = 0x2f,
    UpPktStatus = 0x30, UpListPtr = 0x38
};


enum interrupt_status { /* page 135 and 158 in manual */
    interruptLatch = 0x0001, hostError = 0x0002, txComplete = 0x0004,
    txAvailable = 0x0008, rxComplete = 0x0010, rxEarly = 0x0020,
    intRequested = 0x0040, updateStats = 0x0080,
    linkEvent = (1<<8), dnComplete = (1<<9), upComplete = (1<<10),
    DMAInProgress = (1<<11), cmdInProgress = (1<<12),

    interruptLatchAck = (1<<0), rxEarlyAck = (1<<5),
    intRequestedAck = (1<<6), dnCompleteAck = (1<<9), upCompleteAck = (1<<10)

};

enum tx_status
{
    TxStatus = 0x1b,
    txReclaimError = (1<<1), txStatusOverflow = (1<<2), maxCollisions = (1<<3),
    txUnderrun = (1<<4), txJabber = (1<<5), ts_interruptRequested = (1<<6),
    ts_txComplete = (1<<7)
};

#endif
