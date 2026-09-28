/*
    WNET.C
    Written by Martin McCormick (surfos@wazer.net) (c) 2004
    for the Surf Operating System
*/

/* stuff:
    -Any non standard interrupt errors etc.. should be handled in seperate function (cache)
    -Redo setup of tranceiver / autoselect
    -Statistic support
    -Handle RxEarly interrupts
    -Cut down on dnComplete / upComplete
    -

*/

/* PROTOTYPES, HEADERS AND EXTERNAL VARIABLES */
#include <surfos/types.h>

#include <surfos/interrupts.h>
#include <surfos/interrupt.h>
#include <mm/memory.h>

#include <sys/pci.h>
#include <asm/io.h>

#include <net/wnet.h>
#include <net/ethernet.h>

#include <blibc_common.h>

char mii_preamble_required = 0; //mdio preamble : some need.

/* END PROTOTYPES, HEADERS AND EXTERNAL VARIABLES */



/* OUR DRIVER FUNCTIONS and HEADERS */
unsigned int g_counter;
int do_eeprom_op(unsigned long ioaddr, int ee_cmd); /* querying EEPROM of card */
struct iface * alphaSetup(struct pci_dev * pci);
int alphaSetting(struct iface * interface, int iSetting, void * param);
int alphaQueryCard(struct sAlphaCard * pcard);
int alphaGetPackets(struct iface * interface, struct sPacket ** packets, unsigned int * count);
int alphaSendPackets(struct iface * interface, struct sPacket ** packets, unsigned int * count);
void alphaSelectXmit(struct sAlphaCard * pcard);
void set_media_type(struct sAlphaCard * pcard);
void mdio_write(long ioaddr, int phy_id, int location, int value);
long mdio_read(long ioaddr, int phy_id, int location);
void mdio_sync(long ioaddr, int bits);
struct sAlphaCard * gcard;
struct iface * gface;
/* END OUR DRIVER FUNCTIONS */



/********************************************************/

void * virt_to_phys(void * linear)
{
    return mm_lookup_linear(linear);
}


int do_eeprom_op(unsigned long ioaddr, int ee_cmd)
{
    int timer;

    outw(ioaddr + 10, ee_cmd);
    /* Wait for the read to take place, worst-case 162 us. */

    for (timer = 1620; timer >= 0; timer--)
        if ((inw(ioaddr + 10) & 0x8000) == 0)
            break;

    return inw(ioaddr + 12);
}

