// SPDX-License-Identifier: Apache-2.0
// webc/live.h — the room table, the broadcast, and the two pumps (server, client).
//
// A room is a mutex, a `Buf` holding the newest complete document, and a count of who is
// watching. Nothing else: no per-subscriber queue, because a queue is how a burst becomes 200
// renders, and no timer per room, because a room with no subscribers is a struct nobody reads.
#include <webc/live.h>
#include <webc/host.h> // res_live(): the client half wraps it, on both targets

struct WLiveState {
  WLiveRoom rooms[WC_LIVE_ROOMS];
};

static WLiveState g_live;

WLiveState *live_state(void) { return &g_live; }

void live_reset(void) {
  for (i32 i = 0; i < WC_LIVE_ROOMS; i++) {
    if (g_live.rooms[i].used) buf_free(&g_live.rooms[i].doc);
  }
  memset(&g_live, 0, sizeof g_live);
}

WLiveRoom *wc_live_room(const char *room, str key, bool create) {
  if (!room || !*room) room = "-";
  size_t rn = strlen(room);
  if (rn > sizeof g_live.rooms[0].room - 1) rn = sizeof g_live.rooms[0].room - 1;
  size_t kn = key.p ? key.n : 0;
  if (kn > sizeof g_live.rooms[0].key - 1) kn = sizeof g_live.rooms[0].key - 1;
  // Linear over 64 slots is faster than a hash for the sizes involved and needs no bucket
  // chains to get wrong.
  WLiveRoom *free_slot = nullptr;
  for (i32 i = 0; i < WC_LIVE_ROOMS; i++) {
    WLiveRoom *s = &g_live.rooms[i];
    if (!s->used) {
      if (!free_slot) free_slot = s;
      continue;
    }
    if (strlen(s->room) == rn && memcmp(s->room, room, rn) == 0 && strlen(s->key) == kn &&
        (kn == 0 || memcmp(s->key, key.p, kn) == 0))
      return s;
  }
  if (!create) return nullptr;
  WLiveRoom *s = free_slot;
  if (!s) { // the table is full: reclaim the coldest room that nobody is watching
    for (i32 i = 0; i < WC_LIVE_ROOMS; i++) {
      WLiveRoom *c = &g_live.rooms[i];
      if (c->subs > 0) continue;
      if (!s || c->touched < s->touched) s = c;
    }
    if (!s) return nullptr; // every room is live: the app has 64 rooms and all are busy
    buf_free(&s->doc);
  }
  memset(s, 0, sizeof *s);
  memcpy(s->room, room, rn);
  s->room[rn] = 0;
  if (kn) memcpy(s->key, key.p, kn);
  s->key[kn] = 0;
  s->used = true;
  s->touched = time_ms();
  buf_init(&s->doc, wc_arena_get(), 512);
  return s;
}

void wc_live_broadcast(const char *room, str key, str json) {
  WLiveRoom *s = wc_live_room(room, key, true);
  if (!s) return;
  wmutex_lock(&s->mu);
  // The newest document replaces the last one. That single line is the coalescing guarantee:
  // 200 moves between two ticks are one frame, and it is the whole state, not a diff.
  buf_reset(&s->doc);
  if (json.p && json.n) buf_write(&s->doc, json.p, json.n);
  s->seq++;
  s->touched = time_ms();
  wmutex_unlock(&s->mu);
}

str wc_live_read(const char *room, str key) {
  WLiveRoom *s = wc_live_room(room, key, false);
  if (!s) return (str){0};
  wmutex_lock(&s->mu);
  Buf copy;
  buf_init(&copy, wc_arena_get(), s->doc.n ? s->doc.n : 16);
  if (s->doc.n) buf_write(&copy, s->doc.p, s->doc.n);
  wmutex_unlock(&s->mu);
  return buf_str(&copy);
}

i32 wc_live_presence(const char *room, str key) {
  WLiveRoom *s = wc_live_room(room, key, false);
  if (!s) return 0;
  wmutex_lock(&s->mu);
  i32 n = s->subs;
  wmutex_unlock(&s->mu);
  return n;
}

// The head count rides the *document's* event name, not one of its own: the client half of
// this module (live_doc_in) reads a presence frame off the stream before the app's decoder
// ever sees it, and a subscriber asks the bridge for one name. A second event name meant the
// frames arrived and no client listener existed for them — the pill said "0 here" with three
// people looking at the board.
static bool presence_frame(Response *r, i32 n, str event) {
  char buf[40];
  int len = snprintf(buf, sizeof buf, "{\"presence\":%d}", n);
  return sse_send_text(r, event.p && event.n ? event.p : "presence", (str){buf, (size_t)len});
}

