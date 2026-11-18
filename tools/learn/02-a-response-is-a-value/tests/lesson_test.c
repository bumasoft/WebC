// SPDX-License-Identifier: Apache-2.0
#include <webc/test.h>

TEST(greets_the_reader_by_name) {
  Response r = webc_test_request("GET", "/greet?name=ada");
  ASSERT_EQ(r.status, 200);
  ASSERT_STR_CONTAINS(r, "hello, ada");
}

TEST(a_missing_name_still_answers) {
  Response r = webc_test_request("GET", "/greet");
  ASSERT_EQ(r.status, 200);
  ASSERT_STR_CONTAINS(r, "hello");
}
TEST_MAIN()
