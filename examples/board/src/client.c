// The client half: the same view the server rendered, mounted over the page's markup, plus the
// four things only a browser can do — pick a card up, put it down, take a live document, and
// say how many people are here.
//
// A shared view *names* its controls (`named("card")`); this file says what the names do. That
// split is what lets one view compile for both halves: the server has no actions to point at.
// It is also why the shortcut table lives here and not in the view — a `WCmd` carries a
// function pointer, and a function pointer does not exist on the server (the open part of P4).
//
// The drag is pointer events, not the HTML drag family: a planchette that follows the mouse is
// the same code on a trackpad, a pen and a thumb, and `ev_point()` already hands an action the
// pointer — page coordinates, the element's box, and where inside it the pointer is. That is
// the whole of what a drag needs to know, and the index maths is `dnd.h`'s.
#include "app.h"

#include <webc/host.h>
#include <webc/keys.h>
#include <webc/live.h>
#include <webc/web.h>

// The board the views read, and the store the frames arrive in. A frame's strings belong to the
// frame: they die when the callback returns and the next frame reuses the bytes, which is a board
// drawing one request's titles out of another request's memory. `WStore` is the framework's answer
// — two generations that take turns, so the rows a render is reading outlive it — and `g_cards` is
// the live generation, copied row for row on every good frame.
static VecCard g_cards = {.desc = &WC_VECDESC_VecCard};
static BoardCtx C = {.cards = &g_cards};
static WStore g_room;

// Every frame is the whole board, so loading one is the same operation as loading the seed:
// decode, replace, redraw. A decoder that is idempotent is what makes a reconnect free.
static void board_load(str json) {
  if (!g_room.doc && !store_init(&g_room, BoardDoc_docof(), sizeof(BoardDoc))) return;
  BoardDoc *doc = store_read(&g_room, json);
  if (!doc) return; // a frame nobody can read leaves the board as it was
  g_cards = doc->cards;
}

void wc_app_data(str json) { board_load(json); }

ACTION(new_open)
ACTION(new_close)
ACTION(lane_add)
ACTION(card_del)
ACTION(card_pick)
ACTION(card_hover)
ACTION(lane_hover)
ACTION(board_hover)
ACTION(card_drop)
ACTION(card_up)
ACTION(card_down)
ACTION(card_left)
ACTION(card_right)
ACTION(escape)

static const WCmd CMDS[] = {
    K("New card", "n", new_open),
    // one action for Escape: a dialog that is open closes, a card in the air goes back
    K("Close or cancel", "escape", escape),
    // the keyboard half of drag: `dnd_keys()` announces these two on every row, and an
    // announcement with no binding is a lie (`webc check` cannot see keys.h tables)
    K("Move card up", "alt+arrowup", card_up),
    K("Move card down", "alt+arrowdown", card_down),
    K("Move card left", "alt+arrowleft", card_left),
    K("Move card right", "alt+arrowright", card_right),
};

// ---------------------------------------------------------------- the two halves of a drag

// `dnd.h`'s arithmetic, asked about a lane that may not be the card's own. For the source lane
// the card that left is missing from the list the index counts, which is the off-by-one the
// module exists for; for any other lane nothing left, so the raw index is already the answer.
static i32 insert_index(i32 lane, i32 raw, bool after, i32 over) {
  WDrag d = {.from = lane == (i32)get(drag_lane) ? (i32)get(drag_from) : -1,
             .over = over,
             .after = after,
             .n = cards_in(&g_cards, lane)};
  if (d.from < 0) return raw;
  return dnd_drop(&d); // -1 when the card would land where it already is
}

static void drag_clear(void) {
  set(carrying, false);
  set(drag_id, (i64)-1);
  set(drag_title, fmt(""));
  set(drag_lane, -1);
  set(drag_from, -1);
  set(drop_lane, -1);
  set(drop_at, -1);
}

// ---------------------------------------------------------------- the new-card dialog

action(new_open) {
  set(new_lane, 0);
  set(open_new, true);
}

// Every lane has its own way in, and the dialog remembers which one asked: "add a card" is a
// kanban gesture, and a card that always lands in Todo is a card you then have to drag.
action(lane_add) {
  set(new_lane, (i32)ev_attr_i64("data-lane"));
  set(open_new, true);
}

action(new_close) { set(open_new, false); }

// The toast clears itself only because the app says so: `overlay` draws, the app owns the
// clock (`webc_later` is one timer per verb, re-armed if called again).
static void clear_toast(WJs r) {
  (void)r;
  set(toast_msg, "");
}

