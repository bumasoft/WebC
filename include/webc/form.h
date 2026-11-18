// SPDX-License-Identifier: Apache-2.0
// webc/form.h — a form that is drawn from the model it validates with.
//
// A model already knows what its fields are, which are required, how long they may be and
// what a bad value reads like. The form is the same facts, drawn: so `form_field()` writes
// the label, the input of the right kind, the bounds, and the error message when there is
// one — and adding a field to the model adds a field to the form, with its validation, once.
//
//   WEBC_MODEL(Signup,
//     (email, str, .not_null = true, .required = true, .max = 120, .format = "email"),
//     (plan,  Plan, .not_null = true),
//     (ahead, bool, .default_false = true))
//
//   WForm f;
//   Signup draft = {0};
//   form_init(&f, Signup_modelof(), &draft);
//
//   el("form") { post_to("/signup");
//     form_fields(&f);                  // every writable field, in declaration order
//     form_errors(&f);                  // the summary, only when there is something to say
//     el("button") { type_("submit"); text("Sign up"); }
//   }
//
// Everything the kit emits is server-rendered HTML with a `name` on it, so the form works
// with JavaScript off and posts back to the same model that validated it. The kit is
// rendering only: it reads the object through the descriptor, it never writes to it, and
// the binding of an input to a signal stays the app's one `bind_str()`.
//
// Two forms on one page get two prefixes, which is the one thing a hand-written field set
// gets wrong at the second form:
//
//   f.prefix = "edit";                // id="edit-title", label for="edit-title"
//
// What it will not do: lay itself out, choose a label's copy, or decide that a field is a
// textarea. A long field says so — `form_field_long(&f, "bio", 6)`.
#pragma once

#include <webc/dom.h>
#include <webc/fe.h>
#include <webc/model.h>

