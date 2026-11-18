// WEBC_DOCUMENT: several collections under one JSON object, one declaration for
// both directions — the wire shape a live page sends, checked by the compiler
// instead of hand-encoded and hand-decoded (written for Flowdeck's snapshot).
#include <webc/test.h>
#include <webc/relations.h>

WEBC_ENUM(Lane, backlog, todo, doing, done)
WEBC_MODEL(User, (id, i64, .pk = true), (name, Text, .required = true))
WEBC_MODEL(Card, (id, i64, .pk = true), (lane, Lane, .required = true),
                  (assignee, i64), (title, Text, .required = true))
WEBC_MODEL(ChatMsg, (id, i64, .pk = true), (user, i64), (body, Text, .required = true))
WEBC_MODEL_INTERNAL(User) WEBC_MODEL_INTERNAL(Card) WEBC_MODEL_INTERNAL(ChatMsg)
Vec(User) Vec(Card) Vec(ChatMsg)
WEBC_MODEL_REF(Card, assignee, User, id)

WEBC_DOCUMENT(BoardDoc,
              (v, i64),
              (me, User),
              (users, VecUser),
              (online, VecI64),
              (cards, VecCard),
              (chat, VecChatMsg))

TEST(round_trip_both_directions) {
  Arena *a = arena_new(64 * 1024);
  BoardDoc d = {0};
  d.v = 7;
  d.me = (User){.id = 1, .name = "ada"};
  push(d.users, ((User){.id = 1, .name = "ada"}));
  push(d.users, ((User){.id = 2, .name = "grace"}));
  push(d.online, 1);
  push(d.online, 2);
  push(d.cards, ((Card){.id = 10, .lane = todo, .assignee = 2, .title = "a card"}));
  push(d.chat, ((ChatMsg){.id = 5, .user = 1, .body = "hello"}));

  str json = BoardDoc_write(a, &d);
  ASSERT_STR_CONTAINS(json, "\"v\":7");
  ASSERT_STR_CONTAINS(json, "\"lane\":\"todo\"");   // enums by name inside documents
  ASSERT_STR_CONTAINS(json, "grace");

  BoardDoc back;
  ASSERT_TRUE(BoardDoc_read(a, json, &back));
  ASSERT_EQ(back.v, 7);
  ASSERT_EQ(back.me.id, 1);
  ASSERT_EQ(len(*&back.users), 2);
  ASSERT_EQ(len(*&back.cards), 1);
  ASSERT_EQ((i64)back.cards.at[0].lane, todo);
  ASSERT_STR_EQ(back.chat.at[0].body, "hello");
  arena_destroy(a);
}

TEST(a_stranger_in_a_doc_enum_names_the_field) {
  Arena *a = arena_new(16 * 1024);
  BoardDoc d;
  ASSERT_FALSE(BoardDoc_read(a, str_lit("{\"cards\":[{\"id\":1,\"lane\":\"limbo\",\"title\":\"x\"}]}"), &d));
  arena_destroy(a);
}

TEST(missing_keys_leave_the_zeros_the_app_chose) {
  Arena *a = arena_new(16 * 1024);
  BoardDoc d = {0};
  d.v = 99; // a stale value from a previous frame…
  ASSERT_TRUE(BoardDoc_read(a, str_lit("{\"v\":1}"), &d));
  ASSERT_EQ(d.v, 1);            // …replaced wholesale
  ASSERT_EQ(len(*&d.cards), 0); // and what did not arrive is empty, not stale
  arena_destroy(a);
}

TEST(the_join_links_the_document) {
  Arena *a = arena_new(64 * 1024);
  BoardDoc d = {0};
  push(d.users, ((User){.id = 2, .name = "grace"}));
  push(d.cards, ((Card){.id = 10, .lane = doing, .assignee = 2, .title = "a"}));
  wc_doc_join(a, Card_modelof(), &WC_VECDESC_VecCard, &d.cards, "assignee",
              User_modelof(), &WC_VECDESC_VecUser, &d.users);
  wc_ref_epoch(a); // the rows live in `a`, which is no longer current
  const User *u = ref(Card, &d.cards.at[0], assignee);
  ASSERT_NOT_NULL(u);
  ASSERT_STR_EQ(u->name, "grace");
  arena_destroy(a);
}

TEST_MAIN()
