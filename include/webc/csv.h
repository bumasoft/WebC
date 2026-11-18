// SPDX-License-Identifier: Apache-2.0
// webc/csv.h — a CSV that a spreadsheet can open, and one that can be read back.
//
// The two bugs in every hand-written CSV are the same two: an exporter that forgets a field
// may contain a comma, a quote or a newline, and an importer that keeps the `\r` of a `\r\n`
// line ending in the last column. This file exists so an app does not find out later.
//
// **Out — streamed, so a 200k-row export is not a 200 MB string:**
//
//   static void pump(Response *r, void *ctx) {
//     VecNote *rows = ctx;
//     csv_header(r, Note_modelof());     // title,done,id — docs win, secrets never, id last
//     for_each(n, (*rows)) csv_write_row(r, Note_modelof(), n);
//   }
//   GET("/notes.csv") { return stream(200, pump, rows); }
//
// A header cell is the column name — the field's `.doc` when it has one, because that is the
// place a model already keeps the words a human should read. The `id` column comes last (a
// spreadsheet's first column is the thing you sort by, not the number nothing uses) and a
// field marked `.sensitive` or `.no_json` is not exported at all.
//
// **In — a row at a time, with the reason it failed:**
//
//   CSVParse p = csv_parse(req->body);   // or csv_parse_file("/tmp/import.csv")
//   CSVRow row;
//   while (csv_next(&p, &row)) {
//     Note n;
//     if (!csv_into(Note_modelof(), &p, &row, &n)) return res_error(422, "bad_csv", row.why);
//     if (!save(&n)) return res_error(422, "bad_row", row.why);   // row.line says which
//   }
//   if (p.bad) return res_error(422, "bad_csv", p.why);
//
// `csv_next` reads in place — no allocation per row — and `row.line` is 1-based so a message
// message the reader can act on, and a quoted field may contain commas, quotes (`""`) and
// newlines — which is the whole of RFC 4180 that anyone actually meets.
//
// **Refuses:** Excel's codepage dialects (a `sep=;` first line, UTF-16, `;` as a delimiter by
// locale) — the delimiter is configurable and nothing else is guessed; encodings beyond UTF-8;
// a spreadsheet model; formulas.
#pragma once

#include <webc/core.h>
#include <webc/http.h>
#include <webc/kinds.h>
#include <webc/model.h>

// ---------------------------------------------------------------- out

// One cell, quoted only when it has to be: a comma, a quote, a newline, or leading/trailing
// space (which a spreadsheet trims and a person did not mean to lose).
static inline void wc_csv_cell(Buf *b, str v) {
  bool quote = false;
  for (size_t i = 0; i < v.n; i++) {
    char c = v.p[i];
    if (c == ',' || c == '"' || c == '\n' || c == '\r') {
      quote = true;
      break;
    }
  }
  if (!quote && v.n && (v.p[0] == ' ' || v.p[v.n - 1] == ' ')) quote = true;
  if (!quote) {
    if (v.p && v.n) buf_putstr(b, v);
    return;
  }
  buf_putc(b, '"');
  for (size_t i = 0; i < v.n; i++) {
    if (v.p[i] == '"') buf_putc(b, '"'); // the RFC's escape: "" inside a quoted cell
    buf_putc(b, v.p[i]);
  }
  buf_putc(b, '"');
}
#define csv_cell(buf_, s_) wc_csv_cell((buf_), WC_AS_STR(s_))

// A cell for a raw `str` that is already text (a column name, a header).
static inline void wc_csv_cell_str(Buf *b, str v) { wc_csv_cell(b, v); }

// One model row as a line. Numbers are printed with the same rules JSON uses, so a value that
// round-trips through the wire round-trips through the file; a `bool` is `true`/`false` and an
// enum is its name, because "1" in a spreadsheet is a number nobody can sort by.
static inline str wc_csv_row_text(const WModel *m, const void *obj);
static inline str wc_csv_header_text(const WModel *m);
#define csv_row_text(m_, obj_) wc_csv_row_text((m_), (obj_))
#define csv_header_text(m_) wc_csv_header_text((m_))

static inline bool wc_csv_exportable(const WField *f) {
  if (f->q.sensitive || f->q.no_json) return false;
  if (f->kind == WK_MODEL || f->kind == WK_VEC) return false; // a nested row is not a cell
  return true;
}

// The columns, in the model's order, with the primary key moved to the end: a spreadsheet's
// first column is the one you sort and freeze, and an opaque id there is a nuisance.
static inline void wc_csv_columns(const WModel *m, const WField **out, i32 cap, i32 *n) {
  i32 k = 0;
  for (i32 i = 0; i < m->n && k < cap; i++)
    if (wc_csv_exportable(&m->fields[i]) && !m->fields[i].q.pk) out[k++] = &m->fields[i];
  for (i32 i = 0; i < m->n && k < cap; i++)
    if (wc_csv_exportable(&m->fields[i]) && m->fields[i].q.pk) out[k++] = &m->fields[i];
  *n = k;
}