int alphaInterrupt(unsigned int irq, void * param)
{
    struct sAlphaCard * pcard;
    pcard = (struct sAlphaCard *) param;

    unsigned long ioaddr = pcard->pci->begin[0];
    unsigned int cur_packet; /* used as a temp indexing variable to reffer to a packet */
    unsigned int intAck;
    unsigned int status;
    unsigned char pkt_id;

    unsigned int temp=0;
    kprintf("HARD:IN");
    while( ((status = inw(ioaddr + CommandReg)) & (interruptLatch | upComplete)) && status != 0xffff) /* proccess everything in status */
    {
       kprintf("IN(0x%x)",status);
        /* always ack latch */
        intAck = AckIntr | interruptLatchAck;

        if (status & dnComplete) { /* transmitted a packet */
        /*TX*/  intAck |= dnCompleteAck;
        /*TX*/
        /*TX*/  /* start our with the last packet already sent */
        /*TX*/  cur_packet = pcard->sent_tx_index;  //kprintf("TX INT IN (sti=%d", cur packet);
        /*TX*/  while(cur_packet != pcard->current_tx_index)
        /*TX*/  {
        /*TX*/      cur_packet = (cur_packet + 1) % TX_RING_SIZE; /* select next */
        /*TX*/      //kprintf("(%d), ",cur packet);
        /*TX*/      if(!(pcard->tx_ring[cur_packet].start_header & sh_dnComplete))
        /*TX*/      {/* this packet wasn't downloaded, we've caught up. */
        /*TX*/         cur_packet = (cur_packet - 1) % TX_RING_SIZE; /* move back */
        /*TX*/         break;
        /*TX*/      }
        /*TX*/
        /*TX*/      if (!(pcard->tx_ring[cur_packet].start_header & dpdEmpty)) /* not empty */
        /*TX*/      {
        /*TX*/          /* find ID of  packet */
        /*TX*/          pkt_id = (pcard->tx_ring[cur_packet].start_header >> 2) & 0xff;
        /*TX*/          if (pkt_id < TX_RING_SIZE);
        /*TX*/             pcard->tx_packets[pkt_id]->iFlags |= PACKET_SENT; /* set sent */
        /*TX*/      }
        /*TX*/  }
                kprintf("end with=%d\n", cur_packet);
        /*TX*/  pcard->sent_tx_index = cur_packet;
        }

        if (status & upComplete) { /* received a packet */
        /*RX*/  intAck |= upCompleteAck;
        /*RX*/  /* This is cleanup for packets that were sent or unused on the end of the ring */
        /*RX*/  /* scan through from oldest to newest, and free marked done, move up rx oldest */
        /*RX*/  cur_packet = pcard->rx_oldest;
        /*RX*/  while(cur_packet != pcard->rx_newest) /* all the way until done */
        /*RX*/  {
        /*RX*/      if( (pcard->rx_packets[cur_packet]->iFlags & PACKET_SENT) ||      /* sent successfully */
        /*RX*/          (pcard->rx_packets[cur_packet]->iFlags & PACKET_LOST) ||      /* marked lost */
        /*RX*/          (pcard->rx_ring[cur_packet].start_header & upError) )         /* error */
        /*RX*/      {   /* if so, we can free it */
        /*RX*/          pcard->rx_packets[cur_packet]->iFlags =
        /*RX*/          pcard->rx_packets[cur_packet]->iLen   = 0; /* reset flags, buffer and length */
        /*RX*/
        /*RX*/          pcard->rx_ring[cur_packet].start_header = impliedBufferEnable; /* reset upd */
        /*RX*/      }else{ /* can't free, this is new rx_oldest */
        /*RX*/          break;
        /*RX*/      }
        /*RX*/
        /*RX*/      cur_packet = (cur_packet + 1) % RX_RING_SIZE; /* go to next in ring */
        /*RX*/  }
        /*RX*/  pcard->rx_oldest = cur_packet; /* set rx_oldest to the one we couldn't free */
        /*RX*/
        /*RX*/  /* must process the new packet(s) on the ring... and move up rx newest accordingly */
        /*RX*/  /* NOTE: cur packet is being used for a new loop now */
        /*RX*/  cur_packet = pcard->rx_newest; /* the 'oldest' newest (will change ;-) */
        /*RX*/  while(pcard->rx_ring[cur_packet].start_header & sh_upComplete)
        /*RX*/  {
        /*RX*/      /* check for errors */
        /*RX*/      if(pcard->rx_ring[cur_packet].start_header & upError)
        /*RX*/      {
        /*RX*/          kprintf("Packet ERROR! (0x%x)\n", pcard->rx_ring[cur_packet].start_header);//HandleRxError(cur_packet);
        /*RX*/      } else {/* no err */
        /*RX*/          /* set all checksum information in corresponding packet struct */
        /*RX*/          pcard->rx_packets[cur_packet]->iFlags =
        /*RX*/           ( (pcard->rx_ring[cur_packet].start_header & ipChecksumChecked) ? IP_SUM_CHECKED : 0) |
        /*RX*/           ( (pcard->rx_ring[cur_packet].start_header & tcpChecksumChecked) ? TRANS_SUM_CHECKED : 0) |
        /*RX*/           ( (pcard->rx_ring[cur_packet].start_header & udpChecksumChecked) ? TRANS_SUM_CHECKED : 0) |
        /*RX*/           ( (pcard->rx_ring[cur_packet].start_header & ipChecksumError) ? 0 : IP_SUM_CORRECT) |
        /*RX*/           ( (pcard->rx_ring[cur_packet].start_header & (udpChecksumError | tcpChecksumError)) ?
        /*RX*/             0 : TRANS_SUM_CORRECT ) | PACKET_SYNCHED;
        /*RX*/
        /*RX*/          /* synch length */
        /*RX*/          pcard->rx_packets[cur_packet]->iLen =
        /*RX*/          pcard->rx_ring[cur_packet].start_header & upPktLen;
        /*RX*/      }
        /*RX*/
        /*RX*/      /* check for upcoming interference */
        /*RX*/      if (((cur_packet + RX_PADDING_SIZE) % RX_RING_SIZE) == pcard->rx_oldest) /* nearing a rear ending */
        /*RX*/          pcard->rx_packets[pcard->rx_oldest]->iFlags |= PACKET_LOST; /* mark lost - will be freed next time */
        /*RX*/
        /*RX*/      /* increment cur packet for next */
        /*RX*/      cur_packet = (cur_packet + 1) % RX_RING_SIZE;
        /*RX*/      /* check to make sure not looping all the way around (infinite loop) */
        /*RX*/      if(cur_packet == pcard->rx_oldest) {
                        kprintf("\nOVERRUN!!!\n");
                        break;
                        }
        /*RX*/                 /* NOTE: This will only occur under very heavy traffic;       */
        /*RX*/                 /* there are more packets being received than can be handled. */
        /*RX*/                 /* Because the packet before rx_oldest has been marked used   */
        /*RX*/                 /* by the card, and that the card cannot use rx oldest, it    */
        /*RX*/                 /* is probabably polling on rx_oldest to be complete.  Packets*/
        /*RX*/                 /* may have already been lost.
          */
        /*RX*/  }
        /*RX*/  pcard->rx_newest = cur_packet;
        }


        if (status & txComplete)
        { /* Normally this interrupt is internally ack'd, except in ERROR */
            bool bNeedEnable = false, bNeedReset = false;
            unsigned char txstatus = inb(ioaddr + TxStatus); /* read status */
            outb(ioaddr + TxStatus, AN_ARBITRARY_NUMBER);    /* and clear it */

            kprintf("TXERROR=0x%x\n",txstatus);

            if(txstatus & txReclaimError)
                bNeedEnable = true;
            if(txstatus & txStatusOverflow)
                bNeedEnable = true;
            if(txstatus & maxCollisions)
            {
                bNeedEnable = true;
                /* TODO: resend the packet */
            }
            if(txstatus & (txUnderrun | txJabber))
            {
                bNeedEnable = true;
                bNeedReset = true;
            }
            if(txstatus & ts_interruptRequested)
                bNeedEnable = true;

            if(bNeedReset)
            {
                int i;
                outw(ioaddr + CommandReg, TxReset);
                for (i = 2000; i; i--) /* wait to finish */
                    if (!(inw(ioaddr + CommandReg) & cmdInProgress))
                       break;
                outl(ioaddr + DnListPtr, (unsigned long)virt_to_phys(&pcard->tx_ring[0])); /* set main dpd */
            }

           if(bNeedEnable)
                outw(ioaddr + CommandReg, TxEnable);

        }

        if (status & intRequested) {
            intAck |= intRequestedAck;
            kprintf("INT REQ!\n");
        }

        if (status & rxEarly) {
            intAck |= rxEarlyAck;
            kprintf("RXEARLY!\n");
        }

        if (status & hostError) {
            kprintf("HOST ERROR!\n");
        }

        if (status & updateStats) {
            kprintf("!Status Full!\n");
        }

        g_counter++;

        outw(ioaddr + CommandReg, intAck); /* ack it (free up) */
        kprintf("OUT(0x%x)",status);
    }
    kprintf("HARD:OUT\n");
    return;
}


