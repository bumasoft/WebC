// SPDX-License-Identifier: Apache-2.0
#include <webc/test.h>
#include "../src/app.h"

TEST(an_empty_email_is_refused_and_named) {
  Response r = webc_test_request("POST", "/signup");
  ASSERT_EQ(r.status, 422);
  ASSERT_STR_CONTAINS(r, "email");
}

TEST(a_real_email_is_welcome) {
  Signup s = {.email = "ada@example.com"};
  TestClient c = test_client();
  Response r = tc_post_json(c, "/signup", Signup, &s);
  ASSERT_EQ(r.status, 201);
}
TEST_MAIN()