static inline str wc_csv_cell_of(Buf *b, const WField *f, const void *obj) {
  const char *base = (const char *)obj + f->off;
  switch (f->kind) {
  case WK_BOOL: buf_puts(b, *(const bool *)base ? "true" : "false"); break;
  case WK_STR: {
    str v = *(const str *)base;
    if (v.p && v.n) buf_putstr(b, v);
    break;
  }
  case WK_TEXT: {
    const char *s = *(const char *const *)base;
    if (s) buf_puts(b, s);
    break;
  }
  case WK_ENUM: {
    i32 v = *(const i32 *)base;
    buf_puts(b, wc_enum_name(f->en, v));
    break;
  }
  case WK_I8: buf_printf(b, "%d", (int)*(const i8 *)base); break;
  case WK_I16: buf_printf(b, "%d", (int)*(const i16 *)base); break;
  case WK_I32: buf_printf(b, "%d", *(const i32 *)base); break;
  case WK_I64: buf_printf(b, "%lld", (long long)*(const i64 *)base); break;
  case WK_U8: buf_printf(b, "%u", (unsigned)*(const u8 *)base); break;
  case WK_U16: buf_printf(b, "%u", (unsigned)*(const u16 *)base); break;
  case WK_U32: buf_printf(b, "%u", *(const u32 *)base); break;
  case WK_U64: buf_printf(b, "%llu", (unsigned long long)*(const u64 *)base); break;
  case WK_F32: buf_printf(b, "%.9g", (double)*(const f32 *)base); break;
  case WK_F64: buf_printf(b, "%.17g", *(const f64 *)base); break;
  default: break;
  }
  return buf_str(b);
}

static inline str wc_csv_row_text(const WModel *m, const void *obj) {
  const WField *cols[64];
  i32 n = 0;
  Buf b;
  buf_init(&b, wc_arena_get(), 256);
  wc_csv_columns(m, cols, 64, &n);
  for (i32 i = 0; i < n; i++) {
    if (i) buf_putc(&b, ',');
    Buf cell;
    buf_init(&cell, wc_arena_get(), 64);
    wc_csv_cell_of(&cell, cols[i], obj);
    wc_csv_cell(&b, buf_str(&cell));
  }
  return buf_str(&b);
}

static inline str wc_csv_header_text(const WModel *m) {
  const WField *cols[64];
  i32 n = 0;
  Buf b;
  buf_init(&b, wc_arena_get(), 128);
  wc_csv_columns(m, cols, 64, &n);
  for (i32 i = 0; i < n; i++) {
    if (i) buf_putc(&b, ',');
    // `.doc` is where a model already keeps the words a human should read; the field name is
    // the fallback, and it is the same name `webc doctor --schema` prints.
    wc_csv_cell(&b, cols[i]->q.doc ? str_lit(cols[i]->q.doc) : str_lit(cols[i]->name));
  }
  return buf_str(&b);
}

// A row written into a streaming response. Returns false when the client went away, so the
// caller's loop can stop instead of writing into a closed socket.
static inline bool wc_csv_write_row(Response *r, const WModel *m, const void *obj) {
  str line = wc_csv_row_text(m, obj);
  Buf b;
  buf_init(&b, wc_arena_get(), line.n + 2);
  buf_putstr(&b, line);
  buf_puts(&b, "\r\n"); // CRLF: the one line ending Excel, Numbers and `csv` all accept
  return webc_stream_write(r, buf_str(&b));
}

#define csv_write_row(r_, m_, obj_) wc_csv_write_row((r_), (m_), (obj_))

static inline bool wc_csv_header(Response *r, const WModel *m) {
  Buf b;
  buf_init(&b, wc_arena_get(), 256);
  buf_putstr(&b, wc_csv_header_text(m));
  buf_puts(&b, "\r\n");
  return webc_stream_write(r, buf_str(&b));
}

#define csv_header(r_, m_) wc_csv_header((r_), (m_))

// ---------------------------------------------------------------- in
//
// The parser reads *in place*: the body is copied once, and each cell is a slice of that copy
// with its quotes unescaped where they stood. That is what makes a 200k-row import cost the
// file's size and nothing per row.
typedef struct CSVRow {
  i32 line;         // 1-based, for "row 41: …"
  i32 n;            // cells in this row
  str *cells;       // the cells, unquoted
  const char *why;  // "" or what is wrong with it
} CSVRow;

typedef struct CSVParse {
  char *text;      // the body, mutable and unescaped in place
  size_t n, at;
  i32 line;
  char delim;
  bool bad;
  const char *why;
  str header[64];
  i32 header_n;
  bool first;
  str cells[64];
  i32 ncells;
} CSVParse;