extern FuncIRQHandler parPortISR();
/* alphaSetup -
Called once at startup, allocates buffers ect.. */
struct iface * alphaSetup(struct pci_dev * ppci)
{
    struct iface * interface;           /* returned by this function */
    struct sAlphaCard * pcard;          /* internal structure used by driver */
    unsigned long ioaddr = ppci->begin[0];              /* used for io */
    register unsigned int i;    /* used mostly for loop index and temp storage */
    unsigned char * pRingBase;  /* the base of the block of memory used for upload (rx) and download (tx) descriptors */
    unsigned char * pPacketMemBase; /* Base address for packet structures */
    unsigned char cpciLatency;
    unsigned char cNewLatency;


    g_counter = 0;

    if( !(interface = (struct iface *)kalloc(sizeof(struct iface))) )
    {
        kprintf("NET: IFACE ALLOCATION FAILED!\n");
        return 0;
    }
    if( !(pcard = (struct sAlphaCard *)kalloc(sizeof(struct sAlphaCard))) )
    {
        kprintf("NET: INTERNAL CARD STRUCT ALLOCATION FAILED!\n");
        return 0;
    }

  /* fill out interface */
    interface->id = 0; /* set by caller */
    interface->driver_struct = (void *)pcard;
    interface->Setting = (FuncSetting)&alphaSetting;
    interface->SnagPackets = (FuncSnag)&alphaGetPackets;
    interface->SendPackets = (FuncSend)&alphaSendPackets;

  /* fill out pcard */
    pcard->pci = ppci;
    pcard->enabled = false; /* set to disabled */

    outw(ioaddr + CommandReg, GlobalReset); /* fully reset network card for good measure */
    for (i = 1000; i; i--) /* wait to finish */
        if (!(inw(ioaddr + CommandReg) & cmdInProgress))
            break;
    //if(!i) kprintf("NET: WARNING!  Long GlobalReset()!\n");

    outl(ioaddr + UpListPtr, 0); /* zero the head pointers */
    outl(ioaddr + DnListPtr, 0);

  /* allocate memory for tx (download) and rx (upload) descriptors */
    if( !(pRingBase = palloc(sizeof(struct up_desc)*RX_RING_SIZE + sizeof(struct down_desc)*TX_RING_SIZE)) )
    {
        //#ERROR
        kprintf("NET: TX and RX RING MEMORY ALLOCATION FAILED! :-(\n");
        return 0;
    }
  /* allocate packet structures to point to buffers */
    if( !(pPacketMemBase = kalloc(sizeof(struct sPacket)*RX_RING_SIZE) ))
    {
        //#ERROR
        kprintf("NET: PACKET MEMORY ALLOCATION FAILED! :-(\n");
        return 0;
    }
  /* set cards rx ring head, and tx ring head (in the allocated block of memory) */
    pcard->rx_ring = (struct up_desc *)   pRingBase;
    pcard->tx_ring = (struct down_desc *)(pRingBase + (RX_RING_SIZE * sizeof(struct up_desc))); /*to get to the end of rx */


  /* initialize the members of RX descriptors */
    for (i = 0; i < RX_RING_SIZE; i++)
    {
        pcard->rx_ring[i].start_header = impliedBufferEnable;
        pcard->rx_ring[i].next = virt_to_phys(&pcard->rx_ring[i+1]);

        if (pcard->rx_ring[i].next & 7) /* not 8 byte aligned ! */
        {
            kprintf("NET: rx_ring[%d].next not 8 byte aligned (needs %d bytes)\n", i, 8 - (pcard->rx_ring[i].next % 8));
        }
    }
    pcard->rx_ring[RX_RING_SIZE-1].next = virt_to_phys(&pcard->rx_ring[0]); /* end to start */

