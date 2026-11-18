// The board. Server-rendered for the first paint, hydrated in the browser, patched in place
// afterwards — one view, both targets, one context type.
//
// What this file does *not* contain, and would in a hand-written kanban: the pointer maths that
// turns a drag into a drop index (`dnd.h`), the dialog's focus trap and Escape (`overlay.h`), an
// enter animation with a reduced-motion switch (`motion.h`), the icons (`icons.h`), the toast
// (`overlay.h`), the keyboard hint (`keys.h`). What is left is the board.
//
// The one thing it *does* say, because no module can: a card may cross lanes. `dnd.h` refuses
// cross-list drag on purpose — "the app says what crossing means" — and for a kanban it means
// "leave this lane, land at this index of that one". So the math below is per lane (which index
// inside the lane the pointer is over) and the client decides what the pair means.
#include "app.h"

#include <webc/icons.h>
#include <webc/keys.h>
#include <webc/motion.h>
#include <webc/overlay.h>
#include <webc/ui.h>

static const WOverlay NEW_CARD = {"new_card", "New card", "new_close", "new-title"};


// One card, in the board and in the air. The ghost is the same markup so the thing that follows
// the pointer is the thing that was picked up: same size, same type, same words.
static void card_face(const Card *card) {
  el("span") { class_("title"); text(card->title); }
}

