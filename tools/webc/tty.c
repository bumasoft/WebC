// SPDX-License-Identifier: Apache-2.0
// WebC — see tty.h.
#include "tty.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *g_b, *g_d, *g_g, *g_r, *g_y, *g_x;

static bool g_color;
static bool g_ready;

void tty_init(void) {
  if (g_ready) return;
  g_ready = true;
  // An empty NO_COLOR still counts: no-color.org says "when present and not an
  // empty string", and honouring the stricter reading costs nothing and surprises
  // nobody. WEBC_COLOR overrides in both directions, for the tests and for someone
  // whose terminal is better than isatty() thinks.
  const char *force = getenv("WEBC_COLOR");
  if (force && *force) {
    g_color = force[0] != '0';
  } else if (getenv("NO_COLOR")) {
    g_color = false;
  } else {
    const char *term = getenv("TERM");
    g_color = isatty(STDOUT_FILENO) && !(term && strcmp(term, "dumb") == 0);
  }
  g_b = g_color ? "\033[1m" : "";
  g_d = g_color ? "\033[36m" : ""; // the tool's own voice: `webc check`
  g_g = g_color ? "\033[32m" : "";
  g_r = g_color ? "\033[31m" : "";
  g_y = g_color ? "\033[33m" : "";
  g_x = g_color ? "\033[0m" : "";
}

bool tty_color(void) {
  tty_init();
  return g_color;
}

// ---------------------------------------------------------------- stripping

static size_t strip_into(char *dst, const char *s) {
  size_t n = 0;
  for (const char *p = s; *p;) {
    if ((unsigned char)p[0] == 0x1b && p[1] == '[') {
      p += 2;
      while (*p && !(*p >= '@' && *p <= '~')) p++;
      if (*p) p++;
      continue;
    }
    dst[n++] = *p++;
  }
  dst[n] = 0;
  return n;
}

char *wc_strip_ansi(const char *s) {
  size_t cap = strlen(s) + 1;
  char *out = (char *)malloc(cap);
  if (!out) return nullptr;
  strip_into(out, s);
  return out;
}

// ---------------------------------------------------------------- writing

static int write_stripped(FILE *f, const char *fmt, va_list ap) {
  char stack[4096];
  va_list copy;
  va_copy(copy, ap);
  int n = vsnprintf(stack, sizeof stack, fmt, copy);
  va_end(copy);
  if (n < 0) return n;
  char *heap = nullptr;
  const char *src = stack;
  if ((size_t)n >= sizeof stack) {
    heap = (char *)malloc((size_t)n + 1);
    if (!heap) return -1;
    vsnprintf(heap, (size_t)n + 1, fmt, ap);
    src = heap;
  }
  int rc;
  if (g_color) {
    rc = (int)fwrite(src, 1, (size_t)n, f);
  } else {
    char *plain = (char *)malloc((size_t)n + 1);
    if (!plain) {
      free(heap);
      return -1;
    }
    strip_into(plain, src);
    rc = (int)fwrite(plain, 1, strlen(plain), f);
    free(plain);
  }
  free(heap);
  return rc;
}

int wc_printf(const char *fmt, ...) {
  tty_init();
  va_list ap;
  va_start(ap, fmt);
  int rc = write_stripped(stdout, fmt, ap);
  va_end(ap);
  return rc;
}

int wc_fprintf(FILE *f, const char *fmt, ...) {
  tty_init();
  va_list ap;
  va_start(ap, fmt);
  int rc = write_stripped(f, fmt, ap);
  va_end(ap);
  return rc;
}

int wc_puts(const char *s) {
  tty_init();
  if (g_color) return fputs(s, stdout);
  char *plain = wc_strip_ansi(s);
  if (!plain) return -1;
  int rc = fputs(plain, stdout);
  free(plain);
  return rc;
}

// ---------------------------------------------------------------- capture

void wcap_begin(WCap *c) {
  memset(c, 0, sizeof *c);
  c->saved1 = c->saved2 = c->fd = -1;
  fflush(stdout);
  fflush(stderr);
  FILE *tmp = tmpfile();
  if (!tmp) return; // no capture: the output goes where it always went
  c->fd = fileno(tmp);
  // tmpfile() unlinks on close; we hold the fd and close it ourselves in wcap_end.
  c->saved1 = dup(STDOUT_FILENO);
  c->saved2 = dup(STDERR_FILENO);
  if (dup2(c->fd, STDOUT_FILENO) < 0 || dup2(c->fd, STDERR_FILENO) < 0) return;
  c->on = true;
}

char *wcap_end(WCap *c) {
  fflush(stdout);
  fflush(stderr);
  if (!c->on) {
    char *empty = (char *)malloc(1);
    if (empty) empty[0] = 0;
    return empty;
  }
  dup2(c->saved1, STDOUT_FILENO);
  dup2(c->saved2, STDERR_FILENO);
  close(c->saved1);
  close(c->saved2);
  off_t n = lseek(c->fd, 0, SEEK_END);
  if (n < 0) n = 0;
  lseek(c->fd, 0, SEEK_SET);
  char *text = (char *)malloc((size_t)n + 1);
  if (!text) {
    close(c->fd);
    c->on = false;
    return nullptr;
  }
  size_t got = 0;
  for (;;) {
    ssize_t k = read(c->fd, text + got, (size_t)n - got);
    if (k <= 0) break;
    got += (size_t)k;
    if (got >= (size_t)n) break;
  }
  text[got] = 0;
  close(c->fd);
  c->on = false;
  // clang colours when it believes it is on a terminal, and a build may run with
  // colour on: what comes back from a child is not this process's decision.
  char *plain = wc_strip_ansi(text);
  free(text);
  return plain;
}