  /* initialize the members of TX descriptors */
    for (i = 0; i < TX_RING_SIZE; i++)
    {
        pcard->tx_ring[i].len = 0;
        pcard->tx_ring[i].start_header = 0;
        pcard->tx_ring[i].next = 0;
        pcard->tx_ring[i].addr = 0;
    }

  /* set the current rx index and current_tx_index (both at zero) */
    pcard->rx_oldest = 0;
    pcard->rx_newest = 0;
    pcard->current_tx_index = 0;
    pcard->sent_tx_index  = 0;

  /* zero the TX packet pointers */
    for (i=0; i < TX_RING_SIZE; i++)
        pcard->tx_packets[i] = 0;

  /* link the packet structs buf pointers with UPDs */
    for(i=0;i < RX_RING_SIZE; i++)
    {
        pcard->rx_packets[i] = &((struct sPacket *)pPacketMemBase)[i];
        pcard->rx_packets[i]->iFlags = 0;
        pcard->rx_packets[i]->iLen   = 0;
        pcard->rx_packets[i]->pBuf   = &pcard->rx_ring[i].buf[0];
    }

    /* FIX LATENCY */
    cpciLatency = pci_config_read_byte(pcard->pci->dev, PCI_LATENCY_TIMER);
    cNewLatency = (PCI_IOTYPE & 1) ? 248 : 32;

    if (cpciLatency < cNewLatency)
    {
        //kprintf("NET(%d): Latency too low, changing from %d to %d\n",
            //pcard->pci->dev, cpciLatency, cNewLatency);
            pci_config_write_byte(pcard->pci->dev, PCI_LATENCY_TIMER, cNewLatency);
    }

    alphaQueryCard(pcard);
    alphaSelectXmit(pcard);

    //kprintf("Setting handler and enabling IRQ %d...\n",ppci->irq);
    //kprintf("NET:add irq handler(%d, 0x%x, 0x%x)\n",pcard->pci->irq, (FuncIRQHandler)&alphaInterrupt,(void *)pcard);
    //add_irq_handler(pcard->pci->irq,(FuncIRQHandler)&alphaInterrupt,(void *)pcard);
    // enable irq(ppci->irq); /* pointless (done by adding a handler) */
    add_irq_handler(pcard->pci->irq,(FuncIRQHandler)&parPortISR,(void *)0);
    gcard = pcard;
    gface = interface;

    return interface;
}

