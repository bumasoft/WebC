// SPDX-License-Identifier: Apache-2.0
// WebC — router: segment radix tree, static > :param > *wildcard precedence.
#include <webc/http.h>

typedef enum RMethod : u8 {
  M_GET,
  M_POST,
  M_PUT,
  M_PATCH,
  M_DELETE,
  M_HEAD,
  M_OPTIONS,
  M_ANY,
  M_COUNT
} RMethod;

typedef struct RNode {
  const char *seg;
  Handler h[M_COUNT];
  WMiddleware *mws[M_COUNT];
  i32 nmws[M_COUNT];
  const char *pattern[M_COUNT];
  struct RNode **kids;
  i32 nkids, cap;
} RNode;

struct WRouter {
  Arena *a;
  RNode *root;
  i32 count;
  // Named routes: the pattern is the one place a URL is spelled, and a name is how
  // the rest of the program asks for it (`url_to("story", "slug", st->slug)`).
  // Held flat and separate from the tree: a name is not part of matching.
  struct WNamedRoute {
    const char *name, *method, *pattern;
  } *named;
  i32 nnamed, capnamed;
};

void router_name(WRouter *r, const char *name, const char *method, const char *pattern) {
  if (!r || !name) return;
  for (i32 i = 0; i < r->nnamed; i++)
    if (strcmp(r->named[i].name, name) == 0) return; // one name, one pattern
  if (r->nnamed == r->capnamed) {
    i32 ncap = r->capnamed ? r->capnamed * 2 : 16;
    void *n = arena_alloc(r->a, (size_t)ncap * sizeof *r->named);
    if (r->named) memcpy(n, r->named, (size_t)r->nnamed * sizeof *r->named);
    r->named = n;
    r->capnamed = ncap;
  }
  r->named[r->nnamed++] = (struct WNamedRoute){arena_strdup(r->a, name), method,
                                               arena_strdup(r->a, pattern)};
}

const char *router_pattern_for(WRouter *r, str name) {
  if (!r) return nullptr;
  for (i32 i = 0; i < r->nnamed; i++)
    if (str_eq_c(name, r->named[i].name)) return r->named[i].pattern;
  return nullptr;
}

i32 router_named_count(WRouter *r) { return r ? r->nnamed : 0; }

void router_named_dump(WRouter *r, FILE *out) {
  if (!r) return;
  for (i32 i = 0; i < r->nnamed; i++)
    fprintf(out, "  %-7s %-32s %s\n", r->named[i].method, r->named[i].pattern,
            r->named[i].name);
}

static i64 method_index(str m) {
  if (str_eq_c(m, "GET")) return M_GET;
  if (str_eq_c(m, "POST")) return M_POST;
  if (str_eq_c(m, "PUT")) return M_PUT;
  if (str_eq_c(m, "PATCH")) return M_PATCH;
  if (str_eq_c(m, "DELETE")) return M_DELETE;
  if (str_eq_c(m, "HEAD")) return M_HEAD;
  if (str_eq_c(m, "OPTIONS")) return M_OPTIONS;
  if (str_eq_c(m, "*")) return M_ANY;
  return -1;
}

static RNode *node_new(WRouter *r, const char *seg) {
  RNode *n = arena_zalloc(r->a, sizeof *n);
  n->seg = seg;
  return n;
}

static RNode *child_get(WRouter *r, RNode *n, const char *seg) {
  for (i32 i = 0; i < n->nkids; i++)
    if (strcmp(n->kids[i]->seg, seg) == 0) return n->kids[i];
  if (n->nkids == n->cap) {
    i32 ncap = n->cap ? n->cap * 2 : 4;
    RNode **nk = arena_alloc(r->a, (size_t)ncap * sizeof(RNode *));
    if (n->kids) memcpy(nk, n->kids, (size_t)n->nkids * sizeof(RNode *));
    n->kids = nk;
    n->cap = ncap;
  }
  RNode *c = node_new(r, seg);
  n->kids[n->nkids++] = c;
  return c;
}

WRouter *router_new(Arena *a) {
  WRouter *r = arena_zalloc(a, sizeof *r);
  r->a = a;
  r->root = node_new(r, "");
  return r;
}

