#include "dmg07.h"

#include <string.h>

#define PING_HEADER 0xFE
#define ACK 0x88
#define BEGIN 0xAA
#define BEGUN 0xCC
#define RESTART 0xFF

static uint64_t ms(double m)
{
   return (uint64_t)(m * DMG07_TICKS_PER_MS + 0.5);
}

void dmg07_reset(dmg07 *a)
{
   memset(a, 0, sizeof(*a));
   a->phase = DMG07_PING;
   /* Power-up timing is Pan Docs' 17 ms ping packet: RATE's low nibble 0. A
    * SIZE of 1 until a Game Boy says otherwise. */
   a->rate = 0;
   a->size = 1;
}

static unsigned transmit_len(const dmg07 *a)
{
   return (unsigned)a->size * DMG07_PORTS;
}

unsigned dmg07_packet_len(const dmg07 *a)
{
   switch (a->phase)
   {
   case DMG07_TRANSMIT:
   case DMG07_TO_PING:
      return transmit_len(a);
   default:
      return 4;
   }
}

uint8_t dmg07_out(const dmg07 *a, unsigned p)
{
   switch (a->phase)
   {
   case DMG07_PING:
      if (a->index == 0)
         return PING_HEADER;
      return (uint8_t)(((a->connected & 0x0F) << 4) | ((p + 1) & 0x07));
   case DMG07_TO_TRANSMIT:
      return BEGUN;
   case DMG07_TRANSMIT:
      return a->buffer[a->index];
   default:
      return RESTART;
   }
}

/* The Game Boy whose RATE and SIZE count. Pan Docs: player 1 "acts as master
 * and orchestrates the multiplayer session". Chosen here: if player 1 has not
 * answered, the lowest-numbered player that has. */
static int master(const dmg07 *a)
{
   unsigned p;
   for (p = 0; p < DMG07_PORTS; p++)
      if (a->port[p].acked)
         return (int)p;
   return -1;
}

/* One reply into a port's ping handshake.
 *
 * Pan Docs gives the replies as $88 $88 RATE SIZE, each loaded after the byte it
 * answers, and its two example charts disagree on which slot each lands in. So
 * the stream is read for the sequence, not the slot: two $88 in a row are a Game
 * Boy saying it is there, and the two bytes after them are its RATE and SIZE. */
static void hear_ping(dmg07 *a, unsigned p, uint8_t r)
{
   dmg07_port *s = &a->port[p];
   switch (s->hand)
   {
   case 0:
      if (r == ACK)
         s->hand = 1;
      break;
   case 1:
      if (r == ACK)
      {
         s->hand = 2;
         s->heard = 1;
         s->acked = 1;
         /* At once, not at the packet's end: the STAT bytes later in this same
          * packet already say so, which is the "status can change midway through
          * a ping" Pan Docs describes. */
         a->connected |= (uint8_t)(1u << p);
      }
      else
         s->hand = 0;
      break;
   case 2:
      s->rate = r;
      s->hand = 3;
      break;
   default:
      s->size = r;
      s->hand = 0;
      break;
   }
   if (r == BEGIN)
      s->aa_run++;
   else
      s->aa_run = 0;
}

static void end_ping_packet(dmg07 *a)
{
   unsigned p;
   int m;
   int begin = 0;

   for (p = 0; p < DMG07_PORTS; p++)
   {
      dmg07_port *s = &a->port[p];
      if (s->aa_run >= 3)
         begin = 1;
      /* A Game Boy that said nothing this packet is not connected any more:
       * "If for whatever reason, the ping responses are not sent, the status bits
       * are unset." One that is switching to transmission ($AA) keeps its bit. */
      if (!s->heard && s->aa_run == 0)
      {
         s->acked = 0;
         a->connected &= (uint8_t)~(1u << p);
      }
      s->heard = 0;
   }

   m = master(a);
   if (m >= 0)
   {
      /* $00 "does not change the speed"; SIZE works from 1 to 4. */
      if (a->port[m].rate != 0)
         a->rate = a->port[m].rate;
      if (a->port[m].size >= 1 && a->port[m].size <= 4)
         a->size = a->port[m].size;
   }

   if (begin)
   {
      a->phase = DMG07_TO_TRANSMIT;
      for (p = 0; p < DMG07_PORTS; p++)
         a->port[p].aa_run = 0;
   }
}

