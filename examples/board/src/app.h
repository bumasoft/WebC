// The board's whole data model, and the one thing both halves need to agree on: what a lane is
// called. Nothing in this file can reach a database or a request — that is what
// <webc/shared.h> is for, and the compiler enforces it.
#pragma once

#include <webc/dnd.h> // WDrag: the index maths the rows and the drop share
#include <webc/shared.h>

WEBC_MODEL(Card, (id, i64, .pk = true),
           (title, Text, .required = true, .max = 80),
           (lane, i32, .not_null = true, .gte = 0, .lte = 2),
           (position, i32, .not_null = true, .gte = 0))

// A move, as the client computed it: the card that leaves `(from_lane, from)` and the index it
// takes in `(to_lane, to)`. `to` is the position in the target lane *once the card has left* —
// the off-by-one that makes a hand-written kanban shuffle the wrong card is dnd.h's, not the
// app's. A move is a request body, validated by `body(Move)` and never stored, so
// `WEBC_MODEL_INTERNAL` is the one line that says "no table for this" — without it the schema
// check asks for a `moves` table nobody meant to create.
WEBC_MODEL(Move, (from_lane, i32, .gte = 0, .lte = 2), (from, i32, .gte = 0),
           (to_lane, i32, .gte = 0, .lte = 2), (to, i32, .gte = 0))
WEBC_MODEL_INTERNAL(Move)

// The same trick for the one other body the board takes: a card id to drop.
WEBC_MODEL(Erase, (id, i64, .gte = 1))
WEBC_MODEL_INTERNAL(Erase)

Vec(Card)

// The whole board as one document, which is what a room carries and what every route answers.
// A bare array would do for the wire; a named document is what `WStore` decodes (`store_read`),
// and a frame decoded into a store generation is a frame whose strings live exactly as long as
// the board shows them — the alternative is a hand-rolled arena per frame.
WEBC_DOCUMENT(BoardDoc, (cards, VecCard))

#define LANES 3

static inline const char *lane_name(i32 lane) {
  switch (lane) {
  case 0: return "Todo";
  case 1: return "Doing";
  case 2: return "Done";
  default: return "—";
  }
}

// The view's context: the rows the server read. One struct, so the same view renders from
// either half and the client's mutations are the only difference between them.
typedef struct BoardCtx {
  VecCard *cards;
} BoardCtx;

declare_view(Board, BoardCtx);

// The interaction states. They are declared here rather than in view.c because two files read
// them: the view draws the drag and the toast, the client's actions write them. `state` makes
// one weak slot per name, so the writer and the reader are the same variable whatever file
// each is compiled into.
state(bool, open_new, false);
state(i32, new_lane, 0);     // the lane the New card dialog will add to
state(Text, toast_msg, "");

state(i64, drag_id, -1);     // the card the press landed on, by id (0 = none)
state(bool, carrying, false); // and whether the press has become a drag the board should draw
state(Text, drag_title, ""); // its title, for the card that follows the pointer
state(i32, drag_lane, -1);   // the lane it came from
state(i32, drag_from, -1);   // and its index inside that lane
state(i32, drop_lane, -1);   // the lane the pointer is over
state(i32, drop_at, -1);     // where the card would land there, already adjusted
state(f64, ghost_x, 0);      // the carried card's position on the page
state(f64, ghost_y, 0);
state(f64, press_x, 0);      // where the press started: a click is not a drag
state(f64, press_y, 0);
state(f64, drag_w, 0);       // the picked card's width: the ghost is the size of the card       // the picked card's width: the ghost is the same size as the card
state(i32, here, 0);         // how many people are looking at this board

// How many cards are in a lane, and the nth of them: two lines both halves share, so the
// server and the client cannot disagree about what a lane contains.
//
// A plain `for`, not `for_each`: `for_each` is a *render* loop — each pass opens a keyed item
// scope on the node currently open and sweeps the items the pass did not claim. Calling it to
// count runs that sweep on whatever element the view happens to have open (`fe.h` says so in
// as many words).
static inline i32 cards_in(VecCard *cards, i32 lane) {
  i32 n = 0;
  for (i32 i = 0; cards && i < vlen(cards); i++) n += vat(cards, i).lane == lane ? 1 : 0;
  return n;
}

static inline Card *card_at_lane(VecCard *cards, i32 lane, i32 i) {
  i32 at = 0;
  for (i32 k = 0; cards && k < vlen(cards); k++) {
    Card *c = &vat(cards, k);
    if (c->lane != lane) continue;
    if (at == i) return c;
    at++;
  }
  return nullptr;
}

static inline Card *card_by_id(VecCard *cards, i64 id) {
  for (i32 k = 0; cards && k < vlen(cards); k++)
    if (vat(cards, k).id == id) return &vat(cards, k);
  return nullptr;
}