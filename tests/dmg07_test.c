/* The adapter's protocol against four simulated Game Boys, each doing what Pan
 * Docs says a Game Boy does: load its reply to a byte after receiving it, so
 * the adapter hears it in the NEXT slot. No wire, no clock: dmg07.c alone.
 *
 *     cmake --build build && ctest --test-dir build -C Release
 */
#include <stdio.h>
#include <string.h>

#include "../src/dmg07.c"

static int failures;
static int checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL  " __VA_ARGS__); printf("\n"); } } while (0)

typedef struct gb
{
   int present;
   uint8_t sb;           /* what it shifts out at the next slot */
   uint8_t got[64];      /* the last bytes received */
   unsigned n;
   int mode;             /* 0 ping replies, 1 send AA x4, 2 data, 3 send FF x4 */
   unsigned left;
   uint8_t data;
   unsigned pkt_index;   /* where it believes it is in a transmission packet */
} gb;

static uint8_t rate = 0x10, size = 1;

static void gb_receive(gb *g, uint8_t b, unsigned slot_index, unsigned packet_len)
{
   g->got[g->n++ % 64] = b;
   switch (g->mode)
   {
   case 0:
      if (b == 0xFE) g->sb = 0x88;              /* ACK1 */
      else if (slot_index == 1) g->sb = 0x88;   /* ACK2 after STAT1 */
      else if (slot_index == 2) g->sb = rate;   /* RATE after STAT2 */
      else g->sb = size;                        /* SIZE after STAT3 */
      break;
   case 1:
      g->sb = 0xAA;
      if (g->left && --g->left == 0) g->mode = 0;
      break;
   case 3:
      g->sb = 0xFF;
      if (g->left && --g->left == 0) g->mode = 2;
      break;
   default:
      /* Its data byte is loaded once slot 0 is received, so it goes out at
       * slot 1. */
      g->sb = (slot_index == 0) ? g->data : 0x00;
      break;
   }
}

/* One slot: everyone exchanges. Returns 1 when a packet ended. */
static int step(dmg07 *a, gb *gbs, uint8_t out[DMG07_PORTS])
{
   uint8_t in[DMG07_PORTS];
   unsigned answered = 0, p, idx = a->index, len = dmg07_packet_len(a);
   for (p = 0; p < DMG07_PORTS; p++)
   {
      out[p] = dmg07_out(a, p);
      in[p] = 0xFF;
      if (gbs[p].present)
      {
         in[p] = gbs[p].sb;
         answered |= 1u << p;
      }
   }
   for (p = 0; p < DMG07_PORTS; p++)
      if (gbs[p].present)
         gb_receive(&gbs[p], out[p], idx, len);
   return dmg07_in(a, in, answered);
}

static void run_packets(dmg07 *a, gb *gbs, unsigned packets, uint8_t last[DMG07_PORTS][DMG07_MAX_PACKET])
{
   uint8_t out[DMG07_PORTS];
   unsigned n = 0;
   while (n < packets)
   {
      unsigned idx = a->index, p;
      int ended = step(a, gbs, out);
      for (p = 0; p < DMG07_PORTS; p++)
         last[p][idx] = out[p];
      if (ended)
         n++;
   }
}

