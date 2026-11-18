// SPDX-License-Identifier: Apache-2.0
// webc/mail.h — MIME assembly, the HTML→text pass, and one SMTP conversation.
//
// The conversation is deliberately literal: read the greeting, EHLO, STARTTLS, AUTH, MAIL FROM,
// RCPT TO, DATA, quit. Every step checks its code, because a relay that says 550 after DATA is
// the case an app must hear about, and "the send returned true" is the bug this module exists
// to make impossible.
// System headers first: the shelf's `select()`/`state()` are macros, and a system header included
// after them is a header in the wrong language (docs/reference.md).
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <webc/mail.h>
#include <webc/model.h>

// Two disk primitives, ten lines, written here rather than imported: a shelf module is a header
// and its own .c, and one that drags another module in to save fifteen lines is a module an app
// pays for twice.
static bool mail_dir_ok(const char *dir) {
  if (!dir || !*dir) return false;
  struct stat st;
  if (stat(dir, &st) == 0) return S_ISDIR(st.st_mode);
  char path[4096];
  size_t n = strlen(dir);
  if (n >= sizeof path) return false;
  memcpy(path, dir, n + 1);
  for (size_t i = 1; i <= n; i++) {
    if (path[i] != '/' && path[i] != 0) continue;
    char keep = path[i];
    path[i] = 0;
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
      path[i] = keep;
      return false;
    }
    path[i] = keep;
  }
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool mail_write_file(const char *path, str data) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  size_t off = 0;
  while (off < data.n) {
    ssize_t w = write(fd, data.p + off, data.n - off);
    if (w <= 0) {
      close(fd);
      return false;
    }
    off += (size_t)w;
  }
  close(fd);
  return true;
}

static WMailCfg g_mail;

void mail_config(const WMailCfg *cfg) {
  g_mail = cfg ? *cfg : (WMailCfg){0};
  if (!g_mail.port) g_mail.port = g_mail.tls ? 465 : (g_mail.starttls ? 587 : 25);
  if (!g_mail.timeout_ms) g_mail.timeout_ms = 10000;
}

bool mail_configured(void) { return g_mail.host && *g_mail.host; }

const char *mail_outbox_dir(void) {
  const char *d = getenv("WEBC_MAIL_OUTBOX");
  return d && *d ? d : "";
}

bool mail_config_env(void) {
  WMailCfg c = {0};
  c.host = getenv("WEBC_SMTP_HOST");
  const char *port = getenv("WEBC_SMTP_PORT");
  c.port = (u16)(port ? atoi(port) : 0);
  c.user = getenv("WEBC_SMTP_USER");
  c.pass = getenv("WEBC_SMTP_PASS");
  c.from = getenv("WEBC_SMTP_FROM");
  c.tls = c.port == 465;
  c.starttls = c.port == 0 || c.port == 587;
  mail_config(&c);
  return mail_configured();
}

// ---------------------------------------------------------------- the message

WMail wc_mail_new(str to, str subject) {
  WMail m = {0};
  m.to = to;
  m.subject = subject;
  buf_init(&m.html, wc_arena_get(), 1024);
  buf_init(&m.text, wc_arena_get(), 512);
  return m;
}

// A mail's HTML part is a view rendered into a buffer: the same builder, the same `el()` and
// `text()`, so an app writes one view and the page and the email cannot disagree.
void wc_mail_view(WMail *m, WMailViewFn fn, void *ctx) {
  if (!m || !fn) return;
  Arena *a = m->html.a ? m->html.a : wc_arena_get();
  // A mail's view is a render position like a mounted one: one render, its own copy of any
  // `state_own` it declares, discarded after. Without this an email and a page could not
  // share a view that carries per-row state — the mail half would panic where the page works.
  WInst *own = winst_alloc();
  WBuild *b = wbuild_begin(a, nullptr);
  winst_own_push(own);
  fn(b, ctx);
  winst_own_pop();
  str html = webc_render(b);
  wbuild_end(b);
  winst_release(own);
  buf_init(&m->html, a, html.n + 64);
  buf_putstr(&m->html, html);
}

