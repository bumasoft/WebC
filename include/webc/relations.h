// SPDX-License-Identifier: Apache-2.0
// WebC — relations between models: say it once, prefetch it once, read it in the view.
//
// A story belongs to a section. The section is a row of its own, and every app that has a
// relation answers it the same three ways: one query per row (the N+1 that is invisible
// until the page is slow), a `Site` struct carrying every collection through every
// function, or a hand-written join nobody can type-check.
//
//   app.h    WEBC_MODEL_REF(Story, section, Section, slug)
//   server   prefetch(all, Story, section);          // one query for the whole page
//   view     const Section *sec = ref(Story, st, section);
//
// `ref()` needs no database: it reads the links `prefetch` made for this request. In the
// browser half there is nothing to prefetch from, so a page view that runs both halves
// should read a relation only if the server half filled it — or use the row it was given.
#ifndef WEBC_RELATIONS_H
#define WEBC_RELATIONS_H

#include <webc/core.h>
#include <webc/model.h>

// One declared relation. Small and plain on purpose: it is read by prefetch(), by
// `webc check`, and by anyone printing what the app declares.
typedef struct RefRule {
  const char *owner;  // "Story"
  const char *field;  // "section"
  const char *target; // "Section"
  const char *key;    // "slug"
} RefRule;

// The other direction, which is a different question and therefore a different
// declaration: "a Section's stories are every Story whose `section` references this
// Section". It names the belongs-to rule it is the reverse of (`target` + `back_field`),
// so prefetch reads the FK column from that rule and there is exactly one place the
// relation is described.
typedef struct ManyRule {
  const char *owner;      // "Section"
  const char *field;      // "stories"
  const char *target;     // "Story"
  const char *back_field; // "section" — Story's field that holds the key
  const struct WVecDesc *vec; // VecStory's descriptor: what refs() hands the view back
} ManyRule;

// One line where the models are, next to the field that holds the key. The declaration is
// pure data, so it belongs in a shared header: `prefetch` is the half that needs a database.
#define WEBC_MODEL_REF(T_, field_, RT_, rfield_)                                \
  __attribute__((constructor)) static void WC_CAT(wc_ref_decl_, WC_CAT(T_, field_))(void) { \
    wc_ref_register(#T_, #field_, #RT_, #rfield_);                             \
  }

void wc_ref_register(const char *owner, const char *field, const char *target, const char *key);
const RefRule *wc_ref_rule(const char *owner, const char *field);
i32  wc_ref_rule_count(void);
// For `webc check` and for an app that wants to print what it declared.
const char *wc_ref_rule_at(i32 i, const char **owner, const char **field, const char **target,
                           const char **key);

// ---------------------------------------------------------------- has-many
//
//   WEBC_MODEL_MANY(Section, stories, Story, section)
//
// Registers the reverse of WEBC_MODEL_REF(Story, section, Section, slug) and, with it, the
// typed accessor the view calls:
//
//   prefetch(all, Section, stories);                       // ONE query for the page
//   for_each(s, *all) for_each_ptr(st, *refs(Section, s, stories)) { … st->title … }
//
// The accessor is generated rather than written by hand because `refs(Section, s, stories)`
// has to *know* VecStory to hand back a typed collection, and the one place those three
// names meet is this declaration.
void wc_many_register(const char *owner, const char *field, const char *target,
                      const char *back_field, const struct WVecDesc *vec);
const ManyRule *wc_many_rule(const char *owner, const char *field);
i32  wc_many_rule_count(void);
const char *wc_many_rule_at(i32 i, const char **owner, const char **field, const char **target,
                            const char **back_field);

// The collection a prefetch filled, or an empty one (with a warning) if nobody prefetched
// it — a list has no "missing" rendering: iterating null is a crash, and iterating an
// empty list is a view that quietly lost its rows, so it is the warning that speaks.
void *wc_refs(const WModel *m, const void *row, const char *field);
void wc_many_link(const char *owner, i64 pk, const char *field, void *vec, Arena *epoch);
// The collection prefetch filled for this row, or null — no warning, no allocation. This is
// the form `prefetch` itself uses (it has to hand one collection to every owner row and
// append across chunked statements).
void *wc_many_vec(const char *owner, i64 pk, const char *field, Arena *epoch);
// An empty collection of the rule's type, living in `a`. Shared with `prefetch` so a
// collection always comes from the declared descriptor.
void *wc_many_vec_new(const ManyRule *rule, Arena *a);

#define WEBC_MODEL_MANY(T_, field_, RT_, rfield_)                              \
  __attribute__((constructor)) static void WC_CAT(wc_many_decl_, WC_CAT(T_, field_))(void) { \
    wc_many_register(#T_, #field_, #RT_, #rfield_, &WC_CAT(WC_VECDESC_Vec, RT_));   \
  }                                                                            \
  static inline WC_CAT(Vec, RT_) *WC_CAT(WC_CAT(T_, _), field_)(const void *row_) { \
    return (WC_CAT(Vec, RT_) *)wc_refs(T_##_modelof(), row_, #field_);           \
  }

#define refs(T_, row_, f_) WC_CAT(WC_CAT(T_, _), f_)((const void *)(row_))

// The link table, filled by prefetch() and read by ref().
void wc_ref_link(const char *owner, i64 pk, const char *field, const void *row, Arena *epoch);

// Link one collection against another by a field — the client twin of prefetch:
// a document's decoded rows, joined in one call. After it, ref(Owner, row, field)
// answers from the link table on either half.
void wc_doc_join(Arena *epoch, const WModel *owner_m, const WVecDesc *ov, const void *ovec,
                 const char *field, const WModel *target_m, const WVecDesc *tv,
                 const void *tvec);
void wc_ref_clear(void);
// Name the arena decoded rows live in, when it is not the current one (a store
// that rotates generations, a test). One call after the decode; empty = current.
void wc_ref_epoch(Arena *a);
// The same, for a scope: sets it and returns what was there, so the caller can put it back.
// A render does this around the view with the *request's* arena — the rows a `prefetch` linked
// live there, while the template runs in a scratch arena of its own.
Arena *wc_ref_epoch_swap(Arena *a);
i32  wc_ref_link_count(void);

// `ref(Story, st, section)` — the row `prefetch` linked, or null if nobody prefetched it.
const void *wc_ref(const WModel *m, const void *row, const char *field);
#define ref(T_, row_, f_) wc_ref(T_##_modelof(), (const void *)(row_), #f_)

#endif // WEBC_RELATIONS_H