// Every mutating route answers the whole board — the same document `live_broadcast` carries.
// This tab applies its own answer (the person who dropped the card sees it land immediately)
// and a frame from the room brings any other tab. One decoder, two sources, no guessing.
static void board_reply(WJs r) {
  if (!r.ok) {
    set(toast_msg, persist(fmt("%s", text_of(r.error.p ? r.error : str_lit("could not save")))));
    webc_later(2600, clear_toast);
    return;
  }
  board_load(r.json);
}

// ---------------------------------------------------------------- delete

action(card_del) {
  i64 id = ev_attr_i64("data-id");
  if (id <= 0) return;
  Card *c = card_by_id(&g_cards, id);
  // `persist`: this sentence is read two seconds after the action that wrote it, so it cannot
  // live in the action's arena. One short copy per move is the price of a toast with a name in it.
  set(toast_msg,
      c && c->title ? persist(fmt("“%s” deleted", c->title)) : persist(fmt("Card deleted")));
  webc_post(str_lit("/api/delete"), WC_AS_STR(fmt("{\"id\":%lld}", (long long)id)), board_reply);
  webc_later(2200, clear_toast);
  // a card deleted mid-drag must not be dropped anywhere afterwards
  if ((i64)get(drag_id) == id) drag_clear();
}

// ---------------------------------------------------------------- the drag

// How far the pointer has to travel before a press becomes a drag. Small enough that a drag
// feels immediate, large enough that a shaky hand clicking a button does not move a card.
#define DRAG_SLOP 4.0

// `carrying` is what the view draws the planchette from; `drag_id` is only "this press began
// on a card". Arming is the moment between them: until it happens the board looks untouched.
static bool drag_arm(WPoint p) {
  if (get(carrying)) return true;
  f64 dx = p.cx - get(press_x), dy = p.cy - get(press_y);
  if (dx > -DRAG_SLOP && dx < DRAG_SLOP && dy > -DRAG_SLOP && dy < DRAG_SLOP) return false;
  set(carrying, true);
  return true;
}

action(card_pick) {
  // A control inside the card owns its own press: dragging must not start from the delete
  // corner, or the click that was meant for it is swallowed by a drag that goes nowhere.
  if (str_eq_c(ev_attr("data-hook"), "card_del")) return;
  i64 id = ev_attr_i64("data-id");
  Card *c = card_by_id(&g_cards, id);
  i32 from = (i32)ev_attr_i64("data-dnd-i");
  if (!c || from < 0) return;
  WPoint p = ev_point();
  set(drag_id, id);
  // `persist`: the rows came out of a decoded frame, and the ghost outlives that arena
  set(drag_title, c->title ? persist(c->title) : fmt(""));
  set(drag_lane, (i32)ev_attr_i64("data-lane"));
  set(drag_from, from);
  set(drop_lane, -1);
  set(drop_at, -1);
  set(ghost_x, p.cx);
  set(ghost_y, p.cy);
  // where the press started: a press that never moved is a click, not a drag, and must not
  // shuffle the board (a card that jumps when you click it is the oldest kanban bug there is)
  set(press_x, p.cx);
  set(press_y, p.cy);
  set(drag_w, p.w);

}

// The pointer is over a card: the index comes from which half of the row it is in.
action(card_hover) {
  if (get(drag_id) <= 0 || !drag_arm(ev_point())) return;
  WPoint p = ev_point();
  i32 lane = (i32)ev_attr_i64("data-lane");
  i32 i = (i32)ev_attr_i64("data-dnd-i");
  bool after = p.h > 0 && p.y > p.h / 2;
  set(ghost_x, p.cx);
  set(ghost_y, p.cy);
  set(drop_lane, lane);
  set(drop_at, insert_index(lane, after ? i + 1 : i, after, i));
}

// The pointer is over the lane itself — below its last card or in an empty lane: append.
action(lane_hover) {
  if (get(drag_id) <= 0 || !drag_arm(ev_point())) return;
  WPoint p = ev_point();
  i32 lane = (i32)ev_attr_i64("data-lane");
  i32 n = cards_in(&g_cards, lane);
  set(ghost_x, p.cx);
  set(ghost_y, p.cy);
  set(drop_lane, lane);
  set(drop_at, insert_index(lane, n, false, n));
}

// The pointer is over the board but not over a lane (a gap, the margin): the card follows, and
// nothing would be dropped here.
action(board_hover) {
  if (get(drag_id) <= 0 || !drag_arm(ev_point())) return;
  WPoint p = ev_point();
  set(ghost_x, p.cx);
  set(ghost_y, p.cy);
  set(drop_lane, -1);
  set(drop_at, -1);
}