// ---------------------------------------------------------------- HTML to text
//
// The rule is the one a person would apply reading the page aloud: block elements end a line, a
// link keeps its label and puts the address in parentheses (the one thing a text part must not
// lose), a list item gets a dash, and anything that is not text — a script, a style — is dropped
// rather than printed.
str wc_mail_html_to_text(Arena *a, str html) {
  Buf out;
  buf_init(&out, a, html.n ? html.n / 2 : 64);
  static const char *block[] = {"p", "div", "br", "tr", "h1", "h2", "h3", "h4", "h5", "h6",
                                "table", "ul", "ol", "blockquote"};
  static const char *skip[] = {"script", "style", "head", "title"};
  str open_href = {0}; // the address of the <a> whose closing tag we are waiting for
  for (size_t i = 0; i < html.n;) {
    if (html.p[i] != '<') {
      if (html.p[i] == '&') {
        size_t j = i + 1;
        while (j < html.n && html.p[j] != ';' && j - i < 10) j++;
        str ent = {html.p + i + 1, j < html.n ? j - i - 1 : 0};
        char ch = 0;
        if (ent.n == 3 && memcmp(ent.p, "amp", 3) == 0) ch = '&';
        else if (ent.n == 2 && memcmp(ent.p, "lt", 2) == 0) ch = '<';
        else if (ent.n == 2 && memcmp(ent.p, "gt", 2) == 0) ch = '>';
        else if (ent.n == 4 && memcmp(ent.p, "quot", 4) == 0) ch = '"';
        else if (ent.n == 4 && memcmp(ent.p, "apos", 4) == 0) ch = '\'';
        else if (ent.n == 4 && memcmp(ent.p, "nbsp", 4) == 0) ch = ' ';
        if (ch && j < html.n) {
          buf_putc(&out, ch);
          i = j + 1;
          continue;
        }
      }
      buf_putc(&out, html.p[i++]);
      continue;
    }
    bool closing = i + 1 < html.n && html.p[i + 1] == '/';
    size_t j = i + 1 + (closing ? 1 : 0);
    size_t start = j;
    while (j < html.n && html.p[j] != '>' && html.p[j] != ' ') j++;
    str tag = {html.p + start, j - start};
    size_t tag_end = j;
    while (tag_end < html.n && html.p[tag_end] != '>') tag_end++;
    // a script or a style is not text: skip to its closing tag rather than printing its body
    for (size_t s2 = 0; s2 < sizeof skip / sizeof skip[0]; s2++) {
      if (!closing && tag.n == strlen(skip[s2]) && memcmp(tag.p, skip[s2], tag.n) == 0) {
        char close[32];
        int cl = snprintf(close, sizeof close, "</%s>", skip[s2]);
        size_t k = tag_end;
        while (k + (size_t)cl <= html.n) {
          if (memcmp(html.p + k, close, (size_t)cl) == 0) {
            k += (size_t)cl;
            break;
          }
          k++;
        }
        i = k;
        goto next_tag;
      }
    }
    if (!closing && tag.n == 1 && tag.p[0] == 'a') {
      size_t k = start; // from the tag's name, so the attribute really is inside the tag
      while (k < html.n) {
        if ((html.p[k] == 'h' || html.p[k] == 'H') && k + 5 < html.n &&
            strncmp(html.p + k, "href=", 5) == 0) {
          char q = html.p[k + 5];
          if (q == '"' || q == '\'') {
            size_t s2 = k + 6, e2 = s2;
            while (e2 < html.n && html.p[e2] != q) e2++;
            open_href = (str){html.p + s2, e2 - s2};
          }
          break;
        }
        k++;
      }
    } else if (closing && tag.n == 1 && tag.p[0] == 'a') {
      if (open_href.n) {
        buf_puts(&out, " (");
        buf_putstr(&out, open_href);
        buf_putc(&out, ')');
        open_href = (str){0};
      }
    } else if (tag.n == 2 && tag.p[0] == 'l' && tag.p[1] == 'i') {
      if (!closing) {
        if (out.n && out.p[out.n - 1] != '\n') buf_putc(&out, '\n');
        buf_puts(&out, "- ");
      }
    } else {
      for (size_t s2 = 0; s2 < sizeof block / sizeof block[0]; s2++) {
        if (tag.n != strlen(block[s2]) || memcmp(tag.p, block[s2], tag.n) != 0) continue;
        if (out.n && out.p[out.n - 1] != '\n') buf_putc(&out, '\n');
        // a paragraph ends with a blank line — but only when there is something to end
        if (out.n && tag.n == 1 && tag.p[0] == 'p') buf_putc(&out, '\n');
        break;
      }
    }
    i = tag_end + 1;
  next_tag:;
  }
  // collapse the blank-line runs the block rule leaves behind
  Buf tidy;
  buf_init(&tidy, a, out.n + 1);
  i32 nl = 0;
  for (size_t i = 0; i < out.n; i++) {
    char c = out.p[i];
    if (c == ' ' && nl > 0 && i + 1 < out.n && out.p[i + 1] == '\n') continue;
    if (c == '\n') {
      if (++nl > 2) continue;
    } else {
      nl = 0;
    }
    buf_putc(&tidy, c);
  }
  while (tidy.n && (tidy.p[tidy.n - 1] == '\n' || tidy.p[tidy.n - 1] == ' ')) tidy.n--;
  return buf_str(&tidy);
}

