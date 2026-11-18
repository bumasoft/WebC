// client_harness: the browser half, tested without a browser (include/webc/client_test.h).
//
// The zero-JS round's three late bugs were all client-side and none was caught by a
// suite, because there was no way to run the client half natively. The seams existed —
// the bridge's tables — but nothing an app could reach. These tests prove the hands:
// press a key, type a field, dispatch a control, and read the signals they move.
#include <webc/client_test.h>

state(i64, picked, 0);
state(Text, typed, "");

action(pick) {
  i64 id = ev_attr_i64("data-id");
  if (id > 0) set(picked, id);
}

action(field_dest) { set(typed, persist(text_of(ev_value()))); }

action(pal_open) { set(picked, -1); }

// what client_main does, without the mount: wire the names. No once-guard — a test
// starts from a reset table (wc_host_reset) and wires again, the way each page load does.
static void wire(void) {
  bind("row", "click", pick);
  bind_field("dest", field_dest);
  on_key("meta+k", pal_open);
}

TEST(pressing_a_key_runs_its_action_by_spec) {
  wc_host_reset();
  wire();
  set(picked, 0);
  client_press("meta+k");
  EXPECT_SIGNAL(picked, -1);
  // a spec nothing registered is false, not a crash — that is itself the assertion
  ASSERT_TRUE(!wc_client_press("ctrl+f12"));
}

TEST(typing_a_field_hands_the_action_its_text) {
  wc_host_reset();
  wire();
  set(typed, "");
  client_type("dest", "LHR");
  EXPECT_SIGNAL_STR(typed, "LHR");
  // the value arrives in a call arena; the signal kept its own copy
  client_type("dest", "SIN");
  EXPECT_SIGNAL_STR(typed, "SIN");
  ASSERT_TRUE(!wc_client_type("nowhere", "x"));
}

TEST_MAIN()