view(Board, BoardCtx *c) {
  bool carrying = get(carrying); // armed: the press has become a drag
  i32 drop_lane = (i32)get(drop_lane);
  i32 drop_at = (i32)get(drop_at);
  i32 drag_lane = (i32)get(drag_lane);
  i32 drag_from = (i32)get(drag_from);

  el("div") {
    class_(carrying ? "app is-dragging" : "app");

    el("header") {
      class_("top");
      el("div") {
        class_("brand");
        el("span") { class_("mark"); text("W"); }
        el("div") {
          el("h1") { text("Product board"); }
          el("p") {
            class_("sub");
            text("Drag a card between lanes — every move is a round trip into C.");
          }
        }
      }
      el("div") {
        class_("top-actions");
        el("span") {
          class_("presence");
          attr("aria-live", "polite");
          aria_label("people looking at this board");
          icon(WC_ICON_USERS, 0);
          // "3 here" is the app's copy; the module hands over the number and says nothing
          text("%lld here · %d cards", (long long)get(here), vlen(c->cards));
        }
        el("button") {
          class_("primary");
          named("new_open");
          icon(WC_ICON_PLUS, 0);
          text("New card");
          keycap("n");
        }
      }
    }

    el("main") {
      class_("lanes");
      named("board"); // the fallback drop target: the gaps between lanes
      for (i32 lane = 0; lane < LANES; lane++) {
        i32 n = cards_in(c->cards, lane);
        bool hot = carrying && drop_lane == lane;
        el("section") {
          // one `class_` call: it *replaces* the attribute, so a second one would drop the first
          class_(classes("lane", lane == 0 ? "lane-todo" : lane == 1 ? "lane-doing" : "lane-done",
                         hot ? "is-target" : ""));
          attr("data-lane", str_from_i64(wc_arena_get(), lane));
          named("lane"); // the pointer bindings: over a card it is the card, else this
          el("div") {
            class_("lane-head");
            el("span") {
              class_("lane-name");
              el("span") { class_("dot"); attr("aria-hidden", "true"); }
              el("h2") { text(lane_name(lane)); }
              el("span") { class_("count"); text("%d", n); }
            }
            el("button") {
              class_("icon-btn lane-add");
              named("lane_add");
              attr_i("data-lane", lane);
              aria_label(fmt("Add a card to %s", lane_name(lane)));
              icon(WC_ICON_PLUS, 0);
            }
          }

          el("ul") {
            class_("rows");
            // One WDrag per lane: the module answers "which index inside this lane", and the
            // source lane's own drag is the only one that has a card missing from it
            WDrag drag = {.from = get(drag_lane) == lane ? (i32)get(drag_from) : -1,
                          .over = drop_lane == lane ? drop_at : -1,
                          .n = n};
            dnd_list(&drag, n);
            // Where the seam goes, in the *rendered* list. `dnd_drop()` answers an index into
            // the lane as it would be with the carried card gone — right for the move, off by
            // one for a line drawn between two rows that are both still on screen. Putting the
            // card back into its own slot is the whole conversion.
            i32 seam_at = -1;
            if (hot && drop_at >= 0)
              seam_at = (carrying && drag_lane == lane && drop_at > drag_from) ? drop_at + 1 : drop_at;
            for (i32 i = 0; i < n; i++) {
              Card *card = card_at_lane(c->cards, lane, i);
              if (!card) continue;
              bool source = carrying && card->id == (i64)get(drag_id);
              bool seam_before = seam_at == i && !source;
              bool seam_after = seam_at >= n && i == n - 1 && !source;
              el("li") {
                class_(classes("card", source ? "is-source" : "",
                               seam_before ? "drop-before" : "",
                               seam_after ? "drop-after" : ""));
                attr("tabindex", "0");
                // The row's two numbers: the lane, and this card's index inside it. `dnd_row()`
                // would also write them — and `draggable="true"`, which starts the browser's own
                // drag and stops the pointer moving; a pointer drag owns the gesture here.
                attr_i("data-lane", lane);
                attr_i("data-dnd-i", i);
                attr("aria-grabbed", source ? "true" : "false");
                dnd_keys();
                named("card");
                attr_i("data-id", card->id);
                enter("rise");
                stagger(i);
                card_face(card);
                el("button") {
                  class_("card-del");
                  named("card_del");
                  attr_i("data-id", card->id);
                  aria_label(fmt("Delete “%s”", card->title));
                  icon(WC_ICON_TRASH, 0);
                }
              }
            }
            if (n == 0) {
              el("li") {
                class_(carrying && hot ? "blank is-hot" : "blank");
                text(carrying && hot ? "Drop it here" : "Nothing here yet");
              }
            }
          }

          el("button") {
            class_("lane-foot");
            named("lane_add");
            attr_i("data-lane", lane);
            icon(WC_ICON_PLUS, 0);
            text("Add a card");
          }
        }
      }
    }

    el("p") {
      class_("keys");
      text("Pick a card up with the mouse or a finger. From the keyboard: ");
      keycap("tab");
      text(" to a card, then ");
      keycap("alt+arrowup");
      keycap("alt+arrowdown");
      text(" to reorder it inside its lane and ");
      keycap("alt+arrowleft");
      keycap("alt+arrowright");
      text(" to move it to another one.");
    }

    // The card in the air: `position: fixed` at the pointer, and the same markup as the card on
    // the board, so the thing that follows the pointer is the thing that was picked up. The
    // pointer never stops on it (`pointer-events: none`), so it cannot steal the events that
    // move it. Inside `.app`, because a conditional *top-level* sibling is a shape the patch
    // left on the page after it stopped being rendered — a ghost that outlived the drag.
    if (carrying) {
      el("div") {
        class_("drag-ghost");
        style_fmt("left:%gpx;top:%gpx;width:%gpx", get(ghost_x), get(ghost_y), get(drag_w));
        attr("aria-hidden", "true");
        el("span") {
          class_("card");
          Card lifted = {.id = (i64)get(drag_id), .title = get(drag_title)};
          card_face(&lifted);
        }
      }
    }
  }

  // The new-card dialog. Emitted only while it is open (`overlay` owns the scrim, the focus
  // move and the panel's role), and it is a real form: with JavaScript off it posts and
  // redirects; with JavaScript on the same markup is the dialog.
  dialog(&NEW_CARD, get(open_new)) {
    el("h2") { text("New card"); }
    el("p") {
      class_("dialog-sub");
      text("It lands at the bottom of ");
      el("strong") { text(lane_name((i32)get(new_lane))); }
      text(".");
    }
    el("form") {
      post_to("/api/cards");
      el("label") {
        attr("for", "new-title");
        text("Title");
      }
      el("input") {
        attr("id", "new-title");
        name("title");
        placeholder("What needs doing?");
        required();
        attr("maxlength", "80");
        // The caret belongs in this field, not on the panel: the dialog knows it by name
        // (NEW_CARD.focus), so nothing here has to happen after the batch.
      }
      el("input") {
        type_("hidden");
        name("lane");
        value_(fmt("%d", (i32)get(new_lane)));
      }
      el("div") {
        class_("dialog-actions");
        el("button") { type_("submit"); class_("primary"); text("Add card"); }
        // the third door out of a dialog, next to the scrim and Escape: all three carry the
        // same hook, and the wiring pass can see this one
        el("button") { type_("button"); class_("ghost"); named("new_close"); text("Cancel"); }
      }
    }
  }

  // The toast the drop and delete actions set. `overlay` draws it and says nothing about time:
  // the app clears the state, because a timer inside a module is hidden state.
  if (get(toast_msg) && *get(toast_msg))
    toasts("") { toast(get(toast_msg), "ok"); }
}