void wc_mail_sync_text(WMail *m) {
  if (!m || m->text.n || !m->html.n) return;
  m->text = (Buf){0};
  str t = wc_mail_html_to_text(m->html.a ? m->html.a : wc_arena_get(), buf_str(&m->html));
  buf_init(&m->text, m->html.a ? m->html.a : wc_arena_get(), t.n + 1);
  buf_putstr(&m->text, t);
}

// ---------------------------------------------------------------- MIME
//
// Two parts when there is a text part and an HTML part, one otherwise — a `multipart/alternative`
// with a single child is a mail client's bug report waiting to be filed. Both bodies are
// base64: it is the encoding that cannot be broken by an over-long line or a stray dot, and the
// one every client reads.
static void mime_b64(Arena *a, Buf *out, str body) {
  str enc = {0};
  if (!base64_encode(a, body, &enc)) return;
  // 76-character lines, folded at the encoding boundary (not mid-token) as RFC 2045 asks
  for (size_t i = 0; i < enc.n; i += 76) {
    size_t n = enc.n - i < 76 ? enc.n - i : 76;
    buf_write(out, enc.p + i, n);
    buf_puts(out, "\r\n");
  }
}

// A header value that may not contain CR/LF: a subject with a newline in it is a header
// injection, and a bounce form is user input.
static void mime_header_safe(Buf *out, str v) {
  for (size_t i = 0; i < v.n; i++) {
    char c = v.p[i];
    // a folded newline in a header is a second header: it becomes a space, never a line break
    buf_putc(out, (c == '\r' || c == '\n') ? ' ' : c);
  }
}