action(card_drop) {
  if (get(drag_id) <= 0) return;
  WPoint p = ev_point();
  i32 to_lane = (i32)get(drop_lane);
  i32 to = (i32)get(drop_at);
  i32 from_lane = (i32)get(drag_lane), from = (i32)get(drag_from);
  i64 id = (i64)get(drag_id);
  bool moved = get(carrying); // a press that never armed is a click, and changes nothing
  drag_clear();
  if (!moved || to_lane < 0 || to < 0) return;
  if (to_lane == from_lane && to == from) return;
  (void)id;
  // The server applies the move and broadcasts the new board; this tab is one of the
  // subscribers, so it does not have to guess what the result looks like.
  webc_post(str_lit("/api/move"),
            WC_AS_STR(fmt("{\"from_lane\":%d,\"from\":%d,\"to_lane\":%d,\"to\":%d}", from_lane,
                          from, to_lane, to)),
            board_reply);
  // A reorder is not a change of lane: saying "Moved to Todo" for a card that never left Todo
  // is the sort of sentence that makes a person doubt the board.
  set(toast_msg, from_lane == to_lane ? persist(fmt("Moved in %s", lane_name(to_lane)))
                                      : persist(fmt("Moved to %s", lane_name(to_lane))));
  webc_later(2200, clear_toast);
}

// ---------------------------------------------------------------- the keyboard move
//
// The same server, the same broadcast, one row instead of a pointer. The row is found by the
// two numbers `dnd` wrote on it and the board added; the arithmetic is the server's (it clamps
// a move past the end, which is what makes alt+↓ on the last card harmless).
static void move_by(i32 lanes, i32 rows) {
  i64 id = ev_attr_i64("data-id");
  Card *c = card_by_id(&g_cards, id);
  i32 lane = (i32)ev_attr_i64("data-lane");
  i32 from = (i32)ev_attr_i64("data-dnd-i");
  if (!c || from < 0) return;
  i32 to_lane = lane + lanes;
  i32 to;
  if (lanes) {
    if (to_lane < 0 || to_lane >= LANES) return;
    to = cards_in(&g_cards, to_lane); // the bottom of the lane it moves into
  } else {
    i32 n = cards_in(&g_cards, lane);
    to = from + rows;
    if (to < 0) to = 0;
    if (to >= n) to = n - 1;
    if (to == from) return;
  }
  webc_post(str_lit("/api/move"),
            WC_AS_STR(fmt("{\"from_lane\":%d,\"from\":%d,\"to_lane\":%d,\"to\":%d}", lane, from,
                          to_lane, to)),
            board_reply);
  // `persist` on every branch: the sentence outlives the action that wrote it, and a toast that
  // points into the action's own arena is reading freed bytes by the time it is drawn.
  set(toast_msg, persist(lanes                      ? fmt("Moved to %s", lane_name(to_lane))
                         : rows < 0                 ? fmt("Moved up")
                                                    : fmt("Moved down")));
  webc_later(2200, clear_toast);
}

action(card_up) { move_by(0, -1); }
action(card_down) { move_by(0, 1); }
action(card_left) { move_by(-1, 0); }
action(card_right) { move_by(1, 0); }

action(escape) {
  if (get(open_new)) {
    set(open_new, false);
    return;
  }
  if (get(drag_id) > 0) drag_clear();
}

client_main {
  keys_bind(CMDS);
  bind("new_open", "click", new_open);
  // the close hook exists only while the dialog is open (a control behind a branch, which is
  // exactly what bind_soft is for)
  bind_soft("new_close", "click", new_close);
  // every lane carries both add hooks, so `bind_soft` and not `bind`: a board rendered before
  // the client's first paint, or with no cards at all, still has them — but a test that renders
  // one lane should not hear about the others
  bind_soft("lane_add", "click", lane_add);
  bind_soft("card_del", "click", card_del);
  // `bind_soft`: a card hook exists only once there is a card, so a board whose suite ran first
  // would otherwise open with `bind("card")` shouting that no element carries it
  bind_soft("card", "pointerdown", card_pick);
  bind_soft("card", "pointermove", card_hover);
  bind_soft("lane", "pointermove", lane_hover);
  bind_soft("board", "pointermove", board_hover);
  // the drop is on all three: a card is dropped on a card, on a lane, or in the gap between two
  bind_soft("card", "pointerup", card_drop);
  bind_soft("lane", "pointerup", card_drop);
  bind_soft("board", "pointerup", card_drop);
  // a pointer the browser takes away (a phone call, a scroll gesture) ends the drag in place
  bind_soft("card", "pointercancel", escape);
  bind_soft("lane", "pointercancel", escape);
  bind_soft("board", "pointercancel", escape);
  live_connect("/sse", "doc", board_load);
  live_presence(here); // the head count lands in the state the header reads
  live_open();
  mount(Board, &C, "#root");
  return 0;
}