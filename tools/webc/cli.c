// SPDX-License-Identifier: Apache-2.0
// WebC's CLI — the helpers its own translation units share (cli.h).
#include <webc/core.h>

#include "cli.h"

// A JSON string on a Buf, for the `--json` half of the CLI. Small enough to keep here:
// the alternative is a stream API in the framework's json.h that no app wants.
void jputs(Buf *b, const char *s) {
  if (!s) s = "";
  buf_putc(b, '"');
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    switch (*p) {
      case '"': buf_puts(b, "\\\""); break;
      case '\\': buf_puts(b, "\\\\"); break;
      case '\n': buf_puts(b, "\\n"); break;
      case '\r': buf_puts(b, "\\r"); break;
      case '\t': buf_puts(b, "\\t"); break;
      default:
        if (*p < 0x20) buf_printf(b, "\\u%04x", *p);
        else buf_putc(b, (char)*p);
    }
  }
  buf_putc(b, '"');
}