str mail_mime(const WMail *m) {
  Arena *a = wc_arena_get();
  Buf b;
  buf_init(&b, a, 1024 + m->html.n + m->text.n);
  bool both = m->html.n > 0 && m->text.n > 0;
  str boundary = str_lit("webc-mail-boundary-7f3a");
  buf_puts(&b, "From: ");
  mime_header_safe(&b, str_lit(g_mail.from ? g_mail.from : "nobody@localhost"));
  buf_puts(&b, "\r\nTo: ");
  mime_header_safe(&b, m->to);
  buf_puts(&b, "\r\nSubject: ");
  mime_header_safe(&b, m->subject);
  buf_puts(&b, "\r\nDate: ");
  str stamp = fmt_date(time_ms(), "%a, %d %b %Y %H:%M:%S +0000");
  buf_putstr(&b, stamp);
  if (m->reply_to.n) {
    buf_puts(&b, "\r\nReply-To: ");
    mime_header_safe(&b, m->reply_to);
  }
  if (m->extra_headers.n) {
    buf_puts(&b, "\r\n");
    mime_header_safe(&b, m->extra_headers);
  }
  str id_host = str_lit("webc.local");
  buf_printf(&b, "\r\nMessage-ID: <%llx.%llx@%s>", (unsigned long long)time_ms(),
             (unsigned long long)str_hash(m->subject.p ? m->subject : str_lit("")),
             id_host.p);
  buf_puts(&b, "\r\nMIME-Version: 1.0\r\n");
  if (both) {
    buf_printf(&b, "Content-Type: multipart/alternative; boundary=\"%s\"\r\n\r\n", boundary.p);
    buf_printf(&b, "--%s\r\nContent-Type: text/plain; charset=utf-8\r\n"
                   "Content-Transfer-Encoding: base64\r\n\r\n", boundary.p);
    mime_b64(a, &b, buf_str((Buf *)&m->text));
    buf_printf(&b, "--%s\r\nContent-Type: text/html; charset=utf-8\r\n"
                   "Content-Transfer-Encoding: base64\r\n\r\n", boundary.p);
    mime_b64(a, &b, buf_str((Buf *)&m->html));
    buf_printf(&b, "--%s--\r\n", boundary.p);
  } else if (m->html.n) {
    buf_puts(&b, "Content-Type: text/html; charset=utf-8\r\n"
                 "Content-Transfer-Encoding: base64\r\n\r\n");
    mime_b64(a, &b, buf_str((Buf *)&m->html));
  } else {
    buf_puts(&b, "Content-Type: text/plain; charset=utf-8\r\n"
                 "Content-Transfer-Encoding: base64\r\n\r\n");
    mime_b64(a, &b, buf_str((Buf *)&m->text));
  }
  return buf_str(&b);
}

