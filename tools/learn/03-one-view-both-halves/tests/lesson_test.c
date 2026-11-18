// SPDX-License-Identifier: Apache-2.0
#include <webc/test.h>
#include "../src/app.h"

TEST(the_page_lists_the_items) {
  Response r = webc_test_request("GET", "/");
  ASSERT_HTML_COUNT(r, "ul > li", 2);
  ASSERT_HTML_TEXT(r, "ul > li", "compass");
}

TEST(the_note_is_on_the_page) {
  Response r = webc_test_request("GET", "/");
  ASSERT_HTML_TEXT(r, "p.note", "3 voyages out");
}
TEST_MAIN()
