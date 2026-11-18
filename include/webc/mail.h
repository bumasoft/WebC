// SPDX-License-Identifier: Apache-2.0
// webc/mail.h — a real email, sent by the app.
//
// Password resets, receipts, digests. Every app needs one email, and every app writes the same
// forty lines of SMTP twice: once to send, once to test. This is SMTP because SMTP is a
// line-based protocol the size of a database driver, and because the alternative (an HTTP
// provider's SDK) is a dependency and a bill for a feature that is four verbs.
//
//   mail_config(&(WMailCfg){
//       .host = "smtp.example.com", .port = 587, .starttls = true,
//       .user = getenv("SMTP_USER"), .pass = getenv("SMTP_PASS"),
//       .from = "Meridian <hello@meridian.test>",
//   });
//
//   WMail m = mail_new(user->email, "Your receipt");
//   mail_html(&m, Receipt, &ctx);       // the view you already wrote
//   mail_text(&m, Receipt, &ctx);       // the same view, stripped to text
//   str err;
//   if (!mail_send(&m, &err)) log_warn("mail: %s", err.p);
//
// **One view renders both parts.** `mail_html` renders the view into a buffer; `mail_text` walks
// that same buffer and produces the plain-text alternative, so a `multipart/alternative` cannot
// diverge and a test can assert on the text a reader sees without a mailbox. That is the whole
// reason this module exists rather than an app calling `sendmail`.
//
// **Development sends nothing.** With `WEBC_MAIL_OUTBOX=<dir>` set (which `webc dev` sets), every
// message is written to `<dir>/<timestamp>-<to>.eml` and the send reports success: a reset link
// is a file you can open, and no test ever has to reach a relay.
//
// **Refuses:** templating (a view is a view), DKIM, bounce processing, a queue (that is
// `jobs.h` — `send_receipt_do(&m)`), attachments beyond one inline file, and HTML/CSS inlining
// (an email that needs it needs a template, not a framework).
#pragma once

#include <webc/core.h>
#include <webc/dom.h>
#include <webc/http.h>
#include <webc/task.h>
#include <webc/time.h>
#include <webc/tls.h>

// ---------------------------------------------------------------- config

typedef struct WMailCfg {
  const char *host;     // "smtp.example.com"; empty = don't send, only report why
  u16 port;             // 0 = 587 with starttls, 465 implicit TLS, 25 plain
  const char *user;     // "" = no AUTH (an internal relay)
  const char *pass;
  const char *from;     // "Name <box@host>" — the envelope and the header
  bool starttls;        // upgrade on 587 (true unless port is 465/25)
  bool tls;             // the connection is TLS from the first byte (port 465)
  bool insecure;        // accept any certificate (a lab relay; never production)
  i64 timeout_ms;       // 0 = 10 s
} WMailCfg;

void mail_config(const WMailCfg *cfg);
bool mail_configured(void);
// Read `WEBC_MAIL_OUTBOX`, `WEBC_SMTP_HOST/PORT/USER/PASS/FROM` into the config — what `webc dev`
// and a twelve-factor deploy both want. Returns false when no host is set.
bool mail_config_env(void);

// ---------------------------------------------------------------- the message

typedef struct WMail {
  str to;
  str subject;
  Buf html;
  Buf text;
  str reply_to;
  str extra_headers; // "X-Tenant: 7\r\n" for the odd app that needs one
} WMail;

WMail wc_mail_new(str to, str subject);
#define mail_new(to_, subject_) wc_mail_new(WC_AS_STR(to_), WC_AS_STR(subject_))

// Render a view into the HTML part. Anything the view shape does — `el()`, loops of models —
// works, because it is the same builder a page uses. The view's *name* is the argument, so a
// misspelt one is a compile error instead of an empty email.
typedef void (*WMailViewFn)(WBuild *, void *);
void wc_mail_view(WMail *m, WMailViewFn fn, void *ctx);
#define mail_view(m_, VIEW_, ctx_) wc_mail_view((m_), (WMailViewFn)WC_CAT(view_, VIEW_), (void *)(ctx_))

// The text part *from the HTML part*: paragraphs, `<br>`, list items and links become lines, tags
// disappear, entities unescape, and inline `style`/`<script>` never appear. Call after the HTML.
str wc_mail_html_to_text(Arena *a, str html);
void wc_mail_sync_text(WMail *m);
#define mail_html_to_text(html_) wc_mail_html_to_text(wc_arena_get(), WC_AS_STR(html_))

// ---------------------------------------------------------------- sending

// The MIME message as bytes: what `mail_send` puts on the wire and what a test asserts on.
str mail_mime(const WMail *m);
// One SMTP conversation. False and a sentence in `err` on any refusal at any step.
bool mail_send(const WMail *m, str *err);
// Write it to the outbox directory instead of sending (what WEBC_MAIL_OUTBOX does).
str mail_outbox_write(const WMail *m); // the path written, "" on failure
// Where the dev outbox is: "" when it is not enabled.
const char *mail_outbox_dir(void);