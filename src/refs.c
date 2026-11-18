// SPDX-License-Identifier: Apache-2.0
// WebC — relations between models, without a foreign-key type or a join.
//
// A story has a section; the section is a row of its own. Every app answers that with one
// query per row (or with a `Site` struct that carries every collection everywhere it goes),
// because there is nowhere to say "this text field is a key into that model":
//
//   app.h:    WEBC_MODEL_REF(Story, section, Section, slug)
//   server:   prefetch(all, Story, section);            // one query for the whole page
//   view:     const Section *sec = ref(Story, st, section);   // no query at all
//
// The declaration lives where the models live, so both halves can see it. What `prefetch`
// fills is a small table of links for the current request, and it is deliberately not a
// cache: it is valid while the rows are (the request's arena), and gone afterwards.
//
// Nothing here touches a database, so a shared view can read a relation it was given. A
// view that reads one nobody prefetched gets null — and `webc check` says which.
#include <webc/relations.h>
#include <webc/log.h>

// The link tables are shared by every worker, and a request is a green task that parks and
// resumes — so "the current arena" is not per thread and thread-local tables would break on a
// migration. A lock, then, and a short critical section: linking happens once per query, not
// per row. Without it two workers inside `prefetch` at the same time incremented the same
// counter and wrote the same slot: one request's links silently became another's, which is a
// page showing another page's section.
static _Atomic int g_ref_lock;
static void ref_lock(void) {
  while (atomic_exchange(&g_ref_lock, 1)) {
  }
}
static void ref_unlock(void) { atomic_store(&g_ref_lock, 0); }

static void ref_warn_once(const char *owner, const char *field, Arena *epoch);

#define WC_REF_RULES 32
static RefRule g_rules[WC_REF_RULES];
static i32 g_nrules;

void wc_ref_register(const char *owner, const char *field, const char *target, const char *key) {
  if (!owner || !field || !target || !key) return;
  for (i32 i = 0; i < g_nrules; i++) {
    RefRule *r = &g_rules[i];
    if (strcmp(r->owner, owner) != 0 || strcmp(r->field, field) != 0) continue;
    if (strcmp(r->target, target) != 0 || strcmp(r->key, key) != 0)
      panic("%s.%s points at two different things: %s.%s and %s.%s. One relation, one "
            "target — rename one of them.",
            owner, field, r->target, r->key, target, key);
    return;
  }
  if (g_nrules >= WC_REF_RULES) {
    wc_limit_once(&g_nrules, "relation dropped: too many declared", "WC_REF_RULES", "relation");
    return;
  }
  g_rules[g_nrules++] = (RefRule){owner, field, target, key};
}

i32 wc_ref_rule_count(void) { return g_nrules; }

const char *wc_ref_rule_at(i32 i, const char **owner, const char **field, const char **target,
                           const char **key) {
  if (i < 0 || i >= g_nrules) return nullptr;
  if (owner) *owner = g_rules[i].owner;
  if (field) *field = g_rules[i].field;
  if (target) *target = g_rules[i].target;
  if (key) *key = g_rules[i].key;
  return g_rules[i].target;
}

const RefRule *wc_ref_rule(const char *owner, const char *field) {
  for (i32 i = 0; i < g_nrules; i++)
    if (strcmp(g_rules[i].owner, owner) == 0 && strcmp(g_rules[i].field, field) == 0)
      return &g_rules[i];
  return nullptr;
}

// ---------------------------------------------------------------- the other direction

#define WC_MANY_RULES 32
static ManyRule g_many[WC_MANY_RULES];
static i32 g_nmany;

void wc_many_register(const char *owner, const char *field, const char *target,
                      const char *back_field, const WVecDesc *vec) {
  if (!owner || !field || !target || !back_field) return;
  for (i32 i = 0; i < g_nmany; i++) {
    ManyRule *r = &g_many[i];
    if (strcmp(r->owner, owner) != 0 || strcmp(r->field, field) != 0) continue;
    if (strcmp(r->target, target) != 0 || strcmp(r->back_field, back_field) != 0)
      panic("%s.%s points at two different collections: %s.%s and %s.%s. One relation, one "
            "direction — rename one of them.",
            owner, field, r->target, r->back_field, target, back_field);
    return;
  }
  if (g_nmany >= WC_MANY_RULES) {
    wc_limit_once(&g_nmany, "relation dropped: too many declared", "WC_MANY_RULES", "relation");
    return;
  }
  if (!vec) {
    // The typed accessor `refs(Owner, row, field)` hands back Vec<Target>, so the collection
    // type has to exist where the declaration is: one WEBC_VEC_DEFINE (or one Vec(Target))
    // per model, which every model with a has-many already writes for `db_all`.
    panic("%s.%s: no Vec%s in scope. Declare the collection (WEBC_VEC_DEFINE(Vec%s, …) or "
          "Vec(%s)) before WEBC_MODEL_MANY(%s, %s, %s, %s) — refs() returns it.",
          owner, field, target, target, target, owner, field, target, back_field);
  }
  g_many[g_nmany++] = (ManyRule){owner, field, target, back_field, vec};
}

