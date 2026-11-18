// SPDX-License-Identifier: Apache-2.0
// webc — configuration: env first, then a JSON file, then the field defaults
// (SPEC §6.6). One reader, one precedence rule, and the schema is the model.
#include <ctype.h>
#include <stdio.h>

#include <webc/config.h>
#include <webc/json.h>

static void env_name(char *out, size_t cap, const char *prefix, const char *field) {
  size_t k = 0;
  for (const char *p = prefix; *p && k + 1 < cap; p++) out[k++] = *p;
  for (const char *p = field; *p && k + 1 < cap; p++)
    out[k++] = (char)toupper((unsigned char)*p);
  out[k] = '\0';
}

// Coerce the text of an environment variable into the field, by kind. A value that
// does not fit is an error naming the field: `WEBC_PORT=eighty` must not become 0.
static bool set_from_text(Arena *a, const WField *f, void *out, const char *text, WErr *err) {
  (void)a;
  char *base = (char *)out + f->off;
  switch ((WKind)f->kind) {
  case WK_STR: {
    *(str *)base = (str){strdup(text), strlen(text)};
    return true;
  }
  case WK_BOOL: {
    size_t n = strlen(text);
    char low[16] = {0};
    for (size_t i = 0; i < n && i < sizeof low - 1; i++) low[i] = (char)tolower((unsigned char)text[i]);
    if (!strcmp(low, "1") || !strcmp(low, "true") || !strcmp(low, "yes") || !strcmp(low, "on")) {
      *(bool *)base = true;
      return true;
    }
    if (!strcmp(low, "0") || !strcmp(low, "false") || !strcmp(low, "no") || !strcmp(low, "off") ||
        !*low) {
      *(bool *)base = false;
      return true;
    }
    break;
  }
  case WK_F32:
  case WK_F64: {
    char *end = nullptr;
    double v = strtod(text, &end);
    if (end && *end == '\0') {
      if (f->kind == WK_F32)
        *(f32 *)base = (f32)v;
      else
        *(f64 *)base = v;
      return true;
    }
    break;
  }
  default: {
    char *end = nullptr;
    long long v = strtoll(text, &end, 10);
    if (end && *end == '\0') {
      switch ((WKind)f->kind) {
      case WK_I8: *(i8 *)base = (i8)v; return true;
      case WK_I16: *(i16 *)base = (i16)v; return true;
      case WK_I32: *(i32 *)base = (i32)v; return true;
      case WK_I64: *(i64 *)base = v; return true;
      case WK_U8: *(u8 *)base = (u8)v; return true;
      case WK_U16: *(u16 *)base = (u16)v; return true;
      case WK_U32: *(u32 *)base = (u32)v; return true;
      case WK_U64: *(u64 *)base = (u64)v; return true;
      default: break;
      }
    }
    break;
  }
  }
  *err = werr(WEBC_EINVAL,
              str_fmt(a, "%s: '%s' is not a %s", f->name, text, wc_kind_name((WKind)f->kind)));
  return false;
}

// How many bytes a model's fields span: the model table carries offsets and sizes,
// not a struct size (two models can describe one struct), so the end of the last
// field is the most that can be zeroed safely.
static size_t model_bytes(const WModel *m) {
  size_t end = 0;
  for (i32 i = 0; i < m->n; i++) {
    size_t f = (size_t)m->fields[i].off + m->fields[i].size;
    if (f > end) end = f;
  }
  return end;
}

WErrRes config_load_model(Arena *a, const WModel *m, void *out, const char *path, const char *env) {
  static const char *const prefixes[] = {"WEBC_", ""};
  memset(out, 0, model_bytes(m));

  // 2. the file: flat, or a section per environment with "defaults" underneath
  if (path) {
    FILE *f = fopen(path, "rb");
    if (f) {
      Buf text;
      buf_init(&text, a, 4096);
      char chunk[4096];
      size_t n;
      while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_putstr(&text, (str){chunk, n});
      fclose(f);
      JParse p = json_parse(a, buf_str(&text));
      if (!p.ok) return wfail(WEBC_EPARSE, str_fmt(a, "%s: %s", path, p.e.buf));
      JVal *src = p.v;
      if (src && src->kind == J_OBJ) {
        JVal *def = json_get(src, "defaults");
        bool sectioned = def != nullptr || (env && json_get(src, env) != nullptr);
        if (sectioned) {
          // defaults first, then the environment's own keys on top
          for (i32 round = 0; round < 2; round++) {
            JVal *layer = round == 0 ? def : (env ? json_get(src, env) : nullptr);
            if (!layer || layer->kind != J_OBJ) continue;
            for (i32 i = 0; i < layer->n; i++) {
              for (i32 k = 0; k < m->n; k++) {
                if (!str_eq_c(layer->keys[i], m->fields[k].name)) continue;
                // one key at a time, through the same JSON reader the rest of the
                // framework uses: a config field is a model field, not a special case
                JVal one = {.kind = J_OBJ, .n = 1, .keys = &layer->keys[i],
                            .items = &layer->items[i]};
                WModelParse mp = model_from_json_val(a, m, &one, out);
                if (!mp.ok) return (WErrRes){.ok = false, .e = mp.e};
              }
            }
          }
          src = nullptr; // already consumed
        }
      }
      if (src) {
        WModelParse mp = model_from_json_val(a, m, src, out);
        if (!mp.ok) return (WErrRes){.ok = false, .e = mp.e};
      }
    } else if (env && *env) {
      // No file at all is fine (env-only deployment); the caller named a file that
      // isn't there, which we allow on purpose: containers often have neither.
    } else {
      // missing file, no environment: still fine — env and defaults remain
    }
  }

  // 1. the environment, which outranks the file
  for (i32 k = 0; k < m->n; k++) {
    const WField *fd = &m->fields[k];
    for (size_t pi = 0; pi < WC_COUNTOF(prefixes); pi++) {
      char name[128];
      env_name(name, sizeof name, prefixes[pi], fd->name);
      const char *v = getenv(name);
      if (!v) continue;
      WErr err = {0};
      if (!set_from_text(a, fd, out, v, &err)) return (WErrRes){.ok = false, .e = err};
      break;
    }
  }
  return wok();
}

str config_schema_doc(Arena *a, const WModel *m) {
  Buf b;
  buf_init(&b, a, 256);
  buf_printf(&b, "%s\n", m->name);
  for (i32 i = 0; i < m->n; i++) {
    const WField *f = &m->fields[i];
    char name[128];
    env_name(name, sizeof name, "WEBC_", f->name);
    buf_printf(&b, "  %-20s %-6s env %s", f->name, wc_kind_name((WKind)f->kind), name);
    if (f->q.required) buf_puts(&b, "  required");
    if (f->q.doc) buf_printf(&b, "  — %s", f->q.doc);
    buf_putc(&b, '\n');
  }
  return buf_str(&b);
}