int main(void)
{
   dmg07 a;
   gb gbs[DMG07_PORTS];
   uint8_t last[DMG07_PORTS][DMG07_MAX_PACKET];
   unsigned p;

   dmg07_reset(&a);
   memset(gbs, 0, sizeof(gbs));

   /* Nobody there: FE 01 01 01 to player 1. */
   CHECK(dmg07_out(&a, 0) == 0xFE, "ping/the header is $FE");
   run_packets(&a, gbs, 1, last);
   CHECK(last[0][1] == 0x01 && last[3][1] == 0x04, "ping/alone, STAT is the player id (%02X %02X)", last[0][1], last[3][1]);

   /* Players 1, 2 and 4 cabled and answering. */
   gbs[0].present = gbs[1].present = gbs[3].present = 1;
   run_packets(&a, gbs, 3, last);
   CHECK(a.connected == 0x0B, "ping/players 1, 2 and 4 answer (%02X)", a.connected);
   CHECK(last[0][1] == 0xB1 && last[1][1] == 0xB2 && last[3][3] == 0xB4,
      "ping/STAT is connected<<4 | id (%02X %02X %02X)", last[0][1], last[1][1], last[3][3]);
   CHECK(a.rate == 0x10 && a.size == 1, "ping/player 1's RATE and SIZE count (%02X %u)", a.rate, a.size);

   /* A player stops answering: its bit goes. */
   gbs[3].present = 0;
   run_packets(&a, gbs, 2, last);
   CHECK(a.connected == 0x03, "ping/a silent player is dropped (%02X)", a.connected);
   gbs[3].present = 1;
   run_packets(&a, gbs, 2, last);
   CHECK(a.connected == 0x0B, "ping/and comes back (%02X)", a.connected);

   /* SIZE 2 from player 1, then four $AA starting with its reply to the header. */
   size = 2;
   run_packets(&a, gbs, 2, last);
   CHECK(a.size == 2, "ping/SIZE 2 taken (%u)", a.size);
   while (a.index != 0) { uint8_t o[4]; step(&a, gbs, o); }
   /* The next reply goes out at slot 1 (after the header): load AA once the
    * header has been received. */
   {
      uint8_t o[4];
      step(&a, gbs, o);              /* slot 0: header received */
      gbs[0].mode = 1; gbs[0].left = 4; gbs[0].sb = 0xAA;
      while (a.index != 0) step(&a, gbs, o);
      CHECK(a.phase == DMG07_TO_TRANSMIT, "begin/three $AA from slot 1 switch (phase %s)", dmg07_phase_name(a.phase));
      CHECK(dmg07_out(&a, 2) == 0xCC, "begin/the next packet is $CC to everyone");
   }
   run_packets(&a, gbs, 1, last);
   CHECK(last[0][0] == 0xCC && last[3][3] == 0xCC, "begin/four $CC");
   CHECK(a.phase == DMG07_TRANSMIT && dmg07_packet_len(&a) == 8, "transmit/packets of SIZE x 4 (%u)", dmg07_packet_len(&a));

   /* Data: each player sends its own byte in its first slot. */
   for (p = 0; p < DMG07_PORTS; p++)
   {
      gbs[p].mode = 2;
      gbs[p].data = (uint8_t)(0x10 * (p + 1) + 1);
      gbs[p].sb = gbs[p].data;
   }
   run_packets(&a, gbs, 3, last);
   CHECK(last[0][0] == 0x11 && last[0][2] == 0x21 && last[0][4] == 0x00 && last[0][6] == 0x41,
      "transmit/one packet late: P1 P2 (no P3: zeros) P4 (%02X %02X %02X %02X)",
      last[0][0], last[0][2], last[0][4], last[0][6]);
   CHECK(memcmp(last[0], last[1], 8) == 0 && memcmp(last[0], last[3], 8) == 0,
      "transmit/every port receives the same packet");

   /* Restart: three or more $FF from the start of a packet. */
   while (a.index != 0) { uint8_t o[4]; step(&a, gbs, o); }
   gbs[1].mode = 3; gbs[1].left = 4; gbs[1].sb = 0xFF;
   run_packets(&a, gbs, 1, last);
   CHECK(a.phase == DMG07_TO_PING, "restart/$FF x3 from slot 0: an $FF packet next (%s)", dmg07_phase_name(a.phase));
   run_packets(&a, gbs, 1, last);
   CHECK(last[2][0] == 0xFF && last[2][7] == 0xFF, "restart/a packet of $FF");
   CHECK(a.phase == DMG07_PING && dmg07_out(&a, 0) == 0xFE, "restart/then pings");

   /* Timing: Pan Docs' figures. */
   dmg07_reset(&a);
   CHECK(dmg07_packet_ticks(&a) > 140000 && dmg07_packet_ticks(&a) < 143000,
      "timing/a power-up ping packet is ~17 ms (%llu ticks)", (unsigned long long)dmg07_packet_ticks(&a));
   CHECK(dmg07_byte_ticks(&a) > 12900 && dmg07_byte_ticks(&a) < 13100, "timing/1.548 ms a ping byte");

   /* The core's default: Pan Docs' ping, GBE+'s transmission (8 x (6 x RATE +
    * 512) cycles a byte, two ticks a cycle, packets back to back). */
   dmg07_reset(&a);
   a.timing = DMG07_TIMING_MIXED;
   CHECK(dmg07_byte_ticks(&a) > 12900 && dmg07_byte_ticks(&a) < 13100, "timing/mixed: a ping byte is Pan Docs'");
   CHECK(dmg07_packet_ticks(&a) > 140000 && dmg07_packet_ticks(&a) < 143000, "timing/mixed: and so is a ping packet");
   a.phase = DMG07_TRANSMIT;
   a.rate = 0xF3;
   a.size = 1;
   CHECK(dmg07_byte_ticks(&a) == 8u * (6u * 0xF3 + 512u) * 2u, "timing/mixed: a transmission byte is GBE+'s (%llu)",
      (unsigned long long)dmg07_byte_ticks(&a));
   CHECK(dmg07_packet_ticks(&a) == 4u * dmg07_byte_ticks(&a), "timing/mixed: packets back to back");

   printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "PASS", checks, failures);
   return failures ? 1 : 0;
}
