// Both halves are C, so both are testable in one binary: the page the server renders, the API
// it answers, the reorder the module computed, and the document the room would broadcast.
#include <webc/test.h>

#include "app.h"
#include <webc/live.h>

static void boot(void) {
  setenv("DATABASE_URL", "sqlite::memory:", 1);
  webc_boot();
  db_default();
}

TEST_SETUP(boot);

// Each test starts from an empty board: one database is one suite, and a test that inherited
// its neighbour's rows would be testing the neighbour.
static void wipe(void) { db_exec_sql(db_default(), "delete from cards"); }

static i64 add(const char *title, i32 lane) {
  Card c = {.title = title, .lane = lane, .position = 1000000};
  db_save(Card, &c);
  return c.id;
}

// The board as the page shows it: lane, then position, the order every reader uses.
static const char *title_at(i32 lane, i32 i) {
  VecCard *cards = db_all_ordered(db_default(), Card, "lane, position");
  i32 seen = 0;
  for_each(c, (*cards)) {
    if (c->lane != lane) continue;
    if (seen++ == i) return c->title;
  }
  return "";
}

static i32 lane_size(i32 lane) {
  VecCard *cards = db_all_ordered(db_default(), Card, "lane, position");
  i32 n = 0;
  for_each(c, (*cards))
    if (c->lane == lane) n++;
  return n;
}

TEST(the_page_is_the_board_three_lanes_of_cards) {
  wipe();
  add("write the proposal", 0);
  add("ship it", 1);
  Response r = webc_test_request("GET", "/");
  ASSERT_EQ(r.status, 200);
  ASSERT_HTML(r, "main.lanes section.lane");
  ASSERT_HTML(r, "section.lane.lane-todo");
  ASSERT_HTML_TEXT(r, "section.lane:nth-child(1) h2", "Todo");
  ASSERT_HTML_TEXT(r, "section.lane:nth-child(1) .count", "1");
  ASSERT_HTML_TEXT(r, "section.lane:nth-child(3) .count", "0");
  ASSERT_HTML(r, "section.lane ul.rows li.card");
  // a card is a row the keyboard can reach, and it says which lane and which row it is
  ASSERT_HTML(r, "li.card[tabindex=\"0\"][data-lane=\"0\"][data-dnd-i=\"0\"]");
  // an empty lane is still a place to drop something
  ASSERT_HTML(r, "section.lane:nth-child(3) li.blank");
}

TEST(a_card_is_validated_by_the_model) {
  wipe();
  const char *empty = "{\"title\":\"\",\"lane\":0}";
  Response bad = webc_test_request_ex("POST", "/api/cards", "Content-Type: application/json",
                                      empty, (i32)strlen(empty));
  ASSERT_EQ(bad.status, 422);

  const char *ok = "{\"title\":\"a card\",\"lane\":1}";
  Response made = webc_test_request_ex("POST", "/api/cards", "Content-Type: application/json", ok,
                                       (i32)strlen(ok));
  ASSERT_EQ(made.status, 201);
  // the answer to a fetch is the whole board — the same document the room carries — so the tab
  // that added the card draws it without waiting for its own frame to come back round the room
  ASSERT_JSON(made, "cards[0].title", "a card");
  ASSERT_EQ(lane_size(1), 1);
}

TEST(a_move_reorders_inside_one_lane) {
  wipe();
  add("one", 0);
  add("two", 0);
  add("three", 0);
  // take the first card out, put it back at index 1 — "after two", now that one has left
  const char *move = "{\"from_lane\":0,\"from\":0,\"to_lane\":0,\"to\":1}";
  Response r = webc_test_request_ex("POST", "/api/move", "Content-Type: application/json", move,
                                    (i32)strlen(move));
  ASSERT_EQ(r.status, 200);
  ASSERT_TRUE(strcmp(title_at(0, 0), "two") == 0);
  ASSERT_TRUE(strcmp(title_at(0, 1), "one") == 0);
  ASSERT_TRUE(strcmp(title_at(0, 2), "three") == 0);
  ASSERT_EQ(lane_size(0), 3);
}