void alphaSelectXmit(struct sAlphaCard * pcard)
{
    int offset;
    long i_cfg;
    long phy;
    long phy_idx;
    long mii_status;
    long phyx;
    unsigned int i;
    int reset_opts;
    unsigned long ioaddr = pcard->pci->begin[0];

    //kprintf("Activating tranceiver...");

    outw(ioaddr + CommandReg, SelectWindow + 2);

    reset_opts = inw(ioaddr + 12); /* Wn2_ResetOptions = 12 */

    if (PCI_IOTYPE & 0x400) /* 0x400 = INVERT_LED_POWER */
        reset_opts |= 0x0010;

    if (PCI_IOTYPE & 0x4000) /* MII_XCVR_PWR=0x4000 */
        reset_opts |= 0x4000;

    outw(ioaddr + 12, reset_opts); /* Wn2_ResetOptions = 12 */

    if (PCI_IOTYPE & 0x1000) { /* WN0 XCVR PWR=0x1000 */
        outw(ioaddr + CommandReg, SelectWindow + 0);
        outw(ioaddr, 0x0900);
    }

    //kprintf("Activated.\n");

    /* configuration options */
    outw(ioaddr + CommandReg, SelectWindow + 3);

    pcard->available_media = inw(ioaddr + 8/* Wn3_Options = 8 */);

    i_cfg = inl(ioaddr + 0 /*Wn3_Config=0*/); /* Internal Configuration */

    pcard->default_media = (i_cfg >> 20) & 15;

    //kprintf("NET: Internal config register is %x\n", i_cfg);

    pcard->autoselect = i_cfg & 0x01000000 ? 1 : 0;

    /* media type */
    if (pcard->media_override != 7)
        pcard->if_port = pcard->media_override;
    else
        pcard->if_port = pcard->default_media;

    /* tranceiver setup */
    outw(ioaddr + CommandReg, SelectWindow + 4);
    phy_idx = 0;
    mii_preamble_required++;
    mdio_sync(ioaddr, 32);
    mdio_read(ioaddr, 24, 1);

    for (phy=1; phy <= 32 && phy_idx < 2; phy++)
    {
        phyx = phy & 0x1f;
        mii_status = mdio_read(ioaddr, phyx, 1);
        if ((mii_status & 0xf800)  &&  mii_status != 0xffff) {
            pcard->phys[phy_idx++] = phyx;

            //kprintf("MII transceiver found at address %d status %x.\n", phyx, mii status);
            if ((mii_status & 0x0040) == 0)
                mii_preamble_required++;
        }
    }

    mii_preamble_required--;

    if (phy_idx == 0) {
        //kprintf("***WARNING*** No MII transceivers found!\n");
        pcard->phys[0] = 24;
    } else {
        if (mii_preamble_required == 0  &&
            mdio_read(ioaddr, pcard->phys[0], 1) == 0) {
            kprintf("NET:  MII transceiver has preamble bug.\n");
            mii_preamble_required = 1;
        }

        pcard->advertising = mdio_read(ioaddr, pcard->phys[0], 4);

        if (pcard->full_duplex) {
            /* Only advertise the FD media types. */
            pcard->advertising &= ~0x02A0;
            mdio_write(ioaddr, pcard->phys[0], 4, pcard->advertising);
        }
    }

    if (pcard->capabilities & 0x20) {
        pcard->full_bus_master_tx = 1;
        kprintf("NET:  Using bus-master transmits and %s receives.\n",
               ((pcard->info2 & 1) ? "early" : "whole-frame") );
        pcard->full_bus_master_rx = (pcard->info2 & 1) ? 1 : 2;
    }

    /* startup */

    /* Before initializing select the active media port. */
    if (pcard->media_override != 7) {
        //kprintf("NET: MediaOverride (%d)\n",pcard->media_override);*/

        pcard->if_port = pcard->media_override;
    } else if (pcard->autoselect) {
            pcard->if_port = 8; /* NWAY */
    } else
        pcard->if_port = pcard->default_media; /* let's be poor */

    if (! pcard->medialock)
        pcard->full_duplex = 0;

    //kprintf("NET: Initial media type %d %s-duplex.\n",
             //pcard->if_port, pcard->full_duplex ? "full":"half");

    set_media_type(pcard);
    //start_operation(pcard);

    outw(ioaddr + CommandReg, SelectWindow + 4);

    /* Switch to the stats window, and clear all stats by reading. */
    outw(ioaddr + CommandReg, 22<<11);
    outw(ioaddr + CommandReg, (1<<11) + 6);
    for (i=0; i<10; i++)
        inb(ioaddr + i);

    inw(ioaddr + 10);
    inw(ioaddr + 12);
    /* New: On the Vortex we must also clear the BadSSD counter. */
    inb(ioaddr + 12);
    /* ..and on the Boomerang we enable the extra statistics bits. */
    outw(ioaddr + 6, 0x0040);

    /* Switch to window 7 for normal use. */
    outw(ioaddr + CommandReg, SelectWindow + 7);

}


int alphaQueryCard(struct sAlphaCard * pcard)
{

    short ee_read_cmd;
    short cmd_and_addr;
    int offset;
    long i_cfg;
    long phy;
    long phy_idx;
    long mii_status;
    long phyx;
    unsigned int i;
    unsigned short eeprom[0x40];
    unsigned short checksum;
    unsigned long ioaddr = pcard->pci->begin[0];


  /* EEPROM Query */

    outw(ioaddr + CommandReg, SelectWindow + 0);

    /* Locate the opcode bits, 0xC0 or 0x300. */
    outw(ioaddr + 12, 0x5555);

    ee_read_cmd = (( do_eeprom_op(ioaddr, 0x80) == 0x5555)  ?  0x200 : 0x80);

    if (do_eeprom_op(ioaddr, ee_read_cmd + 0x37) == 0x6d50)
        ee_read_cmd += 0x30;


    for (i = 0; i < 0x40; i++) {
        cmd_and_addr = ee_read_cmd + i;
        if (ee_read_cmd == 0xb0) {      /* Correct for discontinuity. */
            offset = 0x30 + i;
            cmd_and_addr = 0x80 + (offset & 0x3f) + ((offset<<2) & 0x0f00);
        }
        eeprom[i] = do_eeprom_op(ioaddr, cmd_and_addr);
    }


    /* EEPROM checksum */
    checksum = 0;
    for (i = 0; i < 0x18; i++)
        checksum ^= eeprom[i];

    checksum = (checksum ^ (checksum >> 8)) & 0xff;

    if (checksum != 0x00) {
        while (i < 0x21)
            checksum ^= eeprom[i++];

        checksum = (checksum ^ (checksum >> 8)) & 0xff;
    }

    if (checksum != 0x00)
        kprintf("*ALERT*: BAD EEPROM CHECKSUM: 0x%4x!\n", checksum);


    /* get MacAddress */
    for (i=0; i < 3; i++)
    {
        pcard->MacAddress[i*2] = ((eeprom[i + 10] >> 8) & 0x00ff);
        pcard->MacAddress[(i*2)+1] = ((eeprom[i + 10]) & 0x00ff);
        pcard->MacMask[i*2] = 0xff;
        pcard->MacMask[i*2+1] = 0xff;
    }


    return 0;

}

/* enabling:
ENABLE FULL
ENABLE RECEIVE
ENABLE TRANSMIT
ENABLE STATISTICS
ENABLE_INTERRUPTS


*/
#define NET_DISABLE 0
#define NET_ENABLE  1
#define NET_RESET   2