// The body of a live stream. It ends when the client goes away, and a failed write is how this
// half learns that: the frame goes to a socket nobody is reading, so the pump stops, the room
// is one subscriber smaller, and the worker is free. A loop that ignored that answer kept the
// count climbing — the pill said "9 here" over two tabs — and parked a worker per closed page.
void wc_live_pump(Response *r, void *ctx) {
  WLiveSub *sub = ctx;
  if (!r || !sub || !sub->room) return;
  WLiveRoom *s = sub->room;
  wmutex_lock(&s->mu);
  s->subs++;
  i32 subs = s->subs;
  wmutex_unlock(&s->mu);

  bool alive = true;
  if (sub->seed.p && sub->seed.n) alive = sse_send_text(r, sub->event.p, sub->seed);
  if (alive) alive = presence_frame(r, subs, sub->event);
  sub->seen_subs = subs;
  if (alive && (!sub->seed.p || !sub->seed.n)) {
    // nothing was seeded by the handler: the room's own copy is the best first frame there is
    str room_doc = wc_live_read(s->room, str_lit(s->key));
    if (room_doc.n) alive = sse_send_text(r, sub->event.p, room_doc);
  }
  wmutex_lock(&s->mu);
  sub->sent = s->seq;
  wmutex_unlock(&s->mu);

  i64 last_beat = time_ms();
  WCtx *live_ctx = webc_req_current() ? webc_req_current()->ctx : nullptr;
  while (alive && (!live_ctx || !ctx_done(live_ctx))) {
    bool sent = false;
    wmutex_lock(&s->mu);
    if (s->seq != sub->sent) {
      str doc = {s->doc.p, s->doc.n};
      Buf copy;
      buf_init(&copy, r->arena, doc.n + 1);
      if (doc.n) buf_write(&copy, doc.p, doc.n);
      sub->sent = s->seq;
      sent = true;
      wmutex_unlock(&s->mu);
      alive = sse_send_text(r, sub->event.p, buf_str(&copy));
    } else {
      wmutex_unlock(&s->mu);
    }
    wmutex_lock(&s->mu);
    i32 now_subs = s->subs;
    wmutex_unlock(&s->mu);
    if (alive && now_subs != sub->seen_subs) {
      alive = presence_frame(r, now_subs, sub->event);
      sub->seen_subs = now_subs;
      sent = true;
    }
    if (!alive) break;
    i64 now = time_ms();
    if (!sent && now - last_beat >= WC_LIVE_KEEPALIVE_MS) {
      // a comment frame: proxies keep an idle stream open, and the write is the liveness probe
      alive = sse_send_text(r, "", str_lit(""));
      last_beat = now;
    }
    if (sent) last_beat = time_ms();
    if (!alive) break;
    // Park, don't spin. The wait is also the coalescing window: whatever happens during it
    // arrives as the room's newest document, once.
    if (live_ctx) ctx_wait(live_ctx, WC_LIVE_TICK_MS);
    else wg_sleep_ms(WC_LIVE_TICK_MS);
  }

  wmutex_lock(&s->mu);
  if (s->subs > 0) s->subs--;
  s->touched = time_ms();
  wmutex_unlock(&s->mu);
  sse_end(r);
}

// ---------------------------------------------------------------- client half
//
// The wasm half only: `res_live` is the bridge's own verb, and a server build has no page to
// subscribe. The declarations are in live.h either way, so one file can carry both halves.
// The client half. It is not `#if defined(__wasm__)` any more: `res_live` lives in host.c, which
// is compiled for every target, so the wrapper compiles for every target too — which is what
// lets a test drive a live link in-process instead of mocking one.
static WLiveLink g_link;
static WLive *g_handle;

// The stream carries two kinds of frame: a presence count, and the app's document. The wrapper
// is what makes one event name do both without a second bridge verb.
static void live_doc_in(str json) {
  if (json.p && json.n && str_contains(json, str_lit("\"presence\""))) {
    // a presence frame is four bytes of number; a full parse would be a second decoder for
    // one integer, so the digits are read where they stand
    i64 n = 0;
    for (size_t i = 0; i < json.n; i++) {
      if (json.p[i] != ':') continue;
      str num = {json.p + i + 1, 0};
      while (num.n + i + 1 < json.n && json.p[i + 1 + num.n] >= '0' && json.p[i + 1 + num.n] <= '9')
        num.n++;
      if (num.n && str_to_i64(num, &n) && g_link.presence) {
        // through the node, so the render happens: a frame is not an action, and writing the
        // slot alone changed a number nobody drew
        i32 here = (i32)n;
        w_write(g_link.presence, &here, sizeof here);
      }
      break;
    }
    return;
  }
  if (g_link.doc) g_link.doc(json);
}

static void live_on_link(int state) {
  // A lost link sends nothing and a regained one resends the seed: the app's decoder is
  // idempotent by construction, because every frame is a whole document.
  g_link.open = state == W_LIVE_STREAMING;
}

void wc_live_connect(const char *url, const char *event, void (*doc)(str json)) {
  g_link.url = url;
  g_link.event = event && *event ? event : "doc";
  g_link.doc = doc;
  g_handle = res_live(&(WLiveCfg){
      .url = str_lit(url),
      .event = str_lit(g_link.event),
      .doc = live_doc_in,
      .on_link = live_on_link,
  });
}

void wc_live_presence_slot(Wn *node) { g_link.presence = node; }

void wc_live_open(void) {
  if (g_handle) res_live_open(g_handle);
}