static inline CSVParse wc_csv_parse_delim(str body, char delim) {
  CSVParse p = {0};
  p.delim = delim ? delim : ',';
  p.line = 1;
  p.first = true;
  if (body.p && body.n) {
    p.text = (char *)arena_alloc(wc_arena_get(), body.n + 1);
    if (!p.text) {
      p.bad = true;
      p.why = "out of memory";
      return p;
    }
    memcpy(p.text, body.p, body.n);
    p.text[body.n] = 0;
    p.n = body.n;
  } else {
    p.bad = true;
    p.why = "the file is empty";
  }
  return p;
}
#define csv_parse(body_) wc_csv_parse_delim(WC_AS_STR(body_), ',')

// A file, read into the request's arena. `bad` and a reason when it cannot be read.
static inline CSVParse wc_csv_parse_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    CSVParse p = {0};
    p.bad = true;
    p.why = "cannot open the file";
    return p;
  }
  Buf b;
  buf_init(&b, wc_arena_get(), 8192);
  char chunk[8192];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_write(&b, chunk, n);
  fclose(f);
  return wc_csv_parse_delim(buf_str(&b), ',');
}

// One record into `p->cells`. False at end of input. A trailing newline is not a record, and a
// blank line in the middle is (a spreadsheet writes those, and silently skipping them would
// shift every line number in every error message after it).
static inline bool wc_csv_scan(CSVParse *p) {
  if (!p || p->bad || p->at >= p->n) return false;
  i32 n = 0;
  while (true) {
    char *start = p->text + p->at;
    char *out = start;
    bool quoted = false;
    if (p->at < p->n && p->text[p->at] == '"') {
      quoted = true;
      p->at++;
      out = p->text + p->at;
      start = out;
    }
    while (p->at < p->n) {
      char c = p->text[p->at];
      if (quoted) {
        if (c == '"') {
          if (p->at + 1 < p->n && p->text[p->at + 1] == '"') {
            *out++ = '"';
            p->at += 2;
            continue;
          }
          p->at++;
          quoted = false;
          continue;
        }
        if (c == '\n') p->line++;
        *out++ = c;
        p->at++;
        continue;
      }
      if (c == p->delim || c == '\n' || c == '\r') break;
      *out++ = c;
      p->at++;
    }
    str cell = {.p = start, .n = (size_t)(out - start)};
    if (n < 64) p->cells[n++] = cell;
    else if (!p->why) p->why = "more than 64 columns";
    if (p->at >= p->n) break;
    char c = p->text[p->at];
    if (c == p->delim) {
      p->at++;
      continue;
    }
    // a line ending (either flavour), which ends the record
    if (c == '\r') p->at++;
    if (p->at < p->n && p->text[p->at] == '\n') p->at++;
    p->line++;
    break;
  }
  p->ncells = n;
  return n > 0;
}

// The next data row. The first call consumes the header row.
static inline bool wc_csv_next(CSVParse *p, CSVRow *row) {
  if (!p || p->bad) return false;
  while (wc_csv_scan(p)) {
    // a blank line is not a row: a hand-edited file has them, and importing one as an empty
    // record would fail a `.required` field on a line the reader cannot see
    if (!p->first && p->ncells == 1 && p->cells[0].n == 0) continue;
    if (p->first) {
      p->first = false;
      p->header_n = p->ncells;
      for (i32 i = 0; i < p->ncells; i++) p->header[i] = p->cells[i];
      continue;
    }
    row->line = p->line - 1; // where the record started
    row->n = p->ncells;
    row->cells = p->cells;
    row->why = "";
    if (p->header_n > 0 && p->ncells != p->header_n)
      row->why = "the row does not have the header's columns";
    return true;
  }
  return false;
}

#define csv_next(p_, row_) wc_csv_next((p_), (row_))
#define csv_into(m_, p_, row_, out_) wc_csv_into((m_), (p_), (row_), (out_))

// One row into a model, *by header name*: a spreadsheet whose columns were reordered still
// imports, and a column that is missing leaves its field alone instead of shifting every value
// one to the left — which is the bug this function exists to prevent. The values go through
// `model_from_form`, so a number that does not parse fails here exactly as it fails in a form.
static inline bool wc_csv_into(const WModel *m, CSVParse *p, CSVRow *row, void *out) {
  if (row->why && *row->why) return false;
  if (!p->header_n) {
    row->why = "no header row";
    return false;
  }
  Arena *a = wc_arena_get();
  Buf q;
  buf_init(&q, a, 256);
  i32 used = 0;
  for (i32 c = 0; c < row->n && c < p->header_n; c++) {
    str name = p->header[c];
    if (!name.n) continue;
    str enc_name = url_encode(a, name);
    str enc_val = url_encode(a, row->cells[c]);
    if (used++) buf_putc(&q, '&');
    buf_putstr(&q, enc_name);
    buf_putc(&q, '=');
    buf_putstr(&q, enc_val);
  }
  WModelParse r = model_from_form(a, m, buf_str(&q), out);
  if (!r.ok) {
    str msg = werr_msg(&r.e);
    row->why = r.errs.n > 0 ? "a value did not fit its field"
                            : (msg.p && msg.n ? msg.p : "the row did not parse");
    return false;
  }
  return true;
}