int alphaSetting(struct iface * interface, int iSetting, void * param)
{
    struct sAlphaCard * pcard = ((struct sAlphaCard *)(interface->driver_struct));
    unsigned long ioaddr =  pcard->pci->begin[0];

    int i;

    switch(iSetting)
    {
        case NET_ENABLE: {/*Enable the network card (everything needed after full reset) */

          /* set interrupt masks */
            /* Ack all pending interrupts, and set active indicator mask. */
            outw(ioaddr + CommandReg,   AckIntr | interruptLatchAck | rxEarlyAck |
                                        intRequestedAck | dnCompleteAck | upCompleteAck);
            outw(ioaddr + CommandReg, SetStatusEnb | interruptLatch | hostError | dnComplete | upComplete | updateStats | linkEvent | intRequested | txComplete);
            outw(ioaddr + CommandReg, SetIntrEnb   | interruptLatch | hostError | dnComplete | upComplete | updateStats | linkEvent | intRequested | txComplete);

          /* write StationAddress */
            outw(ioaddr + CommandReg, SelectWindow + 2);
            for (i=0; i<6; i++)
            {
                outb(ioaddr + i, pcard->MacAddress[i]); /* write StationAddress */
                //outb(ioaddr +6 + i, pcard->MacMask[i]);   /* and mask */
            }

          /* receiver setup */
            outw(ioaddr + CommandReg, RxDisable); /* disable receiver first */
            for (i = 200000; i; i--) /* wait to finish */
                if (!(inw(ioaddr + CommandReg) & cmdInProgress))
                    break;

            if(inl(ioaddr + UpListPtr) == 0) /* no starting for rx list? */
            {
                outw(ioaddr + CommandReg, UpStall); /* Stall so we don't break things */
                for (i = 200000; i; i--) /* wait to finish */
                    if (!(inw(ioaddr + CommandReg) & cmdInProgress))
                        break;

                pcard->rx_oldest = 0; /* reset counters */
                pcard->rx_newest = 0;

                for (i = 0; i < RX_RING_SIZE; i++)
                    pcard->rx_ring[i].start_header = impliedBufferEnable; /* reset status */

                outl(ioaddr + UpListPtr, (unsigned long)virt_to_phys(&pcard->rx_ring[0])); /* set main dpd */
                outw(ioaddr + CommandReg, UpUnstall);
            }
            outw(ioaddr + CommandReg, SelectWindow + 3);
            outw(ioaddr + 4, 1536); /* max sized packet */

            outw(ioaddr + CommandReg, SetRxFilter + 0x08); /* prom. operation */
            outb(ioaddr + UpPoll, 2); /* set polling rate */

          /* transmitter setup */
            outw(ioaddr + CommandReg, TxDisable); /* disable transmitter first */
            for (i = 200000; i; i--) /* wait to finish */
                if (!(inw(ioaddr + CommandReg) & cmdInProgress))
                    break;

            if(inl(ioaddr + DnListPtr) == 0) /* no starting for tx list? */
            {
                outw(ioaddr + CommandReg, DnStall); /* Stall so we don't break things */
                for (i = 200000; i; i--) /* wait to finish */
                    if (!(inw(ioaddr + CommandReg) & cmdInProgress))
                        break;

                pcard->current_tx_index = 0; /* reset tx counters */
                pcard->sent_tx_index  = 0;

                pcard->tx_ring[0].start_header = dpdEmpty | sh_dnComplete; /* just an empty download descriptor to start */
                outl(ioaddr + DnListPtr, (unsigned long)virt_to_phys(&pcard->tx_ring[0])); /* set main dpd */

                outw(ioaddr + CommandReg, DnUnstall); /* unstall */
            }

            outb(ioaddr + DnPoll, 20); /* set polling rate */

            outw(ioaddr + CommandReg, RxEnable); /* enable receiving */
            outw(ioaddr + CommandReg, TxEnable); /* enable transmiting */

            //kprintf("NET: ENABLED!\n");
            pcard->enabled = true;
            break;
        }
        case NET_DISABLE: /*Disable the network card*/
        {
            pcard->enabled = false;

            outw(ioaddr + CommandReg, TxDisable);
            outw(ioaddr + CommandReg, RxDisable);

            //kprintf("NET: DISABLED!\n");
            break;
        }
        case NET_RESET: /* fully reset card (not enable)*/
        {
            pcard->enabled = false;

            outw(ioaddr + CommandReg, GlobalReset); /* Fully reset network card */
            for (i = 200000; i; i--) /* wait to finish */
                if (!(inw(ioaddr + CommandReg) & cmdInProgress))
                    break;
            //kprintf("NET: RESET!\n");
            break;
        }
    }
    return 0;
}