// The label a field gets when nobody wrote one: the identifier, made out of words.
// "first_name" -> "First name", "ahead" -> "Ahead". It is not copy — it is the identifier
// again, so a form the server renders before anyone has written labels is still readable,
// and the app overrides it by passing its own text to `form_field_as()`.
static inline const char *wc_form_label(Text name) {
  static char bufs[4][96];
  static int which;
  char *out = bufs[which++ & 3];
  const char *in = name ? name : "";
  size_t n = 0;
  for (size_t i = 0; in[i] && n + 1 < sizeof bufs[0]; i++) {
    char c = in[i];
    if (c == '_' || c == '-') {
      out[n++] = ' ';
      continue;
    }
    out[n++] = (i == 0 && c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
  }
  out[n] = 0;
  return out;
}
#define form_label(name_) wc_form_label((name_))

static inline const WField *wc_form_field_of(const WModel *m, const char *name) {
  if (!m || !name) return nullptr;
  for (i32 i = 0; i < m->n; i++)
    if (strcmp(m->fields[i].name, name) == 0) return &m->fields[i];
  return nullptr;
}
#define form_field_of(m_, name_) wc_form_field_of((m_), (name_))

// The value as text, into a rotating buffer: four live at once, which is one row of a form.
static inline const char *wc_form_text_of(const WField *wf, const void *obj) {
  static char bufs[4][64];
  static int which;
  const char *base = (const char *)obj + wf->off;
  char *out = bufs[which++ & 3];
  switch (wf->kind) {
  case WK_STR: {
    str s = *(const str *)base;
    return s.p ? s.p : ""; // a str is already text; no copy, no buffer
  }
  case WK_TEXT: {
    const char *s = *(const char *const *)base;
    return s ? s : "";
  }
  case WK_I8: snprintf(out, sizeof bufs[0], "%d", (int)*(const i8 *)base); return out;
  case WK_I16: snprintf(out, sizeof bufs[0], "%d", (int)*(const i16 *)base); return out;
  case WK_I32: snprintf(out, sizeof bufs[0], "%d", *(const i32 *)base); return out;
  case WK_I64:
    snprintf(out, sizeof bufs[0], "%lld", (long long)*(const i64 *)base);
    return out;
  case WK_U8: snprintf(out, sizeof bufs[0], "%u", (unsigned)*(const u8 *)base); return out;
  case WK_U16: snprintf(out, sizeof bufs[0], "%u", (unsigned)*(const u16 *)base); return out;
  case WK_U32: snprintf(out, sizeof bufs[0], "%u", *(const u32 *)base); return out;
  case WK_U64:
    snprintf(out, sizeof bufs[0], "%llu", (unsigned long long)*(const u64 *)base);
    return out;
  case WK_F32: snprintf(out, sizeof bufs[0], "%g", (double)*(const f32 *)base); return out;
  case WK_F64: snprintf(out, sizeof bufs[0], "%g", *(const f64 *)base); return out;
  default: return "";
  }
}

static inline const char *wc_form_error_href(const WForm *f, i32 which) {
  static char bufs[4][160];
  static int which_buf;
  char *out = bufs[which_buf++ & 3];
  str fld = f->errs.items[which].field;
  snprintf(out, sizeof bufs[0], "#%s-%.*s", f->prefix && *f->prefix ? f->prefix : "f",
           (int)fld.n, fld.p ? fld.p : "");
  return out;
}

static inline const char *wc_form_id(const WForm *f, const WField *wf, const char *suffix) {
  static char bufs[4][160];
  static int which;
  char *out = bufs[which++ & 3];
  snprintf(out, sizeof bufs[0], "%s-%s%s", f->prefix && *f->prefix ? f->prefix : "f", wf->name,
           suffix ? suffix : "");
  return out;
}

// The `type` attribute a format asks for. A field that says `.format = "email"` is asking
// for the keyboard, the autofill and the browser's own check, not just a server-side rule.
static inline const char *wc_form_type(const WField *wf) {
  if (wf->q.format) {
    static const char *const known[] = {"email", "url", "tel", "password", "date", "time",
                                        "search"};
    for (i32 i = 0; i < WC_ARRLEN(known); i++)
      if (strcmp(wf->q.format, known[i]) == 0) return known[i];
  }
  if (wf->kind == WK_F32 || wf->kind == WK_F64) return "number";
  if (wf->kind >= WK_I8 && wf->kind <= WK_U64) return "number";
  return "text";
}

// One field: label, control, error. In that order, because that is the order a reader
// meets them and the order a screen reader announces them.
static inline void wc_form_field_kind(WForm *f, const char *name, i32 rows) {
  const WField *wf = wc_form_field_of(f->m, name);
  if (!wf) return;
  bool text_kind = WC_KIND_IS_TEXT(wf->kind);
  bool num_kind = (wf->kind >= WK_I8 && wf->kind <= WK_F64) && !text_kind;
  const char *id = wc_form_id(f, wf, "");
  const char *err_id = wc_form_id(f, wf, "-err");
  str err = form_error(f, name);
  bool bad = err.n > 0;

  el("div") {
    class_(bad ? "field err" : "field");
    el("label") {
      attr("for", id);
      text(wc_form_label(wf->name));
    }
    if (wf->kind == WK_BOOL) {
      el("input") {
        type_("checkbox");
        attr("name", wf->name);
        id_(id);
        checked(*(const bool *)((const char *)f->obj + wf->off));
        if (bad) attr("aria-invalid", "true");
      }
    } else if (wf->en) {
      el("select") {
        attr("name", wf->name);
        id_(id);
        if (wf->q.required) required();
        if (bad) {
          attr("aria-invalid", "true");
          attr("aria-describedby", err_id);
        }
        i32 cur = *(const i32 *)((const char *)f->obj + wf->off);
        for (i32 i = 0; i < wf->en->n; i++)
          el("option") {
            value_(wf->en->names[i]);
            if (i == cur) attr("selected", "true");
            text(wf->en->names[i]);
          }
      }
    } else if (rows > 0) {
      el("textarea") {
        attr("name", wf->name);
        id_(id);
        attr_i("rows", rows);
        if (wf->q.required) required();
        if (wf->q.max > 0) attr_i("maxlength", wf->q.max);
        if (bad) {
          attr("aria-invalid", "true");
          attr("aria-describedby", err_id);
        }
        text(wc_form_text_of(wf, f->obj));
      }
    } else {
      el("input") {
        type_(wc_form_type(wf));
        attr("name", wf->name);
        id_(id);
        value_(wc_form_text_of(wf, f->obj));
        if (wf->q.required) required();
        if (text_kind && wf->q.max > 0) attr_i("maxlength", wf->q.max);
        if (num_kind) {
          attr("step", wf->kind == WK_F32 || wf->kind == WK_F64 ? "any" : "1");
          if (WC_HAS_GTE(wf->q)) attr_fmt("min", "%g", wf->q.gte);
          if (WC_HAS_LTE(wf->q)) attr_fmt("max", "%g", wf->q.lte);
        }
        if (bad) {
          attr("aria-invalid", "true");
          attr("aria-describedby", err_id);
        }
      }
    }
    if (bad) {
      el("p") {
        class_("field-err");
        id_(err_id);
        text(err);
      }
    }
  }
}
// The field under a name. `form_field(&f, "email")` reads the model, not the view.
#define form_field(f_, name_) wc_form_field_kind((f_), (name_), 0)
#define form_field_long(f_, name_, rows_) wc_form_field_kind((f_), (name_), (rows_))

// Every field the kit can draw, in the order the model declares them — which is the order
// the author wrote the struct in, so a form follows the struct without a second list.
//
// The primary key is the exception, and it is the one that matters: an editable id field is
// a form that can be talked into writing a different row. A pk is emitted as a hidden input
// instead — the id has to survive the POST so an edit knows what it is editing, and it must
// not be typeable.
static inline void wc_form_fields(WForm *f) {
  if (!f || !f->m) return;
  for (i32 i = 0; i < f->m->n; i++) {
    const WField *wf = &f->m->fields[i];
    if (wf->kind == WK_MODEL || wf->kind == WK_VEC) continue; // a nested row is the app's
    if (wf->q.pk) {
      if (WC_KIND_IS_TEXT(wf->kind))
        el("input") {
          type_("hidden");
          attr("name", wf->name);
          value_(wc_form_text_of(wf, f->obj));
        }
      else
        el("input") {
          type_("hidden");
          attr("name", wf->name);
          value_(wc_form_text_of(wf, f->obj));
        }
      continue;
    }
    wc_form_field_kind(f, wf->name, 0);
  }
}
#define form_fields(f_) wc_form_fields((f_))

// The summary: one line per bad field, in a live region so a reader who could not see the
// page change still hears the submission fail. Nothing at all when the form is fine.
static inline void wc_form_errors(WForm *f) {
  if (!f || f->errs.n == 0) return;
  el("ul") {
    class_("form-errors");
    attr("role", "alert");
    for (i32 i = 0; i < f->errs.n; i++) {
      el("li") {
        el("a") {
          // the summary's link goes to the field it names, which is why the id prefix is
          // part of the contract and not a detail
          attr("href", wc_form_error_href(f, i));
          text(wc_form_label(f->errs.items[i].field.p));
        }
        text(": ");
        text(f->errs.items[i].msg);
      }
    }
  }
}
#define form_errors(f_) wc_form_errors((f_))