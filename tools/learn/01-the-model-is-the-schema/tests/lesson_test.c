// SPDX-License-Identifier: Apache-2.0
// the test is the lesson's referee: it fails until the model says what it means
#include <webc/test.h>
#include "../src/app.h"

TEST(done_roundtrips_through_the_json_and_the_table) {
  TestClient c = test_client();
  Task t = {.title = "ship it", .done = true};
  Response r = tc_post_json(c, "/api/tasks", Task, &t);
  ASSERT_EQ(r.status, 201);
  ASSERT_JSON(r, "done", "true");
  ASSERT_JSON(webc_test_request("GET", "/api/tasks"), "0.done", "true");
}
TEST_MAIN()
