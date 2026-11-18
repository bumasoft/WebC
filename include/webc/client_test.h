// SPDX-License-Identifier: Apache-2.0
// webc/client_test.h — the browser half, tested without a browser.
//
// The seams a native test needs to be the bridge: dispatch a control by the name it
// carries, type into a field by its name, press a key by its spec — the same tables the
// real bridge walks. An app's test suite compiles its client sources in (the CLI does it
// already: they are in the test build), so `client_main()` is callable and every action
// is a function. The rule this makes true: anything the browser does through a verb, a
// test can do through a hook.
//
//   CLIENT_TEST(pick_selects_the_leg) {
//     client_main();                       // wires every name, exactly once
//     client_frame("{\"flights\":[…]}");    // the app's own seed door (wc_app_data)
//     client_dispatch("row", "click");     // a click, by name
//     EXPECT_SIGNAL(sel, 7);
//   }
#pragma once

#include <webc/test.h>
#include <webc/dom.h>
#include <webc/host.h>

#define CLIENT_TEST TEST // the same runner; the name says which half is under it

// A click by the name the control carries. The view must have rendered once so the hook
// knows its element (a native render or a server-side one both do).
#define client_dispatch(hook_, ev_) ASSERT_TRUE(wc_test_dispatch((hook_), (ev_)))

// The same, saying where the pointer was: (x, y) inside a box of (w, h). A hover handler is
// then ordinary code a test can watch — `ev_point()` answers what the bridge would have sent,
// so the tooltip and the chart crosshair are not the two things no test ever touched.
#define client_dispatch_at(hook_, ev_, x_, y_, w_, h_)                                               \
  ASSERT_TRUE(wc_test_dispatch_at((hook_), (ev_), (x_), (y_), (w_), (h_)))
bool wc_test_dispatch_at(const char *hook, const char *event, f64 x, f64 y, f64 w, f64 h);

// A keypress by its spec, a typed field by its name — the host's own tables, no page.
#define client_press(spec_) ASSERT_TRUE(wc_client_press((spec_)))
#define client_type(field_, value_) ASSERT_TRUE(wc_client_type((field_), (value_)))

// Signal asserts with the names the app already uses.
#define EXPECT_SIGNAL(sig_, want_)                                                                   \
  ASSERT_EQ((long long)get(sig_), (long long)(want_))
#define EXPECT_SIGNAL_STR(sig_, want_)                                                              \
  ASSERT_TRUE(str_eq(WC_AS_STR(get(sig_)), WC_AS_STR(want_)))
