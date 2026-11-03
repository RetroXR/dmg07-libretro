/* The DMG-07 4 Player Adapter's protocol, with no wire and no clock.
 *
 * Written from Pan Docs' "4-Player Adapter" chapter (gbdev/pandocs,
 * src/Four_Player_Adapter.md), which is shonumi's research. The adapter drives
 * the serial clock for all four Game Boys, which run in external clock mode, and
 * every byte slot is one simultaneous exchange on each port: the adapter shifts a
 * byte out to each Game Boy while each shifts its own byte back.
 *
 * Two phases:
 *
 *   PING      Every packet is 4 bytes to each port: $FE, then three STAT bytes,
 *             (connected players << 4) | that port's player id (1-4). A Game Boy
 *             answers $88 $88 RATE SIZE, one byte behind (it loads each reply
 *             after the byte it answers). Three or more $AA in a row from a Game
 *             Boy at a packet's end switch to transmission: the next packet is
 *             four $CC, then data packets.
 *
 *   TRANSMIT  Packets of SIZE*4 bytes, the same to every port: what each player
 *             sent in its first SIZE slots of the PREVIOUS packet, player 1's
 *             bytes first, zeros for a player with no Game Boy. Three or more
 *             $FF in a row from a Game Boy at the start of a packet switch back:
 *             once the packet ends the adapter sends a packet of $FF, then pings.
 *
 * What is not known and is chosen here (dmg07.c says where): which reply a
 * Game Boy's "connected" bit is taken from, which player's RATE and SIZE count,
 * the buffer's contents on the first data packet, and the 0.36-2.15 ms Pan Docs
 * could not explain in a transmission packet's length. */
#ifndef DMG07_H
#define DMG07_H

#include <stdint.h>

#define DMG07_PORTS 4
#define DMG07_MAX_PACKET 16

/* Link ticks, 8388608 a second (twice the Game Boy's clock), as gb-sio counts. */
#define DMG07_TICKS_PER_MS 8388.608

enum
{
   DMG07_PING = 0,
   DMG07_TO_TRANSMIT,   /* the four $CC */
   DMG07_TRANSMIT,
   DMG07_TO_PING        /* the packet of $FF */
};

/* What one port's reply stream has said lately (the ping handshake). */
typedef struct dmg07_port
{
   uint8_t hand;        /* 0 wait $88, 1 one $88 seen, 2 next is RATE, 3 next is SIZE */
   uint8_t acked;       /* a full $88 $88 seen in the current or the last packet */
   uint8_t heard;       /* any $88 in the current packet */
   uint8_t aa_run;      /* consecutive $AA */
   uint8_t ff_run;      /* consecutive $FF from the start of this packet */
   uint8_t rate;        /* the last RATE and SIZE this Game Boy sent */
   uint8_t size;
   uint8_t pad;
} dmg07_port;

typedef struct dmg07
{
   uint8_t phase;
   uint8_t index;              /* the byte slot in the current packet */
   uint8_t connected;          /* bit k: player k+1 answered the ping */
   uint8_t rate;               /* the settings in force */
   uint8_t size;
   uint8_t restart;            /* an $FF run was heard: ping once this packet ends */
   uint8_t pad[2];
   uint8_t buffer[DMG07_MAX_PACKET];   /* broadcast this packet */
   uint8_t next[DMG07_MAX_PACKET];     /* collected for the next */
   dmg07_port port[DMG07_PORTS];
   uint32_t packets;           /* packets sent, for log lines */
   uint8_t timing;             /* DMG07_TIMING_PANDOCS or DMG07_TIMING_GBE */
   uint8_t pad2[3];
} dmg07;

/* Pan Docs' measured packet timing, or GBE+'s: every byte 8 x (6 x RATE + 512)
 * Game Boy cycles after the last, packets back to back. */
enum { DMG07_TIMING_PANDOCS = 0, DMG07_TIMING_GBE = 1, DMG07_TIMING_BYTES = 2 };

void dmg07_reset(dmg07 *a);

/* Bytes in the current packet. */
unsigned dmg07_packet_len(const dmg07 *a);

/* The byte port `p` receives in the current slot. */
uint8_t dmg07_out(const dmg07 *a, unsigned p);

/* The slot's replies: `in[p]` from each port, `answered` bit p set where a Game
 * Boy took part in the exchange (cabled and armed). Moves to the next slot, and
 * at a packet's end decides the next packet. Returns 1 when a packet ended. */
int dmg07_in(dmg07 *a, const uint8_t in[DMG07_PORTS], unsigned answered);

/* Ticks from the start of this slot to the start of the next one in the same
 * packet, and from the start of a packet to the start of the next. */
uint64_t dmg07_byte_ticks(const dmg07 *a);
uint64_t dmg07_packet_ticks(const dmg07 *a);

const char *dmg07_phase_name(unsigned phase);

#endif
