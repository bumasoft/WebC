// pwa: the manifest is the config, the worker's cache name is the build, and the worker never
// caches anything that is not a GET on this origin.
#include <webc/test.h>
#include <webc/pwa.h>

static void cfg(void) {
  pwa_init(&(WPwa){.name = "Meridian", .short_name = "Meridian", .theme = "#111312",
                   .icon = "/icon-192.png", .mask_icon = "/icon-512.png", .standalone = true,
                   .offline = "/offline"});
}

TEST(the_manifest_is_the_config_and_nothing_else) {
  cfg();
  ASSERT_TRUE(pwa_configured());
  Response r = pwa_manifest();
  ASSERT_EQ(r.status, 200);
  str body = {r.body.p, r.body.n};
  ASSERT_TRUE(str_contains(body, str_lit("\"name\":\"Meridian\"")));
  ASSERT_TRUE(str_contains(body, str_lit("\"start_url\":\"/\"")));
  ASSERT_TRUE(str_contains(body, str_lit("\"display\":\"standalone\"")));
  ASSERT_TRUE(str_contains(body, str_lit("\"theme_color\":\"#111312\"")));
  ASSERT_TRUE(str_contains(body, str_lit("512x512")));
  ASSERT_TRUE(str_contains(body, str_lit("maskable")));
}

TEST(a_quote_in_a_name_does_not_break_the_json) {
  pwa_init(&(WPwa){.name = "Ada's \"notes\"", .theme = "#000"});
  Response r = pwa_manifest();
  str body = {r.body.p, r.body.n};
  ASSERT_TRUE(str_contains(body, str_lit("Ada's \\\"notes\\\"")));
}

TEST(the_cache_name_is_the_build) {
  cfg();
  str name = str_lit(pwa_cache_name());
  ASSERT_TRUE(str_contains(name, str_lit("webc-")));
  // and it is stable within one process — a worker whose cache name changed per request would
  // empty the cache on every load
  ASSERT_TRUE(str_eq(name, str_lit(pwa_cache_name())));
}

TEST(the_worker_is_two_rules_and_says_so) {
  cfg();
  Response r = pwa_worker();
  str js = {r.body.p, r.body.n};
  ASSERT_TRUE(str_contains(js, str_lit("const CACHE = 'webc-")));
  ASSERT_TRUE(str_contains(js, str_lit("const OFFLINE = '/offline'")));
  ASSERT_TRUE(str_contains(js, str_lit("req.method !== 'GET'")));      // nothing but a GET
  ASSERT_TRUE(str_contains(js, str_lit("url.origin !== location.origin"))); // and nothing off-site
  ASSERT_TRUE(str_contains(js, str_lit("req.mode === 'navigate'")));   // network-first page
  ASSERT_TRUE(str_contains(js, str_lit("caches.keys()")));             // old builds are evicted
}

TEST(nothing_configured_means_nothing_to_install) {
  pwa_init(nullptr);
  ASSERT_FALSE(pwa_configured());
}

TEST_MAIN()
