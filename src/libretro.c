/* The DMG-07 4 Player Adapter as a libretro core.
 *
 * No CPU and no game: the adapter is the hub at the middle of up to four Game
 * Boy link cables, and the cables are the frontend's link bus
 * (RETRO_ENVIRONMENT_GET_LINK_INTERFACE). It attaches FOUR ports, one per
 * player, each on the "gb-sio" wire gambatte and mGBA already speak, so each
 * Game Boy is on a cable of two machines exactly as it would be cabled to another
 * Game Boy -- both drivers refuse a wire of more than two -- and neither Game Boy
 * core needed to change.
 *
 * The adapter is the end that drives the clock: every Game Boy runs in external
 * clock mode. At each byte slot it reads what each Game Boy has in its shift
 * register (the state the Game Boy published) and sends each a clock carrying the
 * byte that port receives, the way a Game Boy driving its own clock does
 * (gambatte's LinkSerial::send). The protocol is dmg07.c.
 *
 * Port k is player k+1. The adapter is powered by player 1's cable, as the real
 * one is: with nothing on port 0 it sends nothing at all.
 *
 * The video frame is 4 x 1 pixels, a lamp per player that a frontend may show or
 * ignore: dark with no Game Boy cabled, dim with one cabled, lit once it answers
 * the ping. There is no sound.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>

#include "link_interface.h"
#include "dmg07.h"

#define CORE_NAME    "DMG-07 4 Player Adapter"
#define CORE_VERSION "1.0.0"

#define LINK_PROTOCOL "gb-sio"
#define LINK_CLOCK_RATE ((uint64_t)8388608)

/* One Game Boy video frame in link ticks, so a frame here is a frame of the Game
 * Boys' (the printer's reasoning, gbprinter-libretro). */
#define FRAME_TICKS ((uint64_t)140448)

/* As the printer: rendezvous every 2048 ticks and promise 2048 ahead. A clock is
 * stamped a horizon out, as a Game Boy's own is, so a Game Boy that has run up to
 * the adapter's promise has not passed it. */
#define LINK_GRAIN ((uint64_t)2048)
#define LINK_HORIZON ((uint64_t)2048)
#define LINK_PENDING_MAX 16

enum { LS_STATE = 1, LS_CLOCK = 2 };
enum { LS_FLAG_ARMED = 1, LS_FLAG_FAST = 2 };
#define LS_MSG_SIZE 8

/* The adapter's clock is 62.66 kHz, a byte in 128 us. A Game Boy clocked at the
 * normal rate takes a byte in 976 us (gambatte schedules 4096 cycles), which
 * would leave the game 39 us before the next slot where the real adapter leaves
 * it 0.9 ms. Clocked FAST it takes 30 us. FAST is the nearer of the two. */
#define CLOCK_FLAGS LS_FLAG_FAST

#define STATE_MAGIC 0x37304D44u /* "DM07" */
#define STATE_VERSION 1u

typedef struct pending_msg
{
   uint64_t tick;
   uint8_t sb;
   uint8_t flags;
   uint8_t pad[6];
} pending_msg;

typedef struct port_state
{
   uint64_t promised;
   uint32_t pending_count;
   uint8_t peer_sb;
   uint8_t peer_armed;
   uint8_t ever_armed;      /* this Game Boy has been armed since it was cabled */
   uint8_t pad;
   pending_msg pending[LINK_PENDING_MAX];
} port_state;

typedef struct core_state
{
   uint32_t magic;
   uint32_t version;
   uint32_t size;
   uint32_t powered;
   uint64_t now;
   uint64_t next_slot;
   uint64_t packet_start;
   dmg07 adapter;
   port_state port[DMG07_PORTS];
} core_state;

static core_state st;

/* Per process, not per machine: not in a savestate. */
static struct retro_link_interface link_iface;
static int link_have_iface;
static retro_link_port_t *handle[DMG07_PORTS];
static int anchored[DMG07_PORTS];
static unsigned peers[DMG07_PORTS];
static int self_id[DMG07_PORTS];

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

static uint32_t framebuffer[DMG07_PORTS];
static uint8_t last_connected = 0xFF;
static uint8_t last_phase = 0xFF;
static unsigned state_logs[DMG07_PORTS];
static unsigned relay_logs;
static unsigned trace_slots;
static uint32_t trace_from;
static uint8_t timing;
/* The clock's flags: CLOCK_FLAGS, or none with DMG07_CLOCK=normal (a test). */
static uint8_t clock_flags = CLOCK_FLAGS;
/* DMG07_WAIT=1 (a test): hold a slot while a Game Boy that has been taking part
 * is not armed, as GBE+ and DoubleCherryGB effectively do (both only complete a
 * byte once every Game Boy is ready), for up to WAIT_MAX ticks. */