// ---------------------------------------------------------------- the outbox
//
// `WEBC_MAIL_OUTBOX` is the development answer: a reset link becomes a file, and the app's own
// test can read it. The name carries the recipient and the clock, so ten sends are ten files.
static void outbox_filename(Buf *out, const WMail *m) {
  buf_puts(out, mail_outbox_dir());
  buf_putc(out, '/');
  buf_printf(out, "%lld-", (long long)time_ms());
  for (size_t i = 0; i < m->to.n && i < 48; i++) {
    char c = m->to.p[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '@' ||
              c == '.' || c == '-' || c == '_';
    buf_putc(out, ok ? c : '_');
  }
  buf_puts(out, ".eml");
}

str mail_outbox_write(const WMail *m) {
  const char *dir = mail_outbox_dir();
  if (!dir || !*dir || !m) return (str){0};
  Arena *a = wc_arena_get();
  // a directory that is not there is a first run, not an error
  char cmd[4200];
  snprintf(cmd, sizeof cmd, "%s", dir);
  if (!mail_dir_ok(cmd)) return (str){0};
  Buf path;
  buf_init(&path, a, strlen(dir) + 80);
  outbox_filename(&path, m);
  if (!mail_write_file(buf_cstr(&path), mail_mime(m))) return (str){0};
  return buf_str(&path);
}

// ---------------------------------------------------------------- SMTP
//
// One connection, one message, and a code check at every step. The wire is 7-bit ASCII apart from
// the base64 bodies, so a plain Buf is the right buffer and no encoding layer is needed.
typedef struct MailWire {
  i32 fd;
  void *tls;
  Buf in;
  bool ok;
  char why[160];
} MailWire;

static i64 wire_read(MailWire *w, char *buf, size_t n) {
  if (w->tls) return tls_read(w->tls, buf, n);
  return wfd_read(w->fd, buf, n);
}

static bool wire_write(MailWire *w, str s) {
  size_t off = 0;
  while (off < s.n) {
    i64 n = w->tls ? tls_write(w->tls, s.p + off, s.n - off) : wfd_write(w->fd, s.p + off, s.n - off);
    if (n <= 0) return false;
    off += (size_t)n;
  }
  return true;
}

static bool wire_line(MailWire *w, const char *fmt, ...) {
  Buf b;
  buf_init(&b, wc_arena_get(), 128);
  va_list ap;
  va_start(ap, fmt);
  buf_vprintf(&b, fmt, ap);
  va_end(ap);
  buf_puts(&b, "\r\n");
  return wire_write(w, buf_str(&b));
}

// Read one reply: `250-first` lines until `250 last`. The *code* is what the caller checks,
// because `250` and `550` are the difference between a message sent and a message lost.
static i32 wire_reply(MailWire *w) {
  char buf[2048];
  i32 code = 0;
  for (;;) {
    // one line at a time, buffering whatever arrives
    size_t start = w->in.n;
    for (;;) {
      bool have_nl = false;
      for (size_t i = start; i < w->in.n; i++)
        if (w->in.p[i] == '\n') {
          have_nl = true;
          break;
        }
      if (have_nl) break;
      i64 n = wire_read(w, buf, sizeof buf);
      if (n <= 0) return -1;
      buf_write(&w->in, buf, (size_t)n);
    }
    // take the first complete line
    size_t nl = 0;
    while (nl < w->in.n && w->in.p[nl] != '\n') nl++;
    str line = {w->in.p, nl};
    if (line.n >= 3 && line.p[0] >= '0' && line.p[0] <= '9') code = (line.p[0] - '0') * 100 + (line.p[1] - '0') * 10 + (line.p[2] - '0');
    bool last = !(line.n >= 4 && line.p[3] == '-');
    // drop the consumed line, keep the rest of the buffer
    size_t keep = nl + 1;
    if (keep >= w->in.n) {
      w->in.n = 0;
    } else {
      memmove(w->in.p, w->in.p + keep, w->in.n - keep);
      w->in.n -= keep;
    }
    if (last) return code;
  }
}

static void wire_fail(MailWire *w, const char *why, i32 code) {
  if (!w->ok) return;
  w->ok = false;
  snprintf(w->why, sizeof w->why, "%s (the relay said %d)", why, (int)code);
}

// AUTH PLAIN with the NUL-separated triple, base64 — accepted by every relay that offers AUTH at
// all, and one round trip instead of LOGIN's two.
static bool wire_auth(MailWire *w) {
  if (!g_mail.user || !*g_mail.user) return true;
  Arena *a = wc_arena_get();
  Buf raw;
  buf_init(&raw, a, 128);
  buf_putc(&raw, 0);
  buf_puts(&raw, g_mail.user);
  buf_putc(&raw, 0);
  buf_puts(&raw, g_mail.pass ? g_mail.pass : "");
  str enc = {0};
  if (!base64_encode(a, buf_str(&raw), &enc)) return false;
  wire_line(w, "AUTH PLAIN %.*s", (int)enc.n, enc.p);
  i32 code = wire_reply(w);
  if (code != 235) {
    wire_fail(w, "the relay refused the credentials", code);
    return false;
  }
  return true;
}

bool mail_send(const WMail *m, str *err) {
  if (err) *err = (str){0};
  if (!m) return false;
  // The outbox first: a development machine must never reach a relay, and `webc dev` sets it.
  if (*mail_outbox_dir()) {
    str path = mail_outbox_write(m);
    if (!path.n) {
      if (err) *err = str_lit("the outbox directory could not be written");
      return false;
    }
    return true;
  }
  if (!mail_configured()) {
    if (err) *err = str_lit("no mail host is configured (WEBC_SMTP_HOST or mail_config())");
    return false;
  }
  MailWire w = {0};
  w.ok = true;
  buf_init(&w.in, wc_arena_get(), 512);
  w.fd = wfd_open_connect(g_mail.host, g_mail.port, g_mail.timeout_ms);
  if (w.fd < 0) {
    if (err) {
      Buf why;
      buf_init(&why, wc_arena_get(), 96);
      buf_printf(&why, "cannot reach %s:%u", g_mail.host, (unsigned)g_mail.port);
      *err = buf_str(&why);
    }
    return false;
  }
  wfd_set_deadline(w.fd, time_ms() + g_mail.timeout_ms);
  i32 code = wire_reply(&w);
  if (code != 220) {
    wire_fail(&w, "the relay did not greet us", code);
    goto done;
  }
  // EHLO carries the capability list, which is how STARTTLS is discovered rather than assumed
  wire_line(&w, "EHLO %s", "localhost");
  code = wire_reply(&w);
  if (code != 250) {
    wire_fail(&w, "EHLO was refused", code);
    goto done;
  }
  if (g_mail.starttls && !g_mail.tls) {
    if (!tls_available()) {
      snprintf(w.why, sizeof w.why, "STARTTLS was asked for but this build has no TLS (see webc/tls.h)");
      w.ok = false;
      goto done;
    }
    wire_line(&w, "STARTTLS");
    code = wire_reply(&w);
    if (code != 220) {
      wire_fail(&w, "the relay refused STARTTLS", code);
      goto done;
    }
    char tls_err[128] = {0};
    w.tls = tls_connect(w.fd, g_mail.host, nullptr, g_mail.insecure, tls_err, sizeof tls_err);
    if (!w.tls) {
      snprintf(w.why, sizeof w.why, "TLS handshake failed: %s", tls_err);
      w.ok = false;
      goto done;
    }
    w.in.n = 0; // anything buffered before the upgrade belongs to the plaintext stream
    if (!wire_auth(&w)) goto done;
  } else if (g_mail.tls) {
    char tls_err[128] = {0};
    w.tls = tls_connect(w.fd, g_mail.host, nullptr, g_mail.insecure, tls_err, sizeof tls_err);
    if (!w.tls) {
      snprintf(w.why, sizeof w.why, "TLS handshake failed: %s", tls_err);
      w.ok = false;
      goto done;
    }
    w.in.n = 0;
  }
  if (!wire_auth(&w)) goto done;
  wire_line(&w, "MAIL FROM:<%s>", g_mail.from ? g_mail.from : "nobody@localhost");
  code = wire_reply(&w);
  if (code != 250) {
    wire_fail(&w, "MAIL FROM was refused", code);
    goto done;
  }
  wire_line(&w, "RCPT TO:<%.*s>", (int)m->to.n, m->to.p);
  code = wire_reply(&w);
  if (code != 250 && code != 251) {
    wire_fail(&w, "the recipient was refused", code);
    goto done;
  }
  wire_line(&w, "DATA");
  code = wire_reply(&w);
  if (code != 354) {
    wire_fail(&w, "the relay would not take the message", code);
    goto done;
  }
  {
    // dot-stuffing: a line that begins with '.' would end the message early
    Buf body;
    buf_init(&body, wc_arena_get(), 2048);
    str mime = mail_mime(m);
    for (size_t i = 0; i < mime.n; i++) {
      if (i == 0 || (i > 0 && mime.p[i - 1] == '\n')) {
        if (mime.p[i] == '.') buf_putc(&body, '.');
      }
      buf_putc(&body, mime.p[i]);
    }
    if (body.n < 2 || body.p[body.n - 1] != '\n') buf_puts(&body, "\r\n");
    buf_puts(&body, ".\r\n");
    if (!wire_write(&w, buf_str(&body))) {
      snprintf(w.why, sizeof w.why, "the connection dropped mid-message");
      w.ok = false;
      goto done;
    }
  }
  code = wire_reply(&w);
  if (code != 250) {
    wire_fail(&w, "the relay did not accept the message", code);
    goto done;
  }
  wire_line(&w, "QUIT");
  wire_reply(&w);

done:
  if (w.tls) tls_close(w.tls);
  wfd_close(w.fd);
  if (!w.ok && err) *err = str_lit(w.why);
  return w.ok;
}
