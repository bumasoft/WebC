// SPDX-License-Identifier: Apache-2.0
// webc/upload.h — a file from a form, checked against a policy before it touches the disk.
//
// The parser (`webc/multipart.h`) already refuses a body that lies about its size or its part
// count. What it cannot decide is the app's policy: which types are welcome, how big one file
// may be, and what the saved name is. That is this file, and the one thing it always does is
// **stop trusting the client's filename**: `../../.ssh/authorized_keys` is a real upload.
//
//   UploadPolicy pol = upload_policy(".data/files", 8 * 1024 * 1024);
//   pol.allow = (const char *[]){"image/png", "image/jpeg", "application/pdf"};
//   pol.allow_n = 3;
//
//   Upload up = upload_take(req, "cv", &pol);      // three arguments; `up.ok` says how it went
//   if (!up.ok) return res_error(422, "bad_upload", up.why);
//   text(t("thanks_upload", "name", up.name));    // "a1b2c3-cv.pdf"
//
// What `upload_take` guarantees when it returns true:
//   * the part exists, is a file, and is not empty;
//   * its size is within `max_bytes` and within the request's own multipart limit;
//   * its declared type is in the allow list (when one is given), and — because a
//     Content-Type is a client's opinion — its first bytes agree with that type for the
//     handful of formats whose magic number is unambiguous;
//   * the name it was saved under was built here: a random stem, an extension from the
//     allow list, never a byte of the client's path.
//
// **Refuses:** image resizing (that is a job for `cjpeg`/`vips`, not a web framework), virus
// scanning, S3 (an app that wants object storage has an app's code to say so), chunked resume,
// and pre-signed URLs. A file the app does not want is a 422 with a sentence a person can read.
#pragma once

#include <webc/core.h>
#include <webc/dom.h>
#include <webc/form.h>
#include <webc/http.h>
#include <webc/multipart.h>

typedef struct UploadPolicy {
  const char *dir;             // where to save; created if missing
  size_t max_bytes;           // 0 = the request's multipart limit
  const char **allow;          // MIME types like "image/png"; nullptr = anything
  i32 allow_n;
  const char **ext;            // forced extensions parallel to `allow` ("png"); optional
  i32 public_read;            // 0 = 0600, 1 = 0644 (a file served by this app)
} UploadPolicy;

typedef struct Upload {
  bool ok;
  const char *why;  // "" or a sentence for the person who uploaded it
  str name;         // the saved file name, "a1b2c3d4-cv.pdf"
  str path;         // dir + name
  str type;         // the type the magic bytes agreed on (or the declared one)
  size_t bytes;
  str original;     // the client's filename, kept only for display
} Upload;

// The two things this module does to the disk. Declared here, defined in `src/upload.c` so
// that <sys/stat.h> stays out of a header an app's client half may also include.
bool wc_upload_mkdir_p(const char *dir);
bool wc_upload_write(const char *path, str data, i32 mode);

static inline UploadPolicy wc_upload_policy(const char *dir, size_t max_bytes) {
  UploadPolicy p = {0};
  p.dir = dir;
  p.max_bytes = max_bytes;
  return p;
}
#define upload_policy(dir_, max_) wc_upload_policy((dir_), (max_))

