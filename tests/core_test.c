// core: strings, arenas, buffers, results, cleanup, hashing.
#include <webc/test.h>

TEST(str_helpers) {
  Arena *a = arena_new(4096);
  ASSERT_STR_EQ(str_fmt(a, "%d-%s", 42, "x"), "42-x");
  ASSERT_STR_EQ(str_trim(str_lit("  hi  ")), "hi");
  ASSERT_STR_EQ(str_sub(str_lit("abcdef"), 1, 4), "bcd");
  ASSERT(str_has_prefix_c(str_lit("webc.h"), "webc"));
  ASSERT(str_has_suffix(str_lit("webc.h"), str_lit(".h")));
  ASSERT_EQ(str_index(str_lit("a,b"), ','), 1);
  ASSERT_EQ(str_rindex(str_lit("a.b.c"), '.'), 3);

  i64 n = 0;
  ASSERT(str_to_i64(str_lit("-1234"), &n));
  ASSERT_EQ(n, -1234);
  f64 f = 0;
  ASSERT(str_to_f64(str_lit("1.5"), &f));
  ASSERT_FEQ(f, 1.5);
  ASSERT_STR_EQ(str_fmt(a, "%lld", (long long)i64_max(3, 9)), "9");
  arena_destroy(a);
}

TEST(json_escape_roundtrip) {
  Arena *a = arena_new(4096);
  str raw = str_lit("a\"b\\c\nd\te");
  str esc = json_escape(a, raw);
  // json_escape emits a complete JSON string literal, quotes included
  JParse jp = json_parse(a, str_fmt(a, "{\"k\":%.*s}", (int)esc.n, esc.p));
  ASSERT(jp.ok);
  JVal *k = json_get(jp.v, "k");
  ASSERT_NOT_NULL(k);
  ASSERT_STR(k->s, raw);
  arena_destroy(a);
}

TEST(buf_grows) {
  Arena *a = arena_new(4096);
  Buf b;
  buf_init(&b, a, 4);
  for (int i = 0; i < 1000; i++) buf_puts(&b, "abcde");
  ASSERT_EQ(b.n, 5000);
  buf_free(&b);
  arena_destroy(a);
}

TEST(arena_reset_rewinds) {
  Arena *ar = arena_new(4096);
  (void)arena_alloc(ar, 4000);
  arena_reset(ar);
  ASSERT_EQ(ar->used, 0);
  (void)arena_alloc(ar, 64);
  ASSERT_EQ(ar->used, 64);
  arena_destroy(ar);
}

TEST(result_carries_kind) {
  WErrRes r = wok();
  ASSERT_TRUE(r.ok);
  r = wfail(WEBC_EINVAL, str_lit("bad"));
  ASSERT_FALSE(r.ok);
  ASSERT_EQ(r.e.kind, WEBC_EINVAL);
}

TEST(heap_accounting) {
  void *x = wm_alloc(128);
  ASSERT_NOT_NULL(x);
  WmStats s1 = wm_stats();
  ASSERT_GE(s1.heap_used, 128);
  wm_free(x);
}

TEST(hashing) {
  ASSERT_NE(wc_hash_str(str_lit("abc")), wc_hash_str(str_lit("abd")));
  ASSERT_EQ((i64)wc_hash_str(str_lit("abc")), (i64)wc_hash_str(str_lit("abc")));
}

// /metrics reads wm_stats().arena_live, so it has to be the truth: a create/alloc/
// destroy cycle returns to where it started. Counting a reservation arena_new() never
// made, and subtracting only what was allocated, reports 9 GB of live arenas after
// 300k requests.
TEST(arena_accounting_returns_to_baseline) {
  Arena *a = arena_new(4096);
  size_t base = wm_stats().arena_live;
  for (i32 i = 0; i < 10; i++) (void)arena_alloc(a, 1024);
  ASSERT_GT(wm_stats().arena_live, base);
  arena_destroy(a);
  ASSERT_EQ(wm_stats().arena_live, base);

  // an arena that never allocated reserved nothing
  Arena *b = arena_new(65536);
  ASSERT_EQ(wm_stats().arena_live, base);
  arena_destroy(b);
  ASSERT_EQ(wm_stats().arena_live, base);
}

TEST(an_error_message_outlives_the_arena_that_raised_it) {
  // The message is stored in the error, not borrowed: an error kept after its request
  // arena was reset points at freed memory when the message is a `str`, and the type system
  // cannot tell you. Because it is stored, copying the error copies the message.
  WErrRes r;
  {
    Arena *a = arena_new(1024);
    r = wfail(WEBC_EINVAL, str_fmt(a, "field %s must be %d or less", "quantity", 7));
    arena_destroy(a);
  }
  ASSERT(!r.ok);
  str msg = werr_msg(&r.e);
  ASSERT_STR_EQ(msg, "field quantity must be 7 or less");
  // by value: the copy must be just as readable. On the heap, so that the test is
  // about the error's lifetime and not about the lifetime of a stack temporary.
  WErr *copy = wm_alloc(sizeof *copy);
  *copy = r.e;
  ASSERT_STR_EQ(werr_msg(copy), "field quantity must be 7 or less");
  ASSERT_STR_EQ(werr_kind_name(copy->kind), "invalid");
  wm_free(copy);

  // …and a message longer than the buffer is truncated on purpose, not left dangling.
  char big[512];
  memset(big, 'x', sizeof big);
  WErr longerr = werr(WEBC_EIO, (str){big, sizeof big});
  str lmsg = werr_msg(&longerr);
  ASSERT_EQ((i32)lmsg.n, WC_ERR_MSG_CAP - 1);
  ASSERT(longerr.truncated);
  ASSERT_EQ((int)strlen(longerr.buf), WC_ERR_MSG_CAP - 1);
}

// A Vec's element needs a model descriptor, and a Vec type is declared at file scope
// (the macro defines functions) — exactly the two lines an app's app.h writes.
WEBC_MODEL(Row, (id, i64, .pk = true), (name, str))
Vec(Row);

TEST(each_vlen_vat_take_the_pointer_the_caller_holds) {
  // A Vec travels by pointer, and the value forms turned every use into `at((*v), i)`
  // and `len(*v)` — most of the noise in a real view. The pointer spellings are the
  // ones authors reach for, so they are the ones the headers teach.
  VecRow v = VecRow_with(nullptr);
  Row a = {.id = 1, .name = "one"}, b = {.id = 2, .name = "two"};
  push(v, a);
  push(v, b);
  VecRow *vp = &v;
  ASSERT_EQ((int)vlen(vp), 2);
  ASSERT_EQ((int)vat(vp, 1).id, 2);
  i64 sum = 0;
  const Row *r;
  each(r, vp) sum += r->id;
  ASSERT_EQ((int)sum, 3);
}

TEST_MAIN()