int dmg07_in(dmg07 *a, const uint8_t in[DMG07_PORTS], unsigned answered)
{
   unsigned p;
   unsigned len = dmg07_packet_len(a);

   switch (a->phase)
   {
   case DMG07_PING:
      for (p = 0; p < DMG07_PORTS; p++)
         if (answered & (1u << p))
            hear_ping(a, p, in[p]);
         else
            a->port[p].hand = 0;
      break;
   case DMG07_TRANSMIT:
      for (p = 0; p < DMG07_PORTS; p++)
      {
         dmg07_port *s = &a->port[p];
         if (a->index == 0)
            s->ff_run = 0;
         if (!(answered & (1u << p)))
            continue;
         /* Each player's data is what it sends in slots 1 to SIZE: a Game Boy
          * loads a byte after receiving the one before, so its first byte of a
          * packet goes out one slot in. Pan Docs' prose says "the first few
          * serial transfers"; GBE+ (shonumi's emulator, the source of that
          * research) takes slots 1..SIZE, and so does Wave Race. */
         if (a->index >= 1 && a->index <= a->size)
            a->next[p * a->size + (a->index - 1)] = in[p];
         if (in[p] == RESTART && s->ff_run == a->index)
         {
            s->ff_run++;
            if (s->ff_run >= 3)
               a->restart = 1;
         }
      }
      break;
   default:
      break;
   }

   a->index++;
   if (a->index < len)
      return 0;

   /* The packet is over: what comes next. */
   a->index = 0;
   a->packets++;
   switch (a->phase)
   {
   case DMG07_PING:
      end_ping_packet(a);
      break;
   case DMG07_TO_TRANSMIT:
      /* The buffer starts as "garbage based on output the master Game Boy had
       * sent during the ping phase"; chosen here: zeros. */
      a->phase = DMG07_TRANSMIT;
      memset(a->buffer, 0, sizeof(a->buffer));
      memset(a->next, 0, sizeof(a->next));
      break;
   case DMG07_TRANSMIT:
      memcpy(a->buffer, a->next, sizeof(a->buffer));
      memset(a->next, 0, sizeof(a->next));
      if (a->restart)
      {
         a->restart = 0;
         a->phase = DMG07_TO_PING;
      }
      break;
   default:
      a->phase = DMG07_PING;
      for (p = 0; p < DMG07_PORTS; p++)
      {
         a->port[p].hand = 0;
         a->port[p].aa_run = 0;
         a->port[p].heard = 0;
      }
      break;
   }
   return 1;
}

/* GBE+'s: 8 bits of (6 x RATE + 512) cycles, 2048 before a RATE is set; link
 * ticks are two a cycle. */
static uint64_t gbe_byte(const dmg07 *a)
{
   uint64_t per_bit = (a->phase == DMG07_PING && a->packets == 0) ? 2048u : 6u * a->rate + 512u;
   return per_bit * 8u * 2u;
}

/* DoubleCherryGB's transmission pacing (its emulator was tested against the
 * adapter's games): every byte 6144 x (m + 1) cycles after the last, where m is
 * RATE's top two bits (bit 7 its low bit, bit 6 its high), and no gap at a
 * packet's end. Ping is Pan Docs'. */
static uint64_t dcgb_byte(const dmg07 *a)
{
   unsigned m = ((a->rate & 0x80u) >> 7) | ((a->rate & 0x40u) >> 5);
   return (uint64_t)6144u * (m + 1u) * 2u;
}

/* Whether this packet is paced as GBE+ paces it: always under `gbe`, and the
 * transmission (not the ping) under `mixed`. */
static int gbe_paced(const dmg07 *a)
{
   if (a->timing == DMG07_TIMING_GBE)
      return 1;
   return a->timing == DMG07_TIMING_MIXED && (a->phase == DMG07_TRANSMIT || a->phase == DMG07_TO_PING);
}

uint64_t dmg07_byte_ticks(const dmg07 *a)
{
   if (gbe_paced(a))
      return gbe_byte(a);
   if (a->timing == DMG07_TIMING_DCGB && (a->phase == DMG07_TRANSMIT || a->phase == DMG07_TO_PING))
      return dcgb_byte(a);
   /* Ping (and the $CC packet, which ends ping timing): 128 us a byte and 1.42 ms
    * between bytes. Transmission: 0.887 ms + (RATE >> 4) x 0.106 ms between. */
   if (a->phase == DMG07_PING || a->phase == DMG07_TO_TRANSMIT)
      return ms(0.128 + 1.42);
   return ms(0.128 + 0.887 + (double)(a->rate >> 4) * 0.106);
}

uint64_t dmg07_packet_ticks(const dmg07 *a)
{
   uint64_t span;
   if (gbe_paced(a))
      return gbe_byte(a) * dmg07_packet_len(a);
   if (a->timing == DMG07_TIMING_DCGB && (a->phase == DMG07_TRANSMIT || a->phase == DMG07_TO_PING))
      return dcgb_byte(a) * dmg07_packet_len(a);
   if (a->phase == DMG07_PING || a->phase == DMG07_TO_TRANSMIT)
      /* 4.71 ms of bytes, then 12.2 ms + (RATE & 15) ms. */
      return ms(4.71 + 12.2 + (double)(a->rate & 0x0F));
   /* The larger of (RATE & 15) ms + 17 ms and the bytes plus an add-on Pan Docs
    * measured at 0.36-2.15 ms without a formula; 1 ms is chosen here. */
   span = (uint64_t)dmg07_packet_len(a) * dmg07_byte_ticks(a) + ms(1.0);
   /* A test: no 17 ms floor in transmission, the bytes alone. */
   if (a->timing == DMG07_TIMING_BYTES)
      return span;
   {
      uint64_t floor = ms(17.0 + (double)(a->rate & 0x0F));
      return span > floor ? span : floor;
   }
}

const char *dmg07_phase_name(unsigned phase)
{
   switch (phase)
   {
   case DMG07_PING: return "ping";
   case DMG07_TO_TRANSMIT: return "begin";
   case DMG07_TRANSMIT: return "transmission";
   default: return "restart";
   }
}
