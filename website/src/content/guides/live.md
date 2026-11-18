Real-time in WebC is one decision made early: **the server sends whole documents, not deltas.** A
frame is the board as it is; the client replaces what it had and re-renders. That removes the entire
class of bugs where a delta is applied twice, out of order, or to a client that missed one.

## The server side

```c
// src/board.c — one frame, seeded with the truth the handler just read
#include <webc/server.h>
#include <webc/live.h>

WEBC_MODEL(Card, (id, i64, .pk = true), (title, Text))
Vec(Card)

GET("/api/board/7/stream") {
  VecCard *cards = db_all(Card);
  str doc = live_json(cards);           // exactly the bytes json(200, cards) sends
  return live_join("board", "7", doc);  // SSE, seeded with it
}
```

`live_join(room, key, seed)` opens an SSE stream, sends `seed` as the first frame, and then sends the
room's document whenever it changes. `live_join_as(...)` picks the event name. A mutation broadcasts:

```c
// src/board.c — a write, and the room tells whoever is watching
#include <webc/server.h>
#include <webc/live.h>

WEBC_MODEL(Card, (id, i64, .pk = true), (title, Text))
Vec(Card)

void add_card(Text title) {
  Card k = {.title = title};
  db_save(Card, &k);
  // the whole document, again
  live_broadcast("board", "7", live_json(db_all(Card)));
}
```

`live_json(collection)` encodes with the model that owns the rows, so a subscriber and a tab that just
loaded the page cannot disagree about what the board is. A hand-written `JsonWriter` in the app is
that function with the app's arena in it — which is how the board was written before it existed.

- The room table is **fixed at 64 rooms**. A room that is evicted costs its subscribers a rejoin,
  which is what a reconnect does anyway; a table that grows under load is what an outage is made of.
- Publishing to nobody is not an error and not a queue. The state of record is the database, and the
  next subscriber reads it there.
- `live_doc(room, key)` reads the room's document; `live_presence_count()` is how many are in it.

## The client side

```c
// src/client.c — the other half of the same room
#include <webc/web.h>
#include <webc/live.h>
#include <webc/host.h>                 // on_ready()
#include <webc/shared.h>

state(i32, here, 0);              // how many are looking; the header renders this
WEBC_MODEL(Card, (id, i64, .pk = true), (title, Text))
Vec(Card)

static VecCard g_cards;

action(subscribe) { live_open(); }        // runs once, when the page is mounted

// the app's decoder: decode, replace, redraw
static void board_doc(str json) {
  Arena *a = wc_arena_get();
  JParse p = json_parse(a, json);
  JVal  *arr = (p.ok && p.v && p.v->kind == J_ARR) ? p.v : nullptr;
  if (!arr) return;
  // the whole document replaces the whole list
  g_cards.n = 0;
  for (i32 i = 0; i < arr->n; i++) {
    Card c;
    if (model_read_json_val(a, Card_modelof(), json_at(arr, i), &c))
      push(g_cards, c);
  }
}

client_main {
  live_connect("/api/board/7/stream", "doc", board_doc);
  // the room's head count lands in the signal — the state the header already reads
  live_presence(here);
  on_ready(subscribe);             // subscribe once the page is mounted
  return 0;
}
```

One link per page is the rule: a second stream is a second reconnect loop and a second generation
pair. A page that needs two rooms has one screen too many.

## Frames and generations

A stream's memory discipline is a verb, because every streaming client otherwise writes the same
hundred lines by hand. `WFrame` on the server and `WStore` on the client each own **two
generations**: the
frame being built and the one still in use, so a frame's strings die when the frame after next
replaces it.

```c
// src/board.c — the frame being sent, on the server
#include <webc/server.h>

static str snapshot_json(Arena *a) { (void)a; return str_lit("[]"); }

void build_a_frame(void) {
  WFrame fr;
  frame_init(&fr, 96 * 1024);
  Arena *prev = frame_begin(&fr);      // the fresh generation, made current
  str   frame  = snapshot_json(prev);  // everything allocated here lives in it
  frame_end(&fr, prev);                // the generation before it is now dead
  (void)frame;
}
```

```c
// src/client.c — the document in hand, in the browser
#include <webc/web.h>
#include <webc/shared.h>

WEBC_MODEL(Card, (id, i64, .pk = true), (title, Text))
Vec(Card)
WEBC_DOCUMENT(BoardDoc, (cards, VecCard))

static WStore g_room;
static bool   g_started;

void take_frame(str json) {
  if (!g_started) {              // once: the two generations, allocated here
    store_init(&g_room, BoardDoc_docof(), sizeof(BoardDoc));
    g_started = true;
  }
  BoardDoc *d = store_read(&g_room, json);   // NULL on a bad frame; state unchanged
  if (!d) return;
  (void)store_cur(&g_room);                  // between frames
  (void)store_arena(&g_room);                // where a decode allocates
}
```

## When you write the stream yourself

`res_sse(fn, ctx)` hands the body to a function that owns a socket for as long as it runs. The three
facts that shape a correct stream: a subscriber wakes on a channel rather than spinning, a heartbeat
is what notices a dead tab, and a failed write is how the loop ends.

```c
// src/stream.c — the loop live.h runs, written out
#include <webc/server.h>
#include <webc/task.h>

typedef struct Sub {
  ChanPtr wake;      // the room pushes one value per change
  Response *r;
} Sub;

static str snapshot_json(void) { return str_lit("[]"); }

static void stream_run(Response *r, void *vp) {
  Sub *s = vp;
  s->r = r;
  for (;;) {
    void *ignored = nullptr;
    SelCase cs[1] = {SEL_RECV(s->wake.c, &ignored)};
    // wake, or a heartbeat at 15 s
    i32 got = select_wait(cs, 1, 15000);
    // the room closed the channel
    if (got == SELECT_CLOSED) break;
    if (got == SELECT_TIMEOUT) {
      if (!sse_send_text(r, "ping", str_lit("1"))) break;   // a dead tab fails here
      continue;
    }
    // so does a dead socket
    if (!sse_send_text(r, "sync", snapshot_json())) break;
  }
}

static Sub g_sub;

// res_sse, one line
GET("/api/board/7/stream") { return sse(stream_run, &g_sub); }
```

The framework's `live.h` is that loop, with the coalescing: a hundred writes inside one frame produce
one send, and a client that was slow gets the newest document rather than a queue of them.

## More than one process

A room lives in the process. Behind a load balancer, put Redis pub/sub underneath:

```c
// src/fanout.c — every process hears every publish, each keeps its own rooms
#include <webc/server.h>
#include <webc/live.h>
#include <webc/redis.h>

static void on_msg(str channel, str payload, void *arg) {
  (void)channel; (void)arg;                  // the payload IS the newest document
  // a real app reads these off the message
  wc_live_broadcast("board", str_lit("7"), payload);
}

void fan_out(WRedis *r) {
  // one subscription per process
  redis_subscribe(r, str_lit("board"), on_msg, nullptr);
}
```

`sse_send_text` and `redis_publish` are the two lines that make the fan-out work; the room table stays
per process, which is the right granularity for presence.

## See also

- Reference [§7d](../reference/http.html) — the stream pattern in full, and `res_sse`.
- [Redis and the query cache](../guides/data.html) — the layer under the fan-out.
- `examples/board` — a live kanban with drag and drop, end to end.