int alphaGetPackets(struct iface * interface, struct sPacket ** packets, unsigned int * count)
{
    struct sAlphaCard * pcard = ((struct sAlphaCard *)(interface->driver_struct));
    unsigned long ioaddr =  pcard->pci->begin[0];
    unsigned int cur_packet;
    unsigned int our_count = 0;

    outw(ioaddr + CommandReg, SetStatusEnb | interruptLatch | hostError | dnComplete  /* | upComplete */ | updateStats | linkEvent | intRequested /*| txComplete*/);

    cur_packet = pcard->rx_oldest; /* start at oldest on queue */
    for(;;) /* broken by hitting rx_newest or count */
    {

        if(   (pcard->rx_ring[cur_packet].start_header & sh_upComplete)    /* packet here */
           && (pcard->rx_packets[cur_packet]->iFlags & PACKET_SYNCHED  )       /* handled by interrupt */
           && (!(pcard->rx_ring[cur_packet].start_header & upError))    /* & no errors */
           && (!(pcard->rx_packets[cur_packet]->iFlags & PACKET_ACTIVE)) /* & no handled*/
           && (!(pcard->rx_packets[cur_packet]->iFlags & PACKET_LOST)))   /* & not lost */
        {
            /* add to return list */
            packets[our_count++] = (struct sPacket *)pcard->rx_packets[cur_packet];
        }

        if((cur_packet == pcard->rx_newest) || our_count >= *count) /* that was last */
            break;

        cur_packet = (cur_packet + 1) % RX_RING_SIZE; /* next in ring */
    }
    outw(ioaddr + CommandReg, SetStatusEnb | interruptLatch | hostError | dnComplete | upComplete | updateStats | linkEvent | intRequested | txComplete);

    *count = our_count; /* return actual count */

    return 0;

}

int alphaSendPackets(struct iface * interface, struct sPacket ** packets, unsigned int * count)
{
    struct sAlphaCard * pcard = ((struct sAlphaCard *)(interface->driver_struct));
    unsigned long ioaddr =  pcard->pci->begin[0];
    unsigned int cur_packet;
    struct down_desc * pDesc, * pPrevDesc;

    outw(ioaddr + CommandReg, SetStatusEnb | interruptLatch | hostError /*| dnComplete */ | upComplete  | updateStats | linkEvent | intRequested /*| txComplete */);

    for(cur_packet=0; cur_packet < *count ;cur_packet++) /* proccess each packet in array */
    {
      /* first see if we can fit */
        if (((TX_RING_SIZE + pcard->current_tx_index - pcard->sent_tx_index) % TX_RING_SIZE)
             >= TX_MAXI_SIZE   ||  /* the maximum number of packets are on the ring */
            !packets[cur_packet]                     || /* Null pointer*/
            packets[cur_packet]->iLen > 1536         || /* too big */
            !packets[cur_packet]->pBuf                  /* no buffer */)
        {   /* the ring is full or other error, abort here
             cur packet is now count because cur packet starts from 0,
             but was incremented to a failed packet (no net offset) */
            kprintf("FULL!!!");
            goto out;
        }

      /* pPrevDesc is current */
        pPrevDesc = &pcard->tx_ring[pcard->current_tx_index];
      /* increment the current tx index (and wrap around at end) */
        pcard->current_tx_index =  (pcard->current_tx_index + 1) % TX_RING_SIZE;
      /* set pointer */
        pDesc     = &pcard->tx_ring[pcard->current_tx_index];

      /* fill out download descriptor */
        /*                round up to word        down int (only at given freq.)        packet it (index)   */
        pDesc->start_header     = 0x02 | (cur_packet % TX_INT_FREQ ? 0 : dnIndicate) | (pcard->current_tx_index<<2)                |
        /* eth sum spec? */     ((packets[cur_packet]->iFlags & ETH_SUM_CHECKED) ? crcAppendDisable : 0) |
        /*  ip sum spec? */     ((packets[cur_packet]->iFlags & IP_SUM_CHECKED)  ? 0 : addIpChecksum)      |
        /* tcp sum spec? */     ((packets[cur_packet]->iFlags & TRANS_SUM_CHECKED) ? 0 : addTcpChecksum)   |
        /* udp sum spec? */     ((packets[cur_packet]->iFlags & TRANS_SUM_CHECKED) ? 0 : addUdpChecksum);

        pDesc->next = 0; /* this is at the end of the list */
        pDesc->addr = virt_to_phys(packets[cur_packet]->pBuf);
        pDesc->len = packets[cur_packet]->iLen | (1<<31); /* (1<<31 indicates last (one) fragment) */

        /* record in own list */
        pcard->tx_packets[pcard->current_tx_index] = packets[cur_packet];

        /* link to the end of the download descriptor list */
        pPrevDesc->next = virt_to_phys(pDesc);

    }
    out:
    outw(ioaddr + CommandReg, SetStatusEnb | interruptLatch | hostError | dnComplete | upComplete | updateStats | linkEvent | intRequested | txComplete);

    *count = cur_packet;

    return 0; /* 0 == succesfull */
}


/* MII transceiver control section.
   Read and write the MII registers using software-generated serial
   MDIO protocol.  See the MII specifications or DP83840A data sheet
   for details. */

/* The maximum data clock rate is 2.5 Mhz.  The minimum timing is usually
   met by back-to-back pci I/O cycles, but we insert a delay to avoid
   "overclocking" issues. */


