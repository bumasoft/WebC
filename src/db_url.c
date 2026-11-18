// SPDX-License-Identifier: Apache-2.0
// WebC — one splitter for the drivers' connection strings.
//
// `postgres://user:pass@host:5432/db?sslmode=require` and
// `mysql://user:pass@host:3306/db?ssl-mode=require` are the same grammar with different
// words in the query string, and a second parser per driver is a second set of rules for
// where a name ends: an empty host, a missing port and a password containing '@' have to
// mean one thing across the layer, not three. Everything here is a slice of the URL the
// caller passed — nothing is copied, so an over-long field is the caller's to reject.
#include <string.h>

#include "db_internal.h"

void wc_db_url_split(const char *url, WDbUrl *u) {
  memset(u, 0, sizeof *u);
  if (!url) return;
  const char *end = url + strlen(url);
  // The scheme is a label, not a field: `postgres://` and `mysql://` differ nowhere the
  // parser cares about, and `postgresql://` is the same driver under another name.
  const char *p = strstr(url, "://");
  p = p ? p + 3 : url;
  const char *q = strchr(url, '?');
  if (q) {
    u->query = q[1] ? (str){q + 1, (size_t)(end - q - 1)} : (str){nullptr, 0};
    end = q; // the database name ends where the query begins
  }
  const char *slash = memchr(p, '/', (size_t)(end - p));
  const char *path = slash ? slash : end;
  // The last '@' before the path: a password may contain one unencoded, because a
  // generated password pasted into a dashboard panel is what a URL has to survive.
  const char *at = nullptr;
  for (const char *c = p; c < path; c++)
    if (*c == '@') at = c;
  if (at) {
    const char *colon = memchr(p, ':', (size_t)(at - p));
    if (colon) {
      u->user = (str){p, (size_t)(colon - p)};
      u->pass = (str){colon + 1, (size_t)(at - colon - 1)};
    } else {
      u->user = (str){p, (size_t)(at - p)};
    }
    p = at + 1;
  }
  // An empty host is legal ("user:pw@/db" over a unix socket): the driver's own default
  // stands in for it, and the socket parameter decides the transport.
  const char *colon = memchr(p, ':', (size_t)(path - p));
  if (colon) {
    u->host = (str){p, (size_t)(colon - p)};
    u->port = (str){colon + 1, (size_t)(path - colon - 1)};
  } else {
    u->host = (str){p, (size_t)(path - p)};
  }
  if (slash && slash + 1 < end) u->db = (str){slash + 1, (size_t)(end - slash - 1)};
}