i32 wc_many_rule_count(void) { return g_nmany; }

const char *wc_many_rule_at(i32 i, const char **owner, const char **field, const char **target,
                            const char **back_field) {
  if (i < 0 || i >= g_nmany) return nullptr;
  if (owner) *owner = g_many[i].owner;
  if (field) *field = g_many[i].field;
  if (target) *target = g_many[i].target;
  if (back_field) *back_field = g_many[i].back_field;
  return g_many[i].target;
}

const ManyRule *wc_many_rule(const char *owner, const char *field) {
  for (i32 i = 0; i < g_nmany; i++)
    if (strcmp(g_many[i].owner, owner) == 0 && strcmp(g_many[i].field, field) == 0)
      return &g_many[i];
  return nullptr;
}

// ---------------------------------------------------------------- the links
//
// (owner model, primary key, field) -> the target row. Flat and small: a page has tens of
// relations, and a linear scan of a hundred pointers is faster than a hash and has no
// allocation of its own.
typedef struct RefLink {
  const char *owner;
  i64 pk;
  const char *field;
  const void *row;
  Arena *epoch; // the arena the row (and this link) belongs to
} RefLink;

#define WC_REF_LINKS 512
static RefLink g_links[WC_REF_LINKS];
static i32 g_nlinks;

// Link one collection's rows against another's, by a field — the in-memory join a
// document decode wants on the client (where there is no database to prefetch
// from), and the no-query spelling on the server. The links carry the caller's
// arena as their epoch, exactly as prefetch's do.
void wc_doc_join(Arena *epoch, const WModel *owner_m, const WVecDesc *ov, const void *ovec,
                 const char *field, const WModel *target_m, const WVecDesc *tv,
                 const void *tvec) {
  if (!owner_m || !target_m || !ov || !tv || !ovec || !tvec) return;
  const WField *fk = model_field(owner_m, str_lit(field));
  const WField *opk = nullptr, *tpk = nullptr;
  for (i32 i = 0; i < owner_m->n; i++)
    if (owner_m->fields[i].q.pk) opk = &owner_m->fields[i];
  for (i32 i = 0; i < target_m->n; i++)
    if (target_m->fields[i].q.pk) tpk = &target_m->fields[i];
  if (!fk || !opk || !tpk) return;
  i32 on = wvec_len(ov, ovec), tn = wvec_len(tv, tvec);
  for (i32 i = 0; i < on; i++) {
    void *row = wvec_at(ov, ovec, i);
    i64 key = wfield_get_i64(fk, row);
    if (key == 0) continue;
    for (i32 j = 0; j < tn; j++) {
      void *t = wvec_at(tv, tvec, j);
      if (wfield_get_i64(tpk, t) != key) continue;
      wc_ref_link(owner_m->name, wfield_get_i64(opk, row), field, t, epoch);
      break;
    }
  }
}

void wc_ref_link(const char *owner, i64 pk, const char *field, const void *row, Arena *epoch) {
  if (!owner || !field || !row) return;
  ref_lock();
  for (i32 i = 0; i < g_nlinks; i++) {
    RefLink *l = &g_links[i];
    if (l->epoch != epoch || l->pk != pk || strcmp(l->owner, owner) != 0 ||
        strcmp(l->field, field) != 0)
      continue;
    l->row = row;
    ref_unlock();
    return;
  }
  if (g_nlinks >= WC_REF_LINKS) {
    // Drop the links of any earlier request first: they are dead the moment their arena
    // is, and a long-lived process would otherwise fill the table once and stop linking.
    Arena *now = wc_arena_get();
    i32 keep = 0;
    for (i32 i = 0; i < g_nlinks; i++)
      if (g_links[i].epoch == now) g_links[keep++] = g_links[i];
    g_nlinks = keep;
    if (g_nlinks >= WC_REF_LINKS) {
      wc_limit_once(&g_nlinks, "relation dropped: more than a page's worth of links",
                    "WC_REF_LINKS", "relation");
      ref_unlock();
      return;
    }
  }
  g_links[g_nlinks++] = (RefLink){owner, pk, field, row, epoch};
  ref_unlock();
}

i32 wc_ref_link_count(void) { return g_nlinks; }


// Reads normally answer from links whose arena is the current one (a request's).
// A decoded document outlives its decode: the arena that read it is swapped out
// and the render's scratch is current instead — so the owner of the document
// (a store, a test) names the epoch its rows live in, once, after reading.
static _Thread_local Arena *t_ref_epoch;