void router_insert(WRouter *r, const char *method, const char *pattern, Handler h, WMiddleware *mws,
                   i32 nmws) {
  i64 mi = method_index(str_lit(method));
  if (mi < 0) wc_panic(__FILE__, __LINE__, "unknown method '%s'", method);
  RNode *n = r->root;
  const char *p = pattern;
  if (*p == '/') p++;
  while (*p) {
    const char *slash = strchr(p, '/');
    size_t len = slash ? (size_t)(slash - p) : strlen(p);
    if (len == 0) break;
    char *seg = arena_alloc(r->a, len + 1);
    memcpy(seg, p, len);
    seg[len] = 0;
    if (seg[0] == '*') {
      // a wildcard swallows the rest of the path
      n = child_get(r, n, seg);
      n->pattern[mi] = arena_strdup(r->a, pattern);
      n->h[mi] = h;
      n->mws[mi] = mws;
      n->nmws[mi] = nmws;
      r->count++;
      return;
    }
    n = child_get(r, n, seg);
    p += len;
    if (*p == '/') p++;
  }
  n->h[mi] = h;
  n->mws[mi] = mws;
  n->nmws[mi] = nmws;
  n->pattern[mi] = arena_strdup(r->a, pattern);
  r->count++;
}

static bool match_here(RNode *n, str *segs, i32 nseg, i32 i, i64 mi, WMatch *m);
static bool probe(WRouter *r, str *segs, i32 nseg, i64 mi);

static bool finish(RNode *n, i64 mi, WMatch *m, str rest) {
  (void)rest;
  i64 idx = mi;
  if (!n->h[idx]) {
    if (idx == M_HEAD && n->h[M_GET])
      idx = M_GET;
    else if (n->h[M_ANY])
      idx = M_ANY;
    else
      return false;
  }
  m->found = true;
  m->handler = n->h[idx];
  m->mws = n->mws[idx];
  m->nmws = n->nmws[idx];
  m->pattern = str_lit(n->pattern[idx] ? n->pattern[idx] : "");
  return true;
}

static bool match_here(RNode *n, str *segs, i32 nseg, i32 i, i64 mi, WMatch *m) {
  if (i == nseg) return finish(n, mi, m, (str){"", 0});
  str seg = segs[i];
  // 1. static children win
  for (i32 k = 0; k < n->nkids; k++) {
    RNode *c = n->kids[k];
    if (c->seg[0] == ':' || c->seg[0] == '*') continue;
    if (str_eq_c(seg, c->seg) && match_here(c, segs, nseg, i + 1, mi, m)) return true;
  }
  // 2. parameters
  for (i32 k = 0; k < n->nkids; k++) {
    RNode *c = n->kids[k];
    if (c->seg[0] != ':') continue;
    if (m->nparams < (i32)WC_COUNTOF(m->param_k)) {
      m->param_k[m->nparams] = str_lit(c->seg + 1);
      m->param_v[m->nparams] = seg;
      m->nparams++;
      if (match_here(c, segs, nseg, i + 1, mi, m)) return true;
      m->nparams--;
    }
  }
  // 3. wildcard: rest of the path, slashes included
  for (i32 k = 0; k < n->nkids; k++) {
    RNode *c = n->kids[k];
    if (c->seg[0] != '*') continue;
    str rest = segs[i];
    for (i32 j = i + 1; j < nseg; j++) {
      rest.n = (size_t)(segs[j].p + segs[j].n - rest.p);
    }
    if (m->nparams < (i32)WC_COUNTOF(m->param_k)) {
      m->param_k[m->nparams] = str_lit(c->seg + 1);
      m->param_v[m->nparams] = rest;
      m->nparams++;
      if (finish(c, mi, m, rest)) return true;
      m->nparams--;
    }
    return false;
  }
  return false;
}

i32 router_count(WRouter *r) {
  return r ? r->count : 0;
}

WMatch router_match(WRouter *r, str method, str path) {
  WMatch m = {0};
  i64 mi = method_index(method);
  if (mi < 0) mi = M_ANY;
  // split into segments
  str segs[64];
  i32 nseg = 0;
  const char *p = path.p;
  const char *end = path.p + path.n;
  if (p < end && *p == '/') p++;
  while (p < end && nseg < (i32)WC_COUNTOF(segs)) {
    const char *slash = memchr(p, '/', (size_t)(end - p));
    const char *seg_end = slash ? slash : end;
    segs[nseg++] = (str){p, (size_t)(seg_end - p)};
    p = slash ? slash + 1 : end;
  }
  // a single trailing slash is not a different route
  if (nseg > 0 && segs[nseg - 1].n == 0) nseg--;
  bool found = match_here(r->root, segs, nseg, 0, mi, &m);
  if (!found) {
    // distinguish "method not allowed" from "no such path"
    for (i64 k = 0; k < M_COUNT; k++) {
      if (k == mi || k == M_ANY) continue;
      if (probe(r, segs, nseg, k)) {
        m.method_not_allowed = true;
        break;
      }
    }
  }
  return m;
}

static bool probe(WRouter *r, str *segs, i32 nseg, i64 mi) {
  WMatch out = {0};
  return match_here(r->root, segs, nseg, 0, mi, &out);
}