static int wait_ready;
static unsigned wait_logs;
#define WAIT_STEP ((uint64_t)256)
#define WAIT_MAX FRAME_TICKS

static void log_msg(enum retro_log_level level, const char *fmt, ...)
{
   char buf[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   if (log_cb)
      log_cb(level, "[DMG-07] %s", buf);
}

/* ------------------------------------------------------------------------ */
/* The four wires                                                            */
/* ------------------------------------------------------------------------ */

static int cabled(unsigned p)
{
   return handle[p] && peers[p] >= 2;
}

static uint64_t port_advance(unsigned p, uint64_t request, uint32_t *wake)
{
   uint64_t safe = st.now + LINK_HORIZON;
   if (safe > st.port[p].promised)
      st.port[p].promised = safe;
   anchored[p] = 1;
   return link_iface.advance(handle[p], st.now, safe, request, wake);
}

static void port_send(unsigned p, uint64_t tick, uint8_t type, uint8_t sb, uint8_t flags)
{
   uint8_t msg[LS_MSG_SIZE];
   if (!anchored[p])
      port_advance(p, st.now, NULL);
   if (tick < st.port[p].promised)
      tick = st.port[p].promised;
   memset(msg, 0, sizeof(msg));
   msg[0] = type;
   msg[1] = (uint8_t)self_id[p];
   msg[2] = sb;
   msg[3] = flags;
   link_iface.send(handle[p], tick, RETRO_LINK_BROADCAST, msg, sizeof(msg));
}

static void port_refresh(unsigned p)
{
   unsigned was = peers[p];
   unsigned count = 0;
   int id;

   if (!handle[p])
      return;
   id = link_iface.peers(handle[p], &count);
   if (id < 0 || count > 2)
   {
      /* A Game Boy cable joins two, as both Game Boy drivers insist. */
      if (count > 2 && was != 0)
         log_msg(RETRO_LOG_WARN, "player %u: %u machines on one cable; a Game Boy cable carries 2\n",
            p + 1, count);
      self_id[p] = 0;
      peers[p] = 0;
   }
   else
   {
      self_id[p] = id;
      peers[p] = count;
   }
   if (peers[p] != was)
   {
      /* What the old cable carried is not evidence about the new one. */
      st.port[p].pending_count = 0;
      st.port[p].peer_sb = 0xFF;
      st.port[p].peer_armed = 0;
      anchored[p] = 0;
      log_msg(RETRO_LOG_WARN, "player %u: %s\n", p + 1, peers[p] >= 2 ? "a Game Boy is cabled" : "no Game Boy");
   }
}

static void port_pump(unsigned p)
{
   uint8_t buf[64];
   uint64_t tick;
   unsigned from;
   size_t len = sizeof(buf);
   port_state *s = &st.port[p];

   while (link_iface.recv(handle[p], &tick, &from, buf, &len))
   {
      /* Only the Game Boy's state matters: a Game Boy on an adapter never drives
       * the clock (Pan Docs: internal clock "results in garbage data"). */
      if (len == LS_MSG_SIZE && buf[0] == LS_STATE)
      {
         if (s->pending_count < LINK_PENDING_MAX)
         {
            pending_msg *m = &s->pending[s->pending_count++];
            memset(m, 0, sizeof(*m));
            m->tick = tick;
            m->sb = buf[2];
            m->flags = buf[3];
         }
         else
            log_msg(RETRO_LOG_WARN, "player %u: inbox full, dropping a state\n", p + 1);
      }
      else if (len == LS_MSG_SIZE && buf[0] == LS_CLOCK)
         log_msg(RETRO_LOG_WARN, "player %u: the Game Boy drove the clock; the adapter ignores it\n", p + 1);
      len = sizeof(buf);
   }
}

/* Apply every state stamped at or before `tick`. */
static void port_apply(unsigned p, uint64_t tick)
{
   port_state *s = &st.port[p];
   while (s->pending_count > 0 && s->pending[0].tick <= tick)
   {
      uint8_t armed = (s->pending[0].flags & LS_FLAG_ARMED) != 0;
      /* The first few states each Game Boy says, so a log shows whether a game
       * is listening on its serial port at all. */
      if (state_logs[p] < 12 && (armed != s->peer_armed || s->pending[0].sb != s->peer_sb))
      {
         state_logs[p]++;
         log_msg(RETRO_LOG_WARN, "player %u: %s, SB $%02X\n", p + 1,
            armed ? "armed" : "not armed", s->pending[0].sb);
      }
      s->peer_sb = s->pending[0].sb;
      s->peer_armed = armed;
      if (armed)
         s->ever_armed = 1;
      memmove(&s->pending[0], &s->pending[1], sizeof(pending_msg) * (s->pending_count - 1));
      s->pending_count--;
   }
}

static void attach_all(void)
{
   unsigned p;
   if (!link_have_iface)
      return;
   for (p = 0; p < DMG07_PORTS; p++)
   {
      if (handle[p])
         continue;
      handle[p] = link_iface.attach(p, LINK_PROTOCOL, LINK_CLOCK_RATE);
      anchored[p] = 0;
      peers[p] = 0;
      self_id[p] = 0;
      if (!handle[p])
         log_msg(RETRO_LOG_WARN, "the frontend refused link port %u\n", p);
   }
   log_msg(RETRO_LOG_INFO, "attached link ports 0-3 to %s\n", LINK_PROTOCOL);
}

static void detach_all(void)
{
   unsigned p;
   for (p = 0; p < DMG07_PORTS; p++)
   {
      if (handle[p])
         link_iface.detach(handle[p]);
      handle[p] = NULL;
      peers[p] = 0;
      anchored[p] = 0;
   }
}

/* ------------------------------------------------------------------------ */
/* The adapter                                                               */
/* ------------------------------------------------------------------------ */

static void power(int on)
{
   if (on == (int)st.powered)
      return;
   st.powered = (uint32_t)on;
   dmg07_reset(&st.adapter);
   st.adapter.timing = timing;
   st.next_slot = st.now;
   st.packet_start = st.now;
   log_msg(RETRO_LOG_WARN, "%s\n", on ? "powered by player 1's cable: pinging"
                                      : "player 1's cable is in no Game Boy: off");
}

/* One byte slot at st.now: read each Game Boy's register, clock each its byte. */
static void slot(void)
{
   uint8_t in[DMG07_PORTS];
   unsigned answered = 0;
   unsigned p;
   uint64_t byte_gap = dmg07_byte_ticks(&st.adapter);
   uint64_t packet_len = dmg07_packet_ticks(&st.adapter);
   int ended;

   if (st.adapter.index == 0)
      st.packet_start = st.now;

   for (p = 0; p < DMG07_PORTS; p++)
   {
      in[p] = 0xFF;
      if (!cabled(p))
         continue;
      /* An armed Game Boy's register, or the open line of one that is not
       * waiting to be clocked. */
      if (st.port[p].peer_armed)
      {
         in[p] = st.port[p].peer_sb;
         answered |= 1u << p;
      }
      port_send(p, st.now + LINK_HORIZON, LS_CLOCK, dmg07_out(&st.adapter, p), clock_flags);
   }

   /* DMG07_TRACE=1: every slot's exchange for the first transmission packets. */
   if (trace_slots > 0 && st.adapter.packets >= trace_from)
   {
      trace_slots--;
      log_msg(RETRO_LOG_WARN, "slot %u.%u out %02X/%02X/%02X/%02X in %02X%c %02X%c %02X%c %02X%c\n",
         (unsigned)st.adapter.packets, st.adapter.index,
         dmg07_out(&st.adapter, 0), dmg07_out(&st.adapter, 1), dmg07_out(&st.adapter, 2), dmg07_out(&st.adapter, 3),
         in[0], (answered & 1) ? ' ' : '!', in[1], (answered & 2) ? ' ' : '!',
         in[2], (answered & 4) ? ' ' : '!', in[3], (answered & 8) ? ' ' : '!');
   }

   ended = dmg07_in(&st.adapter, in, answered);

   /* What each transmission packet relays, when it changes: the log is where a
    * player's byte reaching everyone else can be read. Rate-limited. */
   if (ended && st.adapter.phase == DMG07_TRANSMIT && relay_logs < 400)
   {
      static uint8_t last_buf[DMG07_MAX_PACKET];
      unsigned len = dmg07_packet_len(&st.adapter);
      if (memcmp(last_buf, st.adapter.buffer, len) != 0)
      {
         char hex[DMG07_MAX_PACKET * 3 + 1];
         unsigned i;
         for (i = 0; i < len; i++)
            snprintf(hex + i * 3, 4, "%02X ", st.adapter.buffer[i]);
         memcpy(last_buf, st.adapter.buffer, len);
         relay_logs++;
         log_msg(RETRO_LOG_WARN, "packet %u relays %s\n", (unsigned)st.adapter.packets, hex);
      }
   }
   st.next_slot = ended ? st.packet_start + packet_len : st.now + byte_gap;

   if (st.adapter.connected != last_connected || st.adapter.phase != last_phase)
   {
      log_msg(RETRO_LOG_WARN, "%s: players %c%c%c%c answer, SIZE %u, RATE $%02X\n",
         dmg07_phase_name(st.adapter.phase),
         (st.adapter.connected & 1) ? '1' : '-', (st.adapter.connected & 2) ? '2' : '-',
         (st.adapter.connected & 4) ? '3' : '-', (st.adapter.connected & 8) ? '4' : '-',
         st.adapter.size, st.adapter.rate);
      last_connected = st.adapter.connected;
      last_phase = st.adapter.phase;
   }
}

/* Whether every Game Boy taking part is armed, so a slot may clock (DMG07_WAIT).
 * A slot held past WAIT_MAX goes ahead anyway: a Game Boy that has stopped
 * listening must not hold the other three for ever. */
static int all_ready(void)
{
   unsigned p;
   if (!wait_ready)
      return 1;
   for (p = 0; p < DMG07_PORTS; p++)
   {
      if (!cabled(p) || !st.port[p].ever_armed || st.port[p].peer_armed)
         continue;
      if (st.now - st.next_slot >= WAIT_MAX)
      {
         if (wait_logs < 20)
         {
            wait_logs++;
            log_msg(RETRO_LOG_WARN, "player %u not armed for a frame: clocking anyway\n", p + 1);
         }
         return 1;
      }
      return 0;
   }
   if (st.now > st.next_slot + WAIT_STEP && wait_logs < 20)
   {
      wait_logs++;
      log_msg(RETRO_LOG_WARN, "a slot waited %llu ticks for every Game Boy to be armed\n",
         (unsigned long long)(st.now - st.next_slot));
   }
   return 1;
}


static void run_frame(void)
{
   uint64_t end = st.now + FRAME_TICKS;
   unsigned p;

   if (!link_have_iface)
   {
      st.now = end;
      return;
   }

   while (st.now < end)
   {
      uint64_t want = st.now + LINK_GRAIN;
      uint64_t grant = (uint64_t)-1;
      int any = 0;

      for (p = 0; p < DMG07_PORTS; p++)
         port_refresh(p);
      power(cabled(0));

      /* A slot that is due but held: look again shortly rather than at once. */
      int held = st.powered && st.now >= st.next_slot && !all_ready();
      if (held)
         want = st.now + WAIT_STEP;
      else if (st.powered && st.next_slot < want)
         want = st.next_slot;
      if (want > end)
         want = end;
      if (want <= st.now)
         want = st.now;

      for (p = 0; p < DMG07_PORTS; p++)
      {
         uint32_t wake = RETRO_LINK_WAKE_NONE;
         uint64_t g;
         if (!cabled(p))
            continue;
         g = port_advance(p, want, &wake);
         port_pump(p);
         if (g != RETRO_LINK_UNBOUNDED && g < grant)
            grant = g;
         any = 1;
      }
      if (!any || grant == (uint64_t)-1 || grant > want)
         grant = want;

      if (grant > st.now)
         st.now = grant;
      for (p = 0; p < DMG07_PORTS; p++)
         if (cabled(p))
            port_apply(p, st.now);

      if (st.powered && st.now >= st.next_slot && all_ready())
         slot();
      else if (grant <= st.now && want > st.now)
         continue;
   }

   /* Say where the frame ended on every wire (the printer's frame-edge note). */
   for (p = 0; p < DMG07_PORTS; p++)
      if (cabled(p) && anchored[p])
         port_advance(p, st.now, NULL);
}

static void present(void)
{
   unsigned p;
   for (p = 0; p < DMG07_PORTS; p++)
   {
      if (st.adapter.connected & (1u << p))
         framebuffer[p] = 0x00FFFFFF;
      else if (cabled(p))
         framebuffer[p] = 0x00404040;
      else
         framebuffer[p] = 0;
   }
   if (video_cb)
      video_cb(framebuffer, DMG07_PORTS, 1, DMG07_PORTS * sizeof(uint32_t));
}

/* ------------------------------------------------------------------------ */
/* libretro                                                                  */
/* ------------------------------------------------------------------------ */

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t cb)
{
   struct retro_log_callback logging;
   bool no_game = true;
   environ_cb = cb;
   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
      log_cb = logging.log;
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

RETRO_API void retro_init(void)
{
   memset(&link_iface, 0, sizeof(link_iface));
   link_have_iface = environ_cb(RETRO_ENVIRONMENT_GET_LINK_INTERFACE, &link_iface)
      || environ_cb(RETRO_ENVIRONMENT_GET_LINK_INTERFACE_FINAL, &link_iface);
   if (!link_have_iface)
      log_msg(RETRO_LOG_WARN, "the frontend has no link bus; nothing can connect\n");
}

RETRO_API void retro_deinit(void)
{
   detach_all();
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name = CORE_NAME;
   info->library_version = CORE_VERSION;
   info->valid_extensions = "";
   info->need_fullpath = false;
   info->block_extract = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->timing.fps = 4194304.0 / 70224.0;
   info->timing.sample_rate = 32768.0;
   info->geometry.base_width = DMG07_PORTS;
   info->geometry.base_height = 1;
   info->geometry.max_width = DMG07_PORTS;
   info->geometry.max_height = 1;
   info->geometry.aspect_ratio = 0.0f;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

RETRO_API void retro_reset(void)
{
   st.powered = 0;
   last_connected = 0xFF;
}

RETRO_API void retro_run(void)
{
   if (input_poll_cb)
      input_poll_cb();
   run_frame();
   present();
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   (void)game;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      log_msg(RETRO_LOG_ERROR, "XRGB8888 is not supported\n");
      return false;
   }
   memset(&st, 0, sizeof(st));
   dmg07_reset(&st.adapter);
   {
      const char *t = getenv("DMG07_TRACE");
      /* DMG07_TRACE=1 traces from the start; =N from packet N. */
      trace_slots = (t && t[0] && t[0] != '0') ? 2400 : 0;
      trace_from = (t && atoi(t) > 1) ? (uint32_t)atoi(t) : 0;
      t = getenv("DMG07_TIMING");
      /* GBE+'s pacing by default: under Pan Docs' the race in Wave Race froze
       * on mGBA a few frames after GO (its HUD gone, player 1 stuck on the line
       * with A held) while gambatte raced, and under GBE+'s both race
       * (2026-11-04, dmg07_probe). */
      timing = (t && strcmp(t, "pandocs") == 0) ? DMG07_TIMING_PANDOCS
             : (t && strcmp(t, "bytes") == 0) ? DMG07_TIMING_BYTES
             : (t && strcmp(t, "dcgb") == 0) ? DMG07_TIMING_DCGB : DMG07_TIMING_GBE;
      log_msg(RETRO_LOG_WARN, "packet timing: %s\n",
         timing == DMG07_TIMING_GBE ? "gbe" : timing == DMG07_TIMING_PANDOCS ? "pandocs"
         : timing == DMG07_TIMING_BYTES ? "bytes" : "dcgb");
      t = getenv("DMG07_CLOCK");
      clock_flags = (t && strcmp(t, "normal") == 0) ? 0 : CLOCK_FLAGS;
      t = getenv("DMG07_WAIT");
      wait_ready = t && t[0] && t[0] != '0';
      if (wait_ready)
         log_msg(RETRO_LOG_WARN, "slots wait for every Game Boy taking part to be armed (DMG07_WAIT)\n");
      if (!clock_flags)
         log_msg(RETRO_LOG_WARN, "clocking at the normal rate (DMG07_CLOCK)\n");
   }
   st.adapter.timing = timing;
   last_connected = 0xFF;
   last_phase = 0xFF;
   attach_all();
   return true;
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

RETRO_API void retro_unload_game(void)
{
   detach_all();
}

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

RETRO_API size_t retro_serialize_size(void)
{
   return sizeof(core_state);
}

RETRO_API bool retro_serialize(void *data, size_t size)
{
   core_state *s = (core_state *)data;
   if (size < sizeof(core_state))
      return false;
   *s = st;
   s->magic = STATE_MAGIC;
   s->version = STATE_VERSION;
   s->size = (uint32_t)sizeof(core_state);
   return true;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
   const core_state *s = (const core_state *)data;
   if (size < sizeof(core_state) || s->magic != STATE_MAGIC
         || s->version != STATE_VERSION || s->size != sizeof(core_state))
      return false;
   st = *s;
   last_connected = 0xFF;
   last_phase = 0xFF;
   return true;
}

RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}

RETRO_API void *retro_get_memory_data(unsigned id)
{
   (void)id;
   return NULL;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
   (void)id;
   return 0;
}