TEST(a_move_crosses_lanes_and_numbers_both) {
  wipe();
  add("one", 0);
  add("two", 0);
  add("a", 1);
  // "one" into the middle of the other lane
  const char *move = "{\"from_lane\":0,\"from\":0,\"to_lane\":1,\"to\":1}";
  Response r = webc_test_request_ex("POST", "/api/move", "Content-Type: application/json", move,
                                    (i32)strlen(move));
  ASSERT_EQ(r.status, 200);
  ASSERT_EQ(lane_size(0), 1);
  ASSERT_TRUE(strcmp(title_at(0, 0), "two") == 0);
  ASSERT_EQ(lane_size(1), 2);
  ASSERT_TRUE(strcmp(title_at(1, 0), "a") == 0);
  ASSERT_TRUE(strcmp(title_at(1, 1), "one") == 0);
  // no gaps: a lane whose positions are 0,2 is one reorder away from sorting the wrong way
  VecCard *cards = db_all_ordered(db_default(), Card, "lane, position");
  for_each(c, (*cards)) ASSERT_TRUE(c->position < 2);
}

TEST(a_move_past_the_end_lands_last_instead_of_failing) {
  wipe();
  add("one", 0);
  add("two", 0);
  const char *past = "{\"from_lane\":0,\"from\":0,\"to_lane\":0,\"to\":99}";
  Response ok = webc_test_request_ex("POST", "/api/move", "Content-Type: application/json", past,
                                     (i32)strlen(past));
  ASSERT_EQ(ok.status, 200);
  ASSERT_TRUE(strcmp(title_at(0, 0), "two") == 0);
  ASSERT_TRUE(strcmp(title_at(0, 1), "one") == 0);
}

TEST(a_move_that_names_no_card_is_refused) {
  wipe();
  add("one", 0);
  const char *bad = "{\"from_lane\":0,\"from\":7,\"to_lane\":1,\"to\":0}";
  Response r = webc_test_request_ex("POST", "/api/move", "Content-Type: application/json", bad,
                                    (i32)strlen(bad));
  ASSERT_EQ(r.status, 422);
  ASSERT_EQ(lane_size(0), 1);
}

TEST(a_card_can_be_deleted_and_the_lane_closes_up) {
  wipe();
  i64 id = add("one", 0);
  add("two", 0);
  char body[64];
  snprintf(body, sizeof body, "{\"id\":%lld}", (long long)id);
  Response r = webc_test_request_ex("POST", "/api/delete", "Content-Type: application/json", body,
                                    (i32)strlen(body));
  ASSERT_EQ(r.status, 200);
  ASSERT_EQ(lane_size(0), 1);
  ASSERT_TRUE(strcmp(title_at(0, 0), "two") == 0);

  // and deleting it again is a 200: the second click gets the board it asked for. A 404 here
  // would turn a retry into an error box for a card that is already off the board.
  Response again = webc_test_request_ex("POST", "/api/delete", "Content-Type: application/json",
                                        body, (i32)strlen(body));
  ASSERT_EQ(again.status, 200);
  ASSERT_EQ(lane_size(0), 1);
}

TEST(the_room_carries_the_whole_board) {
  wipe();
  add("live", 2);
  const char *made = "{\"title\":\"second\",\"lane\":2}";
  webc_test_request_ex("POST", "/api/cards", "Content-Type: application/json", made,
                       (i32)strlen(made));
  live_reset();
  // the route publishes on every write; the room holds the newest document whole
  const char *third = "{\"title\":\"third\",\"lane\":0}";
  webc_test_request_ex("POST", "/api/cards", "Content-Type: application/json", third,
                       (i32)strlen(third));
  str doc = live_doc("board", "1");
  ASSERT_TRUE(doc.n > 0);
  ASSERT_TRUE(str_contains(doc, str_lit("third")));
  ASSERT_TRUE(str_contains(doc, str_lit("live")));
}

TEST(a_form_post_lands_on_the_board_not_on_json) {
  wipe();
  // the dialog works with JavaScript off: a real form, a redirect, a rendered page
  const char *form = "title=from+a+form&lane=1";
  Response r = webc_test_request_ex("POST", "/api/cards",
                                    "Content-Type: application/x-www-form-urlencoded", form,
                                    (i32)strlen(form));
  ASSERT_EQ(r.status, 303);
  ASSERT_EQ(lane_size(1), 1);
  ASSERT_TRUE(strcmp(title_at(1, 0), "from a form") == 0);
}

TEST_MAIN()