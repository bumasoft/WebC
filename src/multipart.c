// SPDX-License-Identifier: Apache-2.0
// webc — multipart/form-data (SPEC §11: explicit limits, part by part).
//
// The parser is a scanner over the body, not a state machine with allocations per
// byte: a part's data is a (ptr,len) into the request body, so a 4 MB upload costs
// nothing beyond the body the server already read. What it does own is the decision
// of where a part ends, so it refuses early and says why.
#include <webc/multipart.h>

bool multipart_is(const char *content_type) {
  if (!content_type) return false;
  return strncasecmp(content_type, "multipart/form-data", 19) == 0;
}

// boundary=... out of a Content-Type, quotes optional (they are required in the
// grammar, but a real browser sends them and curl does not).
static str find_boundary(str ctype) {
  size_t n = strlen("boundary=");
  for (size_t i = 0; i + n < ctype.n; i++) {
    if (strncasecmp(ctype.p + i, "boundary=", n) != 0) continue;
    size_t j = i + n;
    str v = {ctype.p + j, 0};
    if (j < ctype.n && ctype.p[j] == '"') {
      size_t k = j + 1;
      while (k < ctype.n && ctype.p[k] != '"') k++;
      v.p = ctype.p + j + 1;
      v.n = k - (j + 1);
    } else {
      size_t k = j;
      while (k < ctype.n && ctype.p[k] != ';' && ctype.p[k] != ' ' && ctype.p[k] != ',') k++;
      v.n = k - j;
    }
    return v;
  }
  return (str){0};
}

// The value of a `key=value` pair inside a header, quotes stripped.
static str header_param(str header, const char *key) {
  size_t kn = strlen(key);
  for (size_t i = 0; i + kn + 1 <= header.n; i++) {
    if (strncasecmp(header.p + i, key, kn) != 0) continue;
    if (header.p[i + kn] != '=') continue;
    size_t j = i + kn + 1;
    if (j < header.n && header.p[j] == '"') {
      size_t k = j + 1;
      while (k < header.n && header.p[k] != '"') k++;
      return (str){header.p + j + 1, k - (j + 1)};
    }
    size_t k = j;
    while (k < header.n && header.p[k] != ';' && header.p[k] != ' ') k++;
    return (str){header.p + j, k - j};
  }
  return (str){0};
}

static const char *mem_find(const char *hay, size_t n, const char *needle, size_t nn) {
  if (nn == 0 || n < nn) return nullptr;
  for (size_t i = 0; i + nn <= n; i++)
    if (hay[i] == needle[0] && memcmp(hay + i, needle, nn) == 0) return hay + i;
  return nullptr;
}

WErrRes multipart_parse(Arena *a, str content_type, Bytes body, i32 max_parts, size_t max_bytes,
                        WMultipart *out) {
  *out = (WMultipart){0};
  str boundary = find_boundary(content_type);
  if (boundary.n == 0 || boundary.n > 200)
    return wfail(WEBC_EINVAL, str_lit("multipart body without a boundary"));
  if (body.n > max_bytes)
    return wfail(WEBC_ETOOBIG, str_lit("multipart body too large"));

  // "--" boundary, both dashes included
  char dash[256];
  if (boundary.n + 2 >= sizeof dash)
    return wfail(WEBC_EINVAL, str_lit("multipart boundary too long"));
  dash[0] = '-';
  dash[1] = '-';
  memcpy(dash + 2, boundary.p, boundary.n);
  size_t dn = boundary.n + 2;

  const char *p = mem_find(body.p, body.n, dash, dn);
  if (!p) return wfail(WEBC_EPARSE, str_lit("multipart body has no boundary marker"));
  out->parts = arena_alloc(a, (size_t)(max_parts > 0 ? max_parts : 32) * sizeof(WPart));
  out->cap = max_parts > 0 ? max_parts : 32;

  for (;;) {
    p += dn;
    if (p + 1 < body.p + body.n && p[0] == '-' && p[1] == '-') break; // closing marker
    // skip the CRLF after the boundary
    while (p < body.p + body.n && (*p == '\r' || *p == '\n')) p++;
    if (p >= body.p + body.n) break;
    const char *next = mem_find(p, (size_t)(body.p + body.n - p), dash, dn);
    if (!next) return wfail(WEBC_EPARSE, str_lit("multipart part is not terminated"));
    size_t seg = (size_t)(next - p);
    // a part is headers, a blank line, then the data
    const char *hdrs_end = mem_find(p, seg, "\r\n\r\n", 4);
    size_t hdr_len = 0, data_off = 0;
    if (hdrs_end) {
      hdr_len = (size_t)(hdrs_end - p);
      data_off = hdr_len + 4;
    } else if ((hdrs_end = mem_find(p, seg, "\n\n", 2))) {
      hdr_len = (size_t)(hdrs_end - p);
      data_off = hdr_len + 2;
    } else {
      return wfail(WEBC_EPARSE, str_lit("multipart part has no header terminator"));
    }
    size_t data_len = seg - data_off;
    // the CRLF before the next boundary belongs to the delimiter, not the data
    if (data_len >= 2 && next[-2] == '\r' && next[-1] == '\n') data_len -= 2;
    else if (data_len >= 1 && next[-1] == '\n') data_len -= 1;

    if (out->n >= out->cap)
      return wfail(WEBC_ETOOBIG, str_lit("too many multipart parts"));

    WPart part = {.data = {p + data_off, data_len}};
    str headers = {p, hdr_len};
    size_t hpos = 0;
    while (hpos < headers.n) {
      size_t eol = hpos;
      while (eol < headers.n && headers.p[eol] != '\n') eol++;
      str line = str_trim((str){headers.p + hpos, eol - hpos});
      hpos = eol + 1;
      if (line.n == 0) continue;
      str lower = line;
      if (lower.n > 19 && strncasecmp(lower.p, "content-disposition", 19) == 0) {
        part.name = header_param(line, "name");
        part.filename = header_param(line, "filename");
        if (part.filename.n) part.is_file = true;
      } else if (lower.n > 12 && strncasecmp(lower.p, "content-type", 12) == 0) {
        i64 colon = str_index(line, ':');
        if (colon > 0) part.content_type = str_trim(str_sub(line, (size_t)colon + 1, line.n));
      }
    }
    if (part.name.n == 0)
      return wfail(WEBC_EPARSE, str_lit("multipart part without a name"));
    out->parts[out->n++] = part;
    p = next;
  }
  return wok();
}

static const WPart *find_part(const WMultipart *mp, const char *name) {
  if (!mp || !name) return nullptr;
  size_t n = strlen(name);
  for (i32 i = 0; i < mp->n; i++)
    if (mp->parts[i].name.n == n && memcmp(mp->parts[i].name.p, name, n) == 0) return &mp->parts[i];
  return nullptr;
}

str multipart_value(const WMultipart *mp, const char *name) {
  const WPart *p = find_part(mp, name);
  return p && !p->is_file ? p->data : (str){0};
}

str multipart_file(const WMultipart *mp, const char *name) {
  const WPart *p = find_part(mp, name);
  return p && p->is_file ? p->data : (str){0};
}

str multipart_filename(const WMultipart *mp, const char *name) {
  const WPart *p = find_part(mp, name);
  return p ? p->filename : (str){0};
}

str multipart_file_type(const WMultipart *mp, const char *name) {
  const WPart *p = find_part(mp, name);
  return p ? p->content_type : (str){0};
}

i32 multipart_count(const WMultipart *mp) {
  return mp ? mp->n : 0;
}