void wc_ref_epoch(Arena *a) {
  t_ref_epoch = a;
}

// The scoped form, for code that runs *inside* somebody else's epoch: a render swaps the
// arena to its scratch (`wcomp_html_ctx`), so `ref()` in a view matched links that were
// written in the request arena and found none — for every row, silently, one statement after
// `prefetch` did the work. The epoch names where the rows *live*, not which arena happens to
// be current while a template runs, so the render sets it to the request's arena and puts
// back what it found.
Arena *wc_ref_epoch_swap(Arena *a) {
  Arena *prev = t_ref_epoch;
  t_ref_epoch = a;
  return prev;
}

const void *wc_ref(const WModel *m, const void *row, const char *field) {
  if (!m || !row || !field) return nullptr;
  const WField *pkf = nullptr;
  for (i32 i = 0; i < m->n; i++)
    if (m->fields[i].q.pk) pkf = &m->fields[i];
  if (!pkf) return nullptr;
  i64 pk = wfield_get_i64(pkf, row);
  Arena *epoch = t_ref_epoch ? t_ref_epoch : wc_arena_get();
  const void *found = nullptr;
  ref_lock();
  // newest first: a re-link (a row updated twice in one request) must win
  for (i32 i = g_nlinks - 1; i >= 0; i--) {
    const RefLink *l = &g_links[i];
    if (l->epoch != epoch) continue;
    if (l->pk != pk) continue;
    if (strcmp(l->field, field) != 0) continue;
    if (strcmp(l->owner, m->name) != 0) continue;
    found = l->row;
    break;
  }
  ref_unlock();
  // A null `ref()` is either a row with no target (an optional relation is a real thing) or
  // a view reading a relation nobody prefetched — and the second one is invisible: the page
  // renders with a blank where a name should be. One line per (model, field) per request,
  // never per row, which is what makes it readable in a log.
  if (!found) ref_warn_once(m->name, field, epoch);
  return found;
}
// ---------------------------------------------------------------- one owner, many rows
//
// `ref()` answers with one row because a belongs-to has one. A has-many answers with a
// collection, and the collection is the thing `prefetch` built: one vector per owner row,
// filled from ONE query for the whole page. The difference matters when the page is long —
// thirteen rows and thirteen queries is the N+1 that a relation exists to prevent, and
// `WEBC_TRACE_DB=1` shows the count.
typedef struct ManyLink {
  const char *owner;
  i64 pk;
  const char *field;
  void *vec;   // a WC_VEC_STRUCT-shaped collection, allocated in the epoch arena
  Arena *epoch;
} ManyLink;

#define WC_MANY_LINKS 512
static ManyLink g_mlinks[WC_MANY_LINKS];
static i32 g_nmlinks;


// One warning per (model, field) per request: a view that forgot its prefetch should say so
// once, not once per row. The epoch is what makes "once" mean one page rather than one
// process (a long-lived process serves thousands of pages).
typedef struct ManyWarned {
  const char *owner;
  const char *field;
  const char *kind; // "one" for ref(), "many" for many(); one table, two readers
  Arena *epoch;
} ManyWarned;
#define WC_MANY_WARNED 64
static ManyWarned g_warned[WC_MANY_WARNED];
static i32 g_nwarned;

// The start of a request: nothing is linked yet. A pooled request arena comes back at the
// same address, so an epoch comparison alone cannot tell this request from the last one —
// without this, request 2 reads request 1's rows out of memory request 1 owned.
void wc_ref_clear(void) {
  g_nlinks = 0;
  g_nmlinks = 0;
  g_nwarned = 0;
}

// True the first time this (kind, model, field) is asked about in this epoch. The table is
// shared with the link tables and updated from whichever worker is running the task, so it is
// under the same lock.
static bool warn_once(const char *owner, const char *field, const char *kind, Arena *epoch) {
  ref_lock();
  for (i32 i = 0; i < g_nwarned; i++) {
    if (g_warned[i].epoch != epoch || strcmp(g_warned[i].owner, owner) != 0 ||
        strcmp(g_warned[i].field, field) != 0 || strcmp(g_warned[i].kind, kind) != 0)
      continue;
    ref_unlock();
    return false;
  }
  if (g_nwarned >= WC_MANY_WARNED) {
    // a new request: forget the old page's warnings rather than stop warning
    i32 keep = 0;
    Arena *now = wc_arena_get();
    for (i32 i = 0; i < g_nwarned; i++)
      if (g_warned[i].epoch == now) g_warned[keep++] = g_warned[i];
    g_nwarned = keep;
    if (g_nwarned >= WC_MANY_WARNED) g_nwarned = 0;
  }
  g_warned[g_nwarned++] = (ManyWarned){owner, field, kind, epoch};
  ref_unlock();
  return true;
}

