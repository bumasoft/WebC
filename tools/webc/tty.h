// WebC — the one place a tool's words reach the terminal.
//
// Two rules, both learned the hard way:
//
//   1. Colour is for a person at a keyboard, never for a pipe. Every other `printf`
//      in the CLI writes an escape sequence straight into whatever is on the other
//      end, which is fine for a terminal and wrong for a log, a `grep`, a CI job or
//      an agent reading the output. So the writes go through here, and here decides.
//      `NO_COLOR` (any value, as the convention says), `TERM=dumb` and a
//      non-terminal stdout all mean plain text; `WEBC_COLOR=1` forces it back on for
//      the one case that matters (a test that asserts the escape sequences exist).
//
//   2. Some output is worth keeping instead of printing — a failed build that is
//      about to be retried, a step of `webc check --json`. `wcap_begin`/`wcap_end`
//      redirect both streams at the file-descriptor level, so a *child* process's
//      bytes are caught too, and hand them back as a string. It is the same trick a
//      shell does with `2>&1`, done without a subprocess.
//
// Nothing here allocates except the capture, which is malloc'd and freed by the
// caller, because it is the one thing whose size is not known in advance.
#ifndef WEBC_TOOL_TTY_H
#define WEBC_TOOL_TTY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

// Called once, first thing in main(). Idempotent.
void tty_init(void);

// Whether escapes are welcome on stdout.
bool tty_color(void);

// printf/fprintf/puts, with the escapes dropped when they are not welcome. The
// format attribute keeps clang's -Wformat checking working through the wrapper.
int wc_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int wc_fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int wc_puts(const char *s);

// The escape sequences the CLI uses, as strings that are empty when colour is off.
// A caller writes `printf("%s%s%s\n", g_b, "the developer surface leaks", g_x)` —
// printf argument order stays printf argument order, so -Wformat still counts.
extern const char *g_b, *g_d, *g_g, *g_r, *g_y, *g_x;

// A capture: redirect fd 1 and fd 2 into a temporary file, then read it back.
typedef struct WCap {
  int saved1, saved2, fd;
  bool on;
} WCap;

void wcap_begin(WCap *c);
// Restore the streams and return what was written (malloc'd, NUL-terminated, never
// null). An empty capture is "" and must still be freed.
char *wcap_end(WCap *c);

// The same string with every ANSI escape removed. Used on captured child output,
// which does not go through wc_printf: clang colourises when *it* thinks it is on a
// terminal, and a build may run with colour on.
char *wc_strip_ansi(const char *s);

#endif // WEBC_TOOL_TTY_H