// A keyed list that draws one key twice must not build a node that is its own
// sibling: the second append pointing the first fragment's `next` at itself makes
// every walk of the tree spin — render_ssr first, which is a page that never
// answers, in the server and in the browser alike, with no error anywhere.
//
// The loud failure is the other half: two rows with the unset pk 0 are the
// shape a first page builds by hand (`vec_new` + `push` and no ids), and the
// sentence names the key and says where distinct ids come from.
#include <webc/test.h>
#include <webc/shared.h>

WEBC_MODEL(Item, (id, i64, .pk = true), (name, Text, .required = true))
Vec(Item)

declare_view(Dup, VecItem);

view(Dup, VecItem *items) {
  el("ul") {
    for_each(it, (*items)) {
      el("li") { text("%s", it->name); }
    }
  }
}

static VecItem *hand_built(void) {
  VecItem *items = vec_new(Item);
  push((*items), ((Item){.id = 1, .name = "compass"}));
  push((*items), ((Item){.id = 2, .name = "chart"}));
  return items;
}

GET("/distinct") { return page(Dup, hand_built()); }

GET("/dup") {
  // the trap itself: rows without ids, every key the unset 0
  VecItem *items = vec_new(Item);
  push((*items), ((Item){.name = "compass"}));
  push((*items), ((Item){.name = "chart"}));
  return page(Dup, items);
}

TEST(distinct_keys_render_the_list) {
  Response r = webc_test_request("GET", "/distinct");
  ASSERT_EQ(r.status, 200);
  ASSERT_HTML_COUNT(r, "ul > li", 2);
  ASSERT_HTML_TEXT(r, "ul > li", "compass");
}

TEST(two_rows_with_pk_zero_answer_a_sentence_not_a_spin) {
  // The panic unwinds through the request's frame, exactly like the server: a
  // 500 — and the page *answers*, rather than spinning forever with no error
  // anywhere. A genuine panic stays "internal error" on the wire (a broken
  // handler cannot leak its internals to a stranger); the sentence that names the
  // key and where distinct ids come from is in the log line and the crash report,
  // which is where a person reads it.
  Response r = webc_test_request("GET", "/dup");
  ASSERT_EQ(r.status, 500);
  ASSERT_STR_CONTAINS(r, "internal error");
}
TEST_MAIN()
