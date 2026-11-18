// live: a frame is a whole document, a burst is one frame, and a room nobody watches keeps
// nothing warm.
#include <webc/test.h>
#include <webc/live.h>

TEST(the_room_keeps_the_newest_document_whole) {
  live_reset();
  live_broadcast("board", "7", "{\"v\":1}");
  live_broadcast("board", "7", "{\"v\":2}");
  ASSERT_STR_EQ(live_doc("board", "7"), "{\"v\":2}");
}

TEST(a_burst_is_one_frame_not_two_hundred) {
  live_reset();
  live_broadcast("board", "7", "{\"v\":0}");
  WLiveRoom *r = wc_live_room("board", str_lit("7"), false);
  u64 before = r->seq;
  for (i32 i = 1; i <= 200; i++) {
    char buf[32];
    snprintf(buf, sizeof buf, "{\"v\":%d}", i);
    live_broadcast("board", "7", buf);
  }
  // every broadcast bumped the sequence, but the room holds one document, not 200
  ASSERT_EQ(r->seq - before, 200);
  ASSERT_STR_EQ(live_doc("board", "7"), "{\"v\":200}");
  ASSERT_TRUE(r->doc.n < 16);
}

TEST(two_rooms_do_not_see_each_other) {
  live_reset();
  live_broadcast("board", "7", "{\"v\":7}");
  live_broadcast("board", "8", "{\"v\":8}");
  ASSERT_STR_EQ(live_doc("board", "7"), "{\"v\":7}");
  ASSERT_STR_EQ(live_doc("board", "8"), "{\"v\":8}");
  ASSERT_STR_EQ(live_doc("board", "9"), "");
}

TEST(a_room_nobody_wrote_to_has_no_document) {
  live_reset();
  ASSERT_STR_EQ(live_doc("nope", "1"), "");
  ASSERT_EQ(live_presence_count("nope", "1"), 0);
  // reading a room must not create it
  ASSERT_TRUE(wc_live_room("nope", str_lit("1"), false) == nullptr);
}

TEST(the_table_is_fixed_and_the_cold_room_gives_way) {
  live_reset();
  // 64 slots, 100 rooms: none of them watched, so the oldest is reclaimed rather than grown
  for (i32 i = 0; i < 100; i++) {
    char key[16];
    snprintf(key, sizeof key, "%d", i);
    live_broadcast("board", key, "{\"v\":1}");
  }
  // the newest room is still there, and the first one was reclaimed
  ASSERT_STR_EQ(live_doc("board", "99"), "{\"v\":1}");
  ASSERT_STR_EQ(live_doc("board", "0"), "");
}

TEST(a_long_room_key_is_cut_not_overflowed) {
  live_reset();
  live_broadcast("board",
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                 "{\"v\":1}");
  ASSERT_STR_EQ(live_doc("board",
                         "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
                "{\"v\":1}");
}

TEST_MAIN()
