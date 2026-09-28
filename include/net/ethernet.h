#ifndef _NET_ETHERNET_H
#define _NET_ETHERNET_H
/*
    NETWORK.H - The network header file
    Written by Martin McCormick (surfos@wazer.net) (c) 2004
    for the Surf Operating System
*/

/* because of loopback references, need structure prototypes (weird huh?) */
struct iface;
struct sPacketBuf;


struct sPacket
{
    unsigned int iLen;
    unsigned char * pBuf;

    unsigned int iFlags; /* 32 bit */
    /*
     -Bits 0 through 7 - CHECKSUM FLAGS
      (1<<0) = ETHERNET checksum checked
      (1<<1) = ENCAPSULATION checksum checked
      (1<<2) = IP checksum checked
      (1<<3) = TCP/UDP checksum checked

      (1<<4) = ETHERNET checksum correct
      (1<<5) = ENCAPSULATION checksum correct
      (1<<6) = IP checksum correct
      (1<<7) = TCP/UDP checksum correct

     -Bits 8 through 15 - USER LEVEL FLAGS

     -Bits 16 through 23 - HANDLER FLAGS (if any set, driver will not return packet as 'on buffer')
      (1<<16) = Packet is activated for proccessing.
      (1<<17) = Packet is on TX queue.
      (1<<18) = Packet has been sent and freed.

     -Bits 24 through 32 - DRIVER FLAGS
      (1<<24) = Packet synched.
    */

};

enum PacketChecksumFlags {
    CHECKSUM_FLAGS = (0xff<<0),
    ETH_SUM_CHECKED = (1<<0), ENC_SUM_CHECKED = (1<<1), IP_SUM_CHECKED = (1<<2),
    TRANS_SUM_CHECKED = (1<<3), ETH_SUM_CORRECT = (1<<4), ENC_SUM_CORRECT = (1<<5),

    IP_SUM_CORRECT = (1<<6), TRANS_SUM_CORRECT = (1<<7)
};

enum PacketHandlerFlags {
    HANDLER_FLAGS = (0xff<<16),
    PACKET_ACTIVE = (1<<16), PACKET_ON_TX = (1<<17), PACKET_SENT = (1<<18)
};

enum PacketDriverFlags {
    DRIVER_FLAGS = (0xff<<24),
    PACKET_SYNCHED = (1<<24), PACKET_LOST = (1<<25)
};

/* SnagPackets(struct iface * interface, struct sPacketBuf ** packets, unsigned int * count);
    SnagPackets receives a given number of packets (count)
    iface - the interface to receive on (should be owner of function pointer)
    packets - a pointer to the head of an array of packet pointers
    count - takes in max number to receive, output is actual count
*/
typedef int(* FuncSnag)
    ( /* parameters */
      struct iface * interface,
      struct sPacketBuf ** packets,
      unsigned int * count
    );

/* SendPackets(struct iface * interface, struct sPacketBuf ** packets, unsigned int * count);
    SendPackets sends a given number of packets (count)
    iface - the interface to send upon (should be owner of function pointer)
    packets - a pointer to the head of an array of pointers to outgoing packets
    count - takes in number to send, output is actual sent
*/
typedef int(* FuncSend)
    ( /* parameters */
      struct iface * interface,
      struct sPacketBuf ** packets,
      unsigned int * count
    );

/*  Setting(struct iface * interface, int iSetting, void * param);
    Setting() sets (or receives) network card information.
    iface - interface to use
    iSetting - a constant reffering to the type of setting
    param - Used for I/O operations on settings
*/
typedef int(* FuncSetting)
    ( /* parameters */
      struct iface * interface,
      int iSetting,
      void * param
    );


/* iface - The main interface structure used for network interfaces */
struct iface
{
    unsigned int id;
    void * driver_struct; /* used internally by driver */

    /* standard io functions */
    FuncSetting Setting;
    FuncSnag SnagPackets;
    FuncSend SendPackets;

};

#endif