// ---------------------------------------------------------------- the magic numbers
//
// Only the formats whose first bytes are a fact. A `.csv` has no magic number, and pretending
// otherwise would reject honest files; those fall through to the allow list alone.
static inline const char *wc_upload_sniff(str data) {
  const unsigned char *b = (const unsigned char *)data.p;
  size_t n = data.n;
  if (n >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return "image/png";
  if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return "image/jpeg";
  if (n >= 6 && b[0] == 'G' && b[1] == 'I' && b[2] == 'F' && b[3] == '8') return "image/gif";
  if (n >= 12 && b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F' && b[8] == 'W' &&
      b[9] == 'E' && b[10] == 'B' && b[11] == 'P')
    return "image/webp";
  if (n >= 5 && b[0] == '%' && b[1] == 'P' && b[2] == 'D' && b[3] == 'F') return "application/pdf";
  if (n >= 4 && b[0] == 'P' && b[1] == 'K' && (b[2] == 3 || b[2] == 5)) return "application/zip";
  if (n >= 4 && b[0] == 0x1A && b[1] == 0x45 && b[2] == 0xDF && b[3] == 0xA3) return "video/webm";
  if (n >= 12 && b[4] == 'f' && b[5] == 't' && b[6] == 'y' && b[7] == 'p') return "video/mp4";
  if (n >= 4 && ((b[0] == 'I' && b[1] == 'D' && b[2] == '3') || (b[0] == 0xFF && (b[1] & 0xE0) == 0xE0)))
    return "audio/mpeg";
  return nullptr;
}

#define upload_sniff(data_) wc_upload_sniff(WC_AS_STR(data_))

// Does `type` ("image/png; charset=…") match an allow entry, which may be "image/*"?
static inline bool wc_upload_type_matches(str type, const char *allow) {
  if (!type.p || !type.n) return false;
  size_t semi = 0;
  while (semi < type.n && type.p[semi] != ';' && type.p[semi] != ' ') semi++;
  str bare = {.p = type.p, .n = semi};
  if (strcmp(allow, "*") == 0 || strcmp(allow, "*/*") == 0) return true;
  if (strcmp(allow, bare.p ? allow : allow) == 0 && strlen(allow) == bare.n) return true;
  if (bare.n == strlen(allow) && memcmp(bare.p, allow, bare.n) == 0) return true;
  // "image/*" or "video/*": the prefix before the slash is the whole rule
  size_t slash = 0;
  while (allow[slash] && allow[slash] != '/') slash++;
  if (allow[slash] == '/' && allow[slash + 1] == '*' && allow[slash + 2] == 0 && slash < bare.n &&
      bare.p[slash] == '/' && memcmp(bare.p, allow, slash) == 0)
    return true;
  return false;
}

// The bytes are what they say they are — for the types that can be checked. A text type, a
// CSV, a `.docx` (a zip, already caught above) get no second opinion.
#define upload_bytes_agree(type_, data_) wc_upload_bytes_agree(WC_AS_STR(type_), WC_AS_STR(data_))
#define upload_type_matches(type_, allow_) wc_upload_type_matches(WC_AS_STR(type_), (allow_))

static inline bool wc_upload_bytes_agree(str declared, str data) {
  const char *sniffed = wc_upload_sniff(data);
  if (!sniffed) return true;
  return wc_upload_type_matches(declared, sniffed);
}

// ---------------------------------------------------------------- the name
//
// The client's filename is reduced to letters, digits, `.`, `-`, `_`, and its extension is
// taken from the allow list — never from the client. A random stem means two uploads of
// "cv.pdf" do not overwrite each other, and a name cannot be guessed by walking the directory.
static inline char wc_upload_sixbit(u32 v) {
  static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  return alphabet[v % 36];
}

static inline str wc_upload_stem(Arena *a, str original) {
  Buf b;
  buf_init(&b, a, 32);
  u64 h = str_hash(original);
  h ^= (u64)time_ms() << 13;
  for (int i = 0; i < 10; i++) buf_putc(&b, wc_upload_sixbit((u32)(h >> (i * 5)) ^ (u32)(h >> 3)));
  return buf_str(&b);
}

// The extension for a matched type: from `ext` when the policy gives one, else mapped from the
// MIME type, else "".
static inline str wc_upload_ext(const UploadPolicy *pol, i32 idx, str type) {
  if (pol->ext && idx >= 0 && idx < pol->allow_n && pol->ext[idx]) return str_lit(pol->ext[idx]);
  static const struct {
    const char *type;
    const char *ext;
  } map[] = {{"image/png", "png"},
             {"image/jpeg", "jpg"},
             {"image/gif", "gif"},
             {"image/webp", "webp"},
             {"application/pdf", "pdf"},
             {"application/zip", "zip"},
             {"video/mp4", "mp4"},
             {"video/webm", "webm"},
             {"audio/mpeg", "mp3"},
             {"text/csv", "csv"},
             {"text/plain", "txt"},
             {"text/html", "html"}};
  size_t slash = 0;
  while (type.p && slash < type.n && type.p[slash] != ';') slash++;
  str bare = {.p = type.p, .n = slash};
  for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
    if (strcmp(map[i].type, "") != 0 && strlen(map[i].type) == bare.n &&
        memcmp(map[i].type, bare.p, bare.n) == 0)
      return str_lit(map[i].ext);
  return (str){0};
}

// ---------------------------------------------------------------- the take

// Find the part, apply the policy, write the file. `why` is a sentence for the person, not a
// code, because it is going next to the field they just filled in.
static inline Upload wc_upload_take(Request *r, const char *field, const UploadPolicy *pol) {
  Upload up = {0};
  up.ok = false;
  up.why = "";
  if (!r) {
    up.why = "no request: upload_take() is a handler's call";
    return up;
  }
  // `r->multipart` is set by `body(T)` — and a handler that only wants the file has no reason
  // to parse the body into a model first, so relying on it alone answers "the form was not
  // sent as a file upload" for a good upload. Parsing it here is the same parser with the same
  // limits, and it happens once: the result is stored on the request for the next call.
  if (!r->multipart) {
    str ctype = req_header(r, "content-type");
    if (!multipart_is(ctype.p) || r->body.n == 0) {
      up.why = "the body was not a multipart/form-data upload — a <form> with enctype set "
               "sends one";
      return up;
    }
    WMultipart *mp = arena_zalloc(r->arena, sizeof *mp);
    i32 max_parts = r->max_parts > 0 ? r->max_parts : 32;
    size_t max_bytes = r->max_part_bytes ? r->max_part_bytes : 4u << 20;
    WErrRes e = multipart_parse(r->arena, ctype, r->body, max_parts, max_bytes, mp);
    if (!e.ok) {
      up.why = str_cstr(r->arena, werr_msg(&e.e));
      return up;
    }
    r->multipart = mp;
  }
  WMultipart *mp = r->multipart;
  const WPart *part = nullptr;
  for (i32 i = 0; i < mp->n; i++) {
    WPart *p = &mp->parts[i];
    if (!p->is_file) continue;
    if (strlen(field) == p->name.n && memcmp(field, p->name.p, p->name.n) == 0) {
      part = p;
      break;
    }
  }
  if (!part) {
    up.why = "the file did not arrive";
    return up;
  }
  up.original = part->filename;
  up.bytes = part->data.n;
  if (part->data.n == 0) {
    up.why = "the file is empty";
    return up;
  }
  size_t cap = pol->max_bytes ? pol->max_bytes : (size_t)r->max_part_bytes;
  if (cap && part->data.n > cap) {
    up.why = "the file is larger than this upload allows";
    return up;
  }
  i32 matched = -1;
  str type = part->content_type.p && part->content_type.n ? part->content_type
                                                          : str_lit(wc_upload_sniff(part->data)
                                                                        ? wc_upload_sniff(part->data)
                                                                        : "");
  if (pol->allow && pol->allow_n > 0) {
    for (i32 i = 0; i < pol->allow_n && matched < 0; i++)
      if (wc_upload_type_matches(type, pol->allow[i])) matched = i;
    if (matched < 0) {
      up.why = "that kind of file is not accepted here";
      return up;
    }
    // a declared type has to survive its own first bytes, otherwise a `.png` that is a shell
    // script walks straight into a directory the web server hands out
    if (!wc_upload_bytes_agree(type, part->data)) {
      up.why = "the file is not the kind it says it is";
      return up;
    }
  }
  const char *sniffed = wc_upload_sniff(part->data);
  up.type = sniffed ? str_lit(sniffed) : type;
  Arena *a = wc_arena_get();
  if (pol->dir && *pol->dir && !wc_upload_mkdir_p(pol->dir)) {
    up.why = "the upload directory could not be created";
    return up;
  }
  Buf name;
  buf_init(&name, a, 64);
  buf_putstr(&name, wc_upload_stem(a, part->filename.p ? part->filename : part->name));
  str ext = wc_upload_ext(pol, matched, up.type);
  if (ext.n) {
    buf_putc(&name, '.');
    buf_putstr(&name, ext);
  }
  up.name = buf_str(&name);
  if (pol->dir && *pol->dir) {
    Buf path;
    buf_init(&path, a, name.n + strlen(pol->dir) + 2);
    buf_puts(&path, pol->dir);
    buf_putc(&path, '/');
    buf_putstr(&path, up.name);
    up.path = buf_str(&path);
    if (!wc_upload_write(up.path.p, part->data, pol->public_read ? 0644 : 0600)) {
      up.why = "the file could not be saved";
      return up;
    }
  }
  up.ok = true;
  return up;
}
// Three arguments, and no `body(T)` needed first: an upload handler that wants only the file
// says so, and the parse happens here.
#define upload_take(req_, field_, pol_) wc_upload_take((req_), (field_), (pol_))

// The input that makes the browser show a file picker with the right filter, so the wrong file
// is usually never uploaded at all.
static inline void wc_upload_input(const char *field, const UploadPolicy *pol, bool multiple) {
  attr("type", "file");
  attr("name", field);
  attr("id", wc_form_id(nullptr, nullptr, field));
  if (multiple) attr("multiple", "multiple");
  if (pol && pol->allow && pol->allow_n > 0) {
    Buf b;
    buf_init(&b, wc_arena_get(), 64);
    for (i32 i = 0; i < pol->allow_n; i++) {
      if (i) buf_putc(&b, ',');
      buf_puts(&b, pol->allow[i]);
    }
    attr("accept", buf_str(&b));
  }
}
#define upload_input(field_) wc_upload_input((field_), nullptr, false)
#define upload_input_for(field_, pol_) wc_upload_input((field_), (pol_), false)

// "1.4 MB" — for the message next to the field, so a refusal reads like a sentence.
str wc_upload_bytes_text(size_t n);
#define bytes_text(n_) wc_upload_bytes_text((n_))