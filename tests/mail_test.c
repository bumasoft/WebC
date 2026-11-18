// mail: the text part comes from the HTML part, so a multipart/alternative cannot diverge — and
// the outbox is a file, so the whole path is testable without a relay.
#include <webc/test.h>
#include <webc/mail.h>

typedef struct ReceiptCtx {
  str name;
  str link;
} ReceiptCtx;

view(Receipt, ReceiptCtx *r) {
  el("h1") { text("Your receipt"); }
  el("p") { text(str_lit("Thanks, ")); text(r->name); text(" — £4.20 paid."); }
  el("p") {
    text("Order ");
    el("a") {
      attr("href", r->link);
      text("view it online");
    }
  }
  el("ul") { el("li") { text("One coffee"); } el("li") { text("One bun"); } }
  el("script") { text("void 0"); }
}

TEST(the_text_part_is_the_html_part_stood_up) {
  Arena *a = wc_arena_get();
  Buf b;
  buf_init(&b, a, 256);
  buf_puts(&b, "<h1>Receipt</h1><p>Thanks, Ada &amp; Bo.</p><ul><li>One</li><li>Two</li></ul>");
  str t = mail_html_to_text(buf_str(&b));
  ASSERT_STR_EQ(t, "Receipt\n\nThanks, Ada & Bo.\n\n- One\n- Two");
}

TEST(a_link_keeps_its_address_in_the_text_part) {
  Arena *a = wc_arena_get();
  Buf b;
  buf_init(&b, a, 128);
  buf_puts(&b, "<p>See <a href=\"https://x.test/r/9\">your order</a>.</p>");
  str t = mail_html_to_text(buf_str(&b));
  ASSERT_TRUE(str_contains(t, str_lit("your order")));
  ASSERT_TRUE(str_contains(t, str_lit("https://x.test/r/9")));
}

TEST(a_script_is_dropped_not_printed) {
  Arena *a = wc_arena_get();
  Buf b;
  buf_init(&b, a, 128);
  buf_puts(&b, "<p>hi</p><script>alert(1)</script><style>p{}</style>");
  str t = mail_html_to_text(buf_str(&b));
  ASSERT_STR_EQ(t, "hi");
}

TEST(the_two_parts_come_from_one_view) {
  WMail m = mail_new("ada@x.test", "Your receipt");
  ReceiptCtx r = {.name = str_lit("Ada"), .link = str_lit("https://x.test/r/9")};
  mail_view(&m, Receipt, &r);
  ASSERT_TRUE(m.html.n > 0);
  ASSERT_TRUE(str_contains(buf_str(&m.html), str_lit("Your receipt")));
  ASSERT_EQ(m.text.n, 0); // not yet: the text part is derived on request, not guessed early
  wc_mail_sync_text(&m);
  ASSERT_TRUE(str_contains(buf_str(&m.text), str_lit("Thanks, Ada")));
  ASSERT_TRUE(str_contains(buf_str(&m.text), str_lit("https://x.test/r/9")));
  // the script tag the view wrote never reaches the text part
  ASSERT_FALSE(str_contains(buf_str(&m.text), str_lit("void 0")));
}

TEST(the_mime_has_both_parts_and_no_header_injection) {
  WMail m = mail_new("ada@x.test", "Hi\r\nBcc: evil@x.test");
  buf_puts(&m.html, "<p>hello</p>");
  buf_puts(&m.text, "hello");
  str mime = mail_mime(&m);
  ASSERT_TRUE(str_contains(mime, str_lit("multipart/alternative")));
  ASSERT_TRUE(str_contains(mime, str_lit("text/plain")));
  ASSERT_TRUE(str_contains(mime, str_lit("text/html")));
  ASSERT_FALSE(str_contains(mime, str_lit("\nBcc:"))); // a newline in a subject is a header
  ASSERT_TRUE(str_contains(mime, str_lit("Subject: Hi  Bcc:"))); // ... and became spaces, on one line
  ASSERT_TRUE(str_contains(mime, str_lit("Message-ID:")));
  ASSERT_TRUE(str_contains(mime, str_lit("MIME-Version: 1.0")));
}

TEST(the_body_is_base64_so_a_dot_or_a_long_line_cannot_break_it) {
  WMail m = mail_new("ada@x.test", "dots");
  Buf body;
  buf_init(&body, wc_arena_get(), 128);
  buf_puts(&body, "\n.\n....\n");
  for (i32 i = 0; i < 30; i++) buf_puts(&body, "a long line of text that would have to be folded\n");
  m.text = body;
  str mime = mail_mime(&m);
  ASSERT_TRUE(str_contains(mime, str_lit("base64")));
  ASSERT_FALSE(str_contains(mime, str_lit("\n.\n"))); // the dot line became encoded bytes
}

TEST(the_outbox_is_the_development_answer) {
  char dir[256];
  snprintf(dir, sizeof dir, "/tmp/webc-mail-test-%d", (int)time_ms());
  setenv("WEBC_MAIL_OUTBOX", dir, 1);
  WMail m = mail_new("ada@x.test", "Welcome");
  buf_puts(&m.html, "<p>one</p>");
  wc_mail_sync_text(&m);
  str err = {0};
  ASSERT_TRUE(mail_send(&m, &err));
  ASSERT_TRUE(mail_outbox_dir()[0] != 0);
  unsetenv("WEBC_MAIL_OUTBOX");
}

TEST(nothing_configured_says_so_instead_of_pretending) {
  unsetenv("WEBC_MAIL_OUTBOX");
  mail_config(&(WMailCfg){0});
  WMail m = mail_new("ada@x.test", "X");
  buf_puts(&m.text, "x");
  str err = {0};
  ASSERT_FALSE(mail_send(&m, &err));
  ASSERT_TRUE(str_contains(err, str_lit("no mail host")));
}

TEST_MAIN()