static void many_warn_once(const char *owner, const char *field, Arena *epoch) {
  if (!warn_once(owner, field, "many", epoch)) return;
  log_warn("relation read without a prefetch: the list is empty",
           {"model", str_lit(owner)}, {"field", str_lit(field)},
           {"fix", str_fmt(nullptr, "prefetch(rows, %s, %s)", owner, field)});
}

static void ref_warn_once(const char *owner, const char *field, Arena *epoch) {
  if (!warn_once(owner, field, "one", epoch)) return;
  log_warn("ref() found no link: the row is null in the view",
           {"model", str_lit(owner)}, {"field", str_lit(field)},
           {"fix", str_fmt(nullptr, "prefetch(rows, %s, %s) in the handler", owner, field)},
           {"note", str_lit("a row with no target is null too; this says it once per field")});
}

void wc_many_link(const char *owner, i64 pk, const char *field, void *vec, Arena *epoch) {
  if (!owner || !field || !vec) return;
  for (i32 i = 0; i < g_nmlinks; i++) {
    ManyLink *l = &g_mlinks[i];
    if (l->epoch != epoch || l->pk != pk || strcmp(l->owner, owner) != 0 ||
        strcmp(l->field, field) != 0)
      continue;
    l->vec = vec;
    return;
  }
  if (g_nmlinks >= WC_MANY_LINKS) {
    Arena *now = wc_arena_get();
    i32 keep = 0;
    for (i32 i = 0; i < g_nmlinks; i++)
      if (g_mlinks[i].epoch == now) g_mlinks[keep++] = g_mlinks[i];
    g_nmlinks = keep;
    if (g_nmlinks >= WC_MANY_LINKS) {
      wc_limit_once(&g_nmlinks, "relation dropped: more than a page's worth of collections",
                    "WC_MANY_LINKS", "relation");
      return;
    }
  }
  g_mlinks[g_nmlinks++] = (ManyLink){owner, pk, field, vec, epoch};
}

void *wc_many_vec_new(const ManyRule *rule, Arena *a) {
  const WVecDesc *vd = rule->vec;
  if (!vd) return nullptr;
  if (!a) {
    // no request arena: a test or a CLI command. The collection then lives as long as the
    // process, which is what "no request" means.
    static Arena *fallback;
    if (!fallback) fallback = arena_new(16 * 1024);
    a = fallback;
  }
  void *v = arena_zalloc(a, vd->size ? vd->size : 1);
  if (!v) return nullptr;
  *(const WVecDesc **)v = vd; // the descriptor lives inside the collection
  if (vd->off_arena) *(Arena **)((char *)v + vd->off_arena) = a;
  return v;
}

void *wc_refs(const WModel *m, const void *row, const char *field) {
  if (!m || !row || !field) return nullptr;
  const ManyRule *rule = wc_many_rule(m->name, field);
  if (!rule) {
    wc_panic(__FILE__, __LINE__,
             "refs: %s.%s is not a declared collection. Add WEBC_MODEL_MANY(%s, %s, Target, "
             "back_field) next to the models.",
             m->name, field, m->name, field);
    return nullptr;
  }
  const WField *pkf = nullptr;
  for (i32 i = 0; i < m->n; i++)
    if (m->fields[i].q.pk) pkf = &m->fields[i];
  if (!pkf) return wc_many_vec_new(rule, wc_arena_get());
  i64 pk = wfield_get_i64(pkf, row);
  // The epoch is the arena the rows live in, and it may be null ("no request arena": a test,
  // a CLI command) exactly as it is for `ref()`'s links — the two must agree, or a prefetch
  // and the read that follows it look at different tables.
  Arena *epoch = t_ref_epoch ? t_ref_epoch : wc_arena_get();
  for (i32 i = g_nmlinks - 1; i >= 0; i--) {
    const ManyLink *l = &g_mlinks[i];
    if (l->epoch != epoch || l->pk != pk) continue;
    if (strcmp(l->field, field) != 0 || strcmp(l->owner, m->name) != 0) continue;
    return l->vec;
  }
  many_warn_once(m->name, field, epoch);
  return wc_many_vec_new(rule, epoch);
}

void *wc_many_vec(const char *owner, i64 pk, const char *field, Arena *epoch) {
  for (i32 i = g_nmlinks - 1; i >= 0; i--) {
    const ManyLink *l = &g_mlinks[i];
    if (l->epoch != epoch || l->pk != pk) continue;
    if (strcmp(l->field, field) != 0 || strcmp(l->owner, owner) != 0) continue;
    return l->vec;
  }
  return nullptr;
}
