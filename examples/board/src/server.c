// The server half: the same view rendered with a context, the routes that change it, and the
// live room that tells every open tab about the change.
//
// Every answer is the model — no JSON is written by hand, and no SQL either (the migration
// below is the model's own).
#include <webc/live.h>
#include <webc/server.h>

#include "app.h"

#include <webc/motion.h>

// The board's rows, in the order the board shows them: lane, then position. Every reader of
// the board goes through this, so the page, the API and the broadcast cannot disagree.
static VecCard *board_rows(void) { return db_all_ordered(db_default(), Card, "lane, position"); }

// The document a room carries, and the one every route answers: the whole board, every time. A
// frame that were a diff would leave a client that missed one frame wrong forever (live.h).
static str board_doc(void) {
  BoardDoc d = {.cards = *board_rows()};
  return BoardDoc_write(wc_arena_get(), &d);
}

// `json(200, str)` is bytes that are already JSON, which is the one shape a *document* has:
// `json()` writes a model or a collection, and this answer is a document wrapped around one.
static Response board_reply(i32 status) { return json(status, board_doc()); }

static void board_publish(void) { live_broadcast("board", "1", board_doc()); }

// Cards of one lane, in position order. Insertion sort by hand: a lane is ten rows, and the
// point of the loop is that it is shorter than the call to qsort.
static VecCard lane_rows(VecCard *all, i32 lane) {
  VecCard out = {.desc = &WC_VECDESC_VecCard};
  for_each(c, (*all)) {
    if (c->lane != lane) continue;
    i32 at = vlen(&out);
    while (at > 0 && vat(&out, at - 1).position > c->position) at--;
    Card copy = *c;
    push(out, copy);
    for (i32 i = vlen(&out) - 1; i > at; i--) vat(&out, i) = vat(&out, i - 1);
    vat(&out, at) = copy;
  }
  return out;
}

// Number a lane 0..n-1 and write the rows that changed — position, or the lane itself for the
// card that just crossed in. A kanban re-numbers both lanes it touched, and a board that leaves
// gaps in `position` is one reorder away from sorting the wrong way.
//
// The lane is part of the comparison, not a loop of its own: the card that arrives at index 0
// of a new lane already has position 0, so a check on position alone skipped it and saved
// nothing — the card stayed in the lane it came from.
static void lane_renumber(VecCard *lane, i32 lane_no) {
  for (i32 i = 0; i < vlen(lane); i++) {
    Card *c = &vat(lane, i);
    if (c->lane == lane_no && c->position == i) continue;
    c->lane = lane_no;
    c->position = i;
    db_save(Card, c);
  }
}

GET("/") {
  // the shelf's animation stylesheet, in the page head: one keyframe set, one stagger scale,
  // and the reduced-motion switch that hand-rolled animation always forgets
  motion_head_html();
  return page(Board, &(BoardCtx){.cards = board_rows()});
}

GET("/api/board") { return board_reply(200); }

POST("/api/cards") {
  Card c = body(Card);
  c.position = 1000000; // appends: the lane's sort puts it last
  db_save(Card, &c);
  board_publish();
  // A form post is a browser navigation and wants a page; a fetch wants the new board, the
  // same document the room carries, so the tab that asked can draw it without waiting for
  // its own frame to come back round the room.
  if (wc_body_is_form()) return redirect("/");
  return board_reply(201);
}

// One move, both shapes: a reorder inside a lane (`from_lane == to_lane`) and a card crossing
// into another one. The client sends the index the card should end up at, already adjusted for
// the fact that it left its old list; this route removes first, then inserts, then numbers both
// lanes. Clamping instead of rejecting keeps a drop past the end a move to the bottom.
POST("/api/move") {
  Move m = body(Move);
  VecCard *all = board_rows();
  VecCard source = lane_rows(all, m.from_lane);
  if (m.from < 0 || m.from >= vlen(&source)) fail(422, "no card at %d", (i32)m.from);
  Card moved = vat(&source, m.from);

  // out of the old lane
  for (i32 i = m.from; i < vlen(&source) - 1; i++) vat(&source, i) = vat(&source, i + 1);
  source.n--;

  // and into the new one, at the index the pointer named. Crossing lanes starts from that
  // lane's own rows; a reorder is the source lane with the card put back somewhere else.
  VecCard target = m.to_lane == m.from_lane ? source : lane_rows(all, m.to_lane);
  i32 at = m.to < 0 ? 0 : (m.to > vlen(&target) ? vlen(&target) : (i32)m.to);
  push(target, moved);
  for (i32 i = vlen(&target) - 1; i > at; i--) vat(&target, i) = vat(&target, i - 1);
  vat(&target, at) = moved;

  lane_renumber(&target, m.to_lane);
  if (m.to_lane != m.from_lane) lane_renumber(&source, m.from_lane);

  board_publish();
  return board_reply(200);
}

// A card leaves the board. Soft-delete would be a product decision; a demo deletes.
//
// Deleting a card that is already gone is not an error: the caller asked for "this is not on
// my board", and that is true. Idempotent is the only shape a double click or a retry
// survives — and it keeps the route off a `db_get()`-then-decide, which answers from the row
// pool and would confirm a row this same request just deleted.
POST("/api/delete") {
  Erase e = body(Erase);
  db_delete(Card, e.id);
  board_publish();
  return board_reply(200);
}

// One room, one key: this board is the whole app. `live_join` sends the document immediately,
// then one frame per change, and parks the task until the client goes away.
GET("/sse") { return live_join("board", "1", board_doc()); }

GET("/health") { return res_text(200, "ok"); }