#define MDIO_SHIFT_CLK  0x01
#define MDIO_DIR_WRITE  0x04
#define MDIO_DATA_WRITE0 (0x00 | 0x04)
#define MDIO_DATA_WRITE1  (0x02 | 0x04)
#define MDIO_DATA_READ  0x02
#define MDIO_ENB_IN     0x00

/* Generate the preamble required for initial synchronization and
   a few older transceivers. */
void mdio_sync(long ioaddr, int bits)
{
    long mdio_addr;

    mdio_addr = ioaddr + 8 /*Wn4_PhysicalMgmt*/;

    /* Establish sync by sending at least 32 logic ones. */
    while (--bits >= 0)
    {
        outw(mdio_addr, MDIO_DATA_WRITE1);
        inl(mdio_addr); /* delay */
        outw(mdio_addr, MDIO_DATA_WRITE1 | MDIO_SHIFT_CLK);
        inl(mdio_addr); /* delay */
    }
}


long mdio_read(long ioaddr, int phy_id, int location)
{
    int i;
    long read_cmd;
    long dataval;
    unsigned long retval;
    long mdio_addr;
    long lRead;

    retval = 0;
    mdio_addr = ioaddr + 8;
    read_cmd =  (0xf6 << 10) | (phy_id << 5) | location;

    if (mii_preamble_required)
        mdio_sync(ioaddr, 32);

    /* Shift the read command bits out. */
    for (i=14; i >= 0; i--) {
        dataval = (read_cmd&(1<<i)) ? MDIO_DATA_WRITE1 : MDIO_DATA_WRITE0;
        outw(mdio_addr, dataval);
        inl(mdio_addr); /* delay */
        outw(mdio_addr, dataval | MDIO_SHIFT_CLK);
        inl(mdio_addr); /* delay */
    }
    /* Read the two transition and 16 data bits. */
    for (i=18; i > 0; i--) {
        outw(mdio_addr, MDIO_ENB_IN);
        inl(mdio_addr); /* delay */
        lRead = inw(mdio_addr);
        retval = (retval << 1) | ((lRead & MDIO_DATA_READ) ? 1 : 0);
        outw(mdio_addr, MDIO_ENB_IN | MDIO_SHIFT_CLK);
        inl(mdio_addr); /* delay */
    }

    return (retval & 0x10000 ? 0xffff : retval & 0xffff);
}

void mdio_write(long ioaddr, int phy_id, int location, int value)
{
    long write_cmd;
    long mdio_addr;
    int i;
    long dataval;

    mdio_addr = ioaddr + 8;
    write_cmd= 0x50020000 | (phy_id << 23) | (location << 18) | value;

    if (mii_preamble_required)
        mdio_sync(ioaddr, 32);

    /* Shift the command bits out. */
    for (i = 31; i >= 0; i--)
    {
        dataval = (write_cmd&(1<<i)) ? MDIO_DATA_WRITE1 : MDIO_DATA_WRITE0;
        outw(mdio_addr, dataval);
        inl(mdio_addr); /* delay */
        outw(mdio_addr, dataval | MDIO_SHIFT_CLK);
        inl(mdio_addr); /* delay */
    }
    /* Leave the interface idle. */
    mdio_sync(ioaddr, 32);

    return;
}

void set_media_type(struct sAlphaCard * pcard)
{

    long ioaddr = pcard->pci->begin[0];
    long i_cfg;
    int mii_reg1, mii_reg5;

    outw(ioaddr + CommandReg, SelectWindow + 3);
    i_cfg = inl(ioaddr + 0);
    i_cfg &= ~0x00f00000;

    outl(ioaddr + 0, i_cfg | 0x00800000);

    outw(ioaddr + CommandReg, SelectWindow + 4);

    /* Read BMSR (reg1) only to clear old status. */
    mii_reg1 = mdio_read(ioaddr, pcard->phys[0], 1);
    mii_reg5 = mdio_read(ioaddr, pcard->phys[0], 5);

    if (mii_reg5 == 0xffff  ||  mii_reg5 == 0x0000)
        ;                   /* No MII device or no link partner report */
    else if ((mii_reg5 & 0x0100) != 0   /* 100baseTx-FD */
              || (mii_reg5 & 0x00C0) == 0x0040) /* 10T-FD, but not 100-HD */
        pcard->full_duplex = 1;

    kprintf("%s\n",
            pcard->phys[0],mii_reg1, mii_reg5, pcard->full_duplex ? "100Mb/s" : "10Mb/s");


    /* Do we require link beat to transmit? */
    if (pcard->info1 & 0x4000)
        outw(inw(ioaddr + 10) & ~0x0080, ioaddr + 10);

    /* Set the full-duplex and oversized frame bits. */
    outw(ioaddr + CommandReg, SelectWindow + 3);

    pcard->wn3_mac_ctrl = pcard->full_duplex ? 0x0120 : 0;

    outb(ioaddr + 6, pcard->wn3_mac_ctrl);

}

void fake_inter() {
    u_long ioaddr = gcard->pci->begin[0];
    outw(ioaddr + CommandReg, FakeIntr);
}
