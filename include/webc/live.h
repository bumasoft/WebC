// SPDX-License-Identifier: Apache-2.0
// webc/live.h — a room: everyone looking at the same thing sees the same state.
//
// WebC already has the four pieces and no product. `res_sse` streams. The client's `WLive`
// holds two generations so a frame is decoded into memory the last one no longer uses. The
// model's own document encoder writes the frame. What is missing is *rooms* — and the three
// bugs every hand-rolled version has:
//
//   1. a frame that is a diff, so a client that missed one frame is wrong forever;
//   2. a timer per dead tab, so a closed laptop costs a wake-up a second forever;
//   3. a frame per row, so a burst of 200 moves is 200 renders of the same list.
//
// This module is those three answers: **a frame is a complete document**, **a room with no
// subscribers has no loop**, and **broadcasts coalesce** — the room keeps only the newest
// document, so a burst is one frame on the wire.
//
// Server half:
//
//   GET("/sse/board/:id") {
//     return live_join("board", path("id"), board_read("id"));   // blocks; one frame per change
//   }
//
//   POST("/api/board/:id/move") {
//     Board b = board_apply(path_i64("id"), body(Move));
//     live_broadcast("board", path("id"), Board_doc(&b));        // json: the model's codec
//     return json(200, Board, &b);
//   }
//
// `live_join` sends the current document immediately (so a tab that opens late is never blank),
// then a `presence` frame whenever the room's head count changes, then one `doc` frame per
// change, and a comment line every 15 s so a proxy does not close an idle stream.
//
// Client half — the same room, through the store that already exists:
//
//   static void on_doc(str json) { board_apply_json(&board, json); }   // app's decoder
//   action(ready) { live_connect("/sse/board/7", "doc", on_doc); }
//   state(i32, here, 0);
//   ...
//   live_presence(here);        // the head count lands here; the pill re-renders
//
// `live_connect` is a *wrapper* around `res_live()`: same bridge verb, same two generations,
// same reconnect — plus the presence frame, which it reads off the stream before the app's
// decoder ever sees it. That is why this module adds no verb: the bridge is 20 KB every app
// pays, and a shelf module does not get to make it heavier.
//
// **Refuses:** CRDTs, offline merge, per-field patches, cursor interpolation, chat typing
// indicators, and persisting a room — a room is process memory, and the state of record is the
// database the broadcast was built from.
#pragma once

#include <webc/core.h>
#include <webc/react.h>
#include <webc/http.h>
#include <webc/json.h>
#include <webc/task.h>
#include <webc/time.h>

#define WC_LIVE_ROOMS 64
#define WC_LIVE_TICK_MS 120
#define WC_LIVE_KEEPALIVE_MS 15000

typedef struct WLiveRoom {
  char room[24];
  char key[48];
  WMutex mu;
  Buf doc;         // the newest complete document, JSON as the model wrote it
  u64 seq;         // bumped on every broadcast
  i32 subs;        // live subscribers
  i64 touched;     // when it was last used, for reclaiming a slot
  bool used;
} WLiveRoom;

typedef struct WLiveState WLiveState;
WLiveState *live_state(void);
void live_reset(void); // a test's door: forget every room

// The room's slot, created on demand. The table is fixed (64 rooms): a room that is evicted
// costs its subscribers a rejoin, which is what a reconnect does anyway, and a table that grows
// under load is the thing an outage is made of.
WLiveRoom *wc_live_room(const char *room, str key, bool create);

// Publish the whole document. Nobody subscribed is not an error and not a queue: the state of
// record is the database, and the next subscriber reads it there.
void wc_live_broadcast(const char *room, str key, str json);
// The room's document, "" when the room has never been written to.
str wc_live_read(const char *room, str key);
i32 wc_live_presence(const char *room, str key);

#define live_broadcast(room_, key_, json_) \
  wc_live_broadcast((room_), WC_AS_STR(key_), WC_AS_STR(json_))
#define live_doc(room_, key_) wc_live_read((room_), WC_AS_STR(key_))
#define live_presence_count(room_, key_) wc_live_presence((room_), WC_AS_STR(key_))

// The frame a room carries, encoded by the model that owns the rows — the same bytes
// `json(200, cards)` answers `GET /api/board` with, so a subscriber and a tab that just loaded
// the page cannot disagree about what the board is.
//
//   str doc = live_json(cards);            // [{"id":1,"title":"…"}, …]
//   live_broadcast("board", "7", doc);
//
// A hand-written JsonWriter in the app is this function with the app's arena in it, and
// `webc lint` complains about it for the same reason.
static inline str wc_live_json(const void *vp) {
  const WVecDesc *d = vp ? *(const WVecDesc *const *)vp : nullptr;
  JsonWriter w;
  jw_init(&w, wc_arena_get(), 512);
  jw_arr_begin(&w);
  if (d) {
    const void *at = *(void *const *)((const char *)vp + d->off_at);
    i64 n = (i64)*(const i32 *)((const char *)vp + d->off_n);
    for (i64 i = 0; i < n; i++)
      model_json_write(&w, d->sub, (const char *)at + i * d->elem_size);
  }
  jw_arr_end(&w);
  return jw_finish(&w);
}
#define live_json(v_) wc_live_json((const void *)(v_))

// ---------------------------------------------------------------- the stream

typedef struct WLiveSub {
  WLiveRoom *room;
  str event;      // the event name the app's decoder listens for
  u64 sent;       // the room's seq this subscriber has already received
  i32 seen_subs;  // the head count it last told the client
  str seed;       // the document to send first (from the app's own read)
  size_t frames;
} WLiveSub;

void wc_live_pump(Response *r, void *ctx);

// Join a room and stream it. `seed` is the document the app already read (a `Board_doc()`),
// so the first frame is the truth the handler saw, not a stale room copy.
static inline Response wc_live_join(const char *room, str key, str seed, const char *event) {
  WLiveRoom *slot = wc_live_room(room, key, true);
  WLiveSub *sub = wc_alloc(sizeof *sub);
  memset(sub, 0, sizeof *sub);
  sub->room = slot;
  sub->event = event ? str_lit(event) : str_lit("doc");
  sub->seed = seed;
  sub->seen_subs = -1;
  return res_sse(wc_live_pump, sub);
}
#define live_join(room_, key_, seed_) \
  wc_live_join((room_), WC_AS_STR(key_), WC_AS_STR(seed_), "doc")
#define live_join_as(room_, key_, seed_, event_) \
  wc_live_join((room_), WC_AS_STR(key_), WC_AS_STR(seed_), (event_))

// ---------------------------------------------------------------- client half
//
// One link per page is the rule (a second stream is a second reconnect loop and a second
// generation pair); a page that needs two rooms has one screen too many.
typedef struct WLiveLink {
  const char *url;
  const char *event;
  void (*doc)(str json);
  Wn *presence;
  bool open;
} WLiveLink;

void wc_live_connect(const char *url, const char *event, void (*doc)(str json));
// The *signal*, not its slot: a presence frame arrives outside any action, so a plain write to
// the slot would change the number and draw nothing. Going through the node is what makes the
// head count appear (`live_presence(here)` names the state a view already reads).
void wc_live_presence_slot(Wn *node);
void wc_live_open(void); // from on_ready: subscribe to the link live_connect() declared
#define live_connect(url_, event_, doc_) wc_live_connect((url_), (event_), (doc_))
#define live_presence(N_) wc_live_presence_slot(&N_##_node)
#define live_open() wc_live_open()