// SPDX-License-Identifier: Apache-2.0
// WebC — `webc lsp`: JSON-RPC over stdio, and the four modes that make it testable.
//
// Transport rules this file keeps, because LSP servers that break them are the ones
// editors distrust:
//   * one reader, one dispatcher; a response is written only after the request's
//     work is finished, so an answer is never out of order;
//   * `Content-Length` is bytes, and the separator is tolerated either way;
//   * a request for a feature the client did not advertise is still answered — with
//     the truth, not with a guess;
//   * a notification never produces a response, and an unknown notification is
//     ignored rather than answered (an editor that adds one must not break us).
//
// And the three modes the spec put in the gate:
//   --selftest   a scripted session with assertions, run by `make lsp`
//   --replay     a recorded JSONL session, diffed exactly (the protocol's api.txt)
//   --trace      every message, its bytes and its milliseconds, as JSONL
#include "lspx.h"
#include "prefix.h"
#include "ceng.h"

#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------- URIs

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

char *lsp_path_of_uri(Arena *a, const char *uri) {
  const char *p = strstr(uri, "file://");
  if (!p) return arena_strdup(a, uri);
  p += 7;
  // file:///Users/... → the authority is empty, the path keeps its leading slash
  Buf b;
  buf_init(&b, a, 256);
  while (*p) {
    if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
      buf_putc(&b, (char)(hexval(p[1]) * 16 + hexval(p[2])));
      p += 3;
      continue;
    }
    buf_putc(&b, *p++);
  }
  return buf_cstr(&b);
}

char *lsp_uri_of_path(Arena *a, const char *path) {
  Buf b;
  buf_init(&b, a, 256);
  buf_puts(&b, "file://");
  const char *p = path;
  // a relative path is resolved against the cwd once, so two spellings of the same
  // file are one document
  char abs[1024];
  if (p[0] != '/') {
    char cwd[768];
    if (getcwd(cwd, sizeof cwd)) {
      snprintf(abs, sizeof abs, "%s/%s", cwd, p);
      p = abs;
    }
  }
  if (*p != '/') buf_putc(&b, '/');
  for (; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c == ' ' || c == '%' || c == '#' || c == '?') buf_printf(&b, "%%%02X", c);
    else buf_putc(&b, (char)c);
  }
  return buf_cstr(&b);
}

// ---------------------------------------------------------------- documents

static WDoc *doc_new(WLsp *L, const char *path) {
  if (L->ndoc == L->capdoc) {
    i32 ncap = L->capdoc ? L->capdoc * 2 : 8;
    WDoc *q = arena_alloc(L->a, sizeof(WDoc) * (size_t)ncap);
    if (L->doc) memcpy(q, L->doc, sizeof(WDoc) * (size_t)L->ndoc);
    L->doc = q;
    L->capdoc = ncap;
  }
  WDoc *d = &L->doc[L->ndoc++];
  memset(d, 0, sizeof *d);
  d->path = arena_strdup(L->a, path);
  d->uri = lsp_uri_of_path(L->a, path);
  d->a = arena_new(64 * 1024);
  return d;
}

WDoc *lsp_doc_find(WLsp *L, const char *uri) {
  for (i32 i = 0; i < L->ndoc; i++)
    if (!strcmp(L->doc[i].uri, uri)) return &L->doc[i];
  return nullptr;
}

void lsp_doc_reindex(WLsp *L, WDoc *d) {
  (void)L;
  d->src = dsl_src(d->a, d->text);
  d->scan = dsl_scan(d->a, d->src);
}

void lsp_doc_set_text(WDoc *d, str text) {
  // The old text points into `d->a`, which is about to be rewound: copy it out
  // first, or the tokenizer reads the buffer it is writing into.
  char *tmp = (char *)malloc(text.n + 1);
  if (text.n) memcpy(tmp, text.p, text.n);
  tmp[text.n] = 0;
  dsl_scan_free(d->scan);
  dsl_src_free(d->src);
  d->scan = nullptr;
  d->src = nullptr;
  arena_reset(d->a);
  d->text = (str){arena_memdup(d->a, tmp, text.n), text.n};
  free(tmp);
  lsp_doc_reindex(nullptr, d);
}

WDoc *lsp_doc_open(WLsp *L, const char *path, const char *uri, i32 version, str text) {
  char abs[1024];
  const char *use = path;
  char *rp = realpath(path, nullptr);
  if (rp) {
    snprintf(abs, sizeof abs, "%s", rp);
    free(rp);
    use = abs;
  }
  WDoc *d = nullptr;
  for (i32 i = 0; i < L->ndoc; i++)
    if (!strcmp(L->doc[i].path, use)) d = &L->doc[i];
  if (!d) {
    d = doc_new(L, use);
    d->uri = lsp_uri_of_path(L->a, use);
    (void)uri;
  }
  d->version = version;
  d->open = true;
  lsp_doc_set_text(d, text);
  // The index must see the buffer, not the file on disk: an unsaved edit is the
  // state the editor is asking about.
  facts_refresh(L->F, d->path, d->text);
  return d;
}

// ---------------------------------------------------------------- output

static void write_msg(WLsp *L, const char *payload, size_t n) {
  if (L->capture) {
    buf_write(&L->out, payload, n);
    buf_putc(&L->out, '\n');
    return;
  }
  fprintf(stdout, "Content-Length: %zu\r\n\r\n", n);
  fwrite(payload, 1, n, stdout);
  fflush(stdout);
}

static void send_result(WLsp *L, const char *id_json, const char *result_json) {
  Buf b;
  buf_init(&b, L->req, 512);
  buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
  buf_puts(&b, id_json);
  buf_puts(&b, ",\"result\":");
  buf_puts(&b, result_json);
  buf_putc(&b, '}');
  L->nresp++;
  write_msg(L, buf_cstr(&b), b.n);
  buf_free(&b);
}

static void send_error(WLsp *L, const char *id_json, i32 code, const char *msg) {
  Buf b;
  buf_init(&b, L->req, 256);
  buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
  buf_puts(&b, id_json);
  buf_printf(&b, ",\"error\":{\"code\":%d,\"message\":", code);
  wj_str(&b, msg);
  buf_puts(&b, "}}");
  L->nresp++;
  write_msg(L, buf_cstr(&b), b.n);
  buf_free(&b);
}

static void send_note(WLsp *L, const char *method, const char *params_json) {
  Buf b;
  buf_init(&b, L->req, 512);
  buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"method\":");
  wj_str(&b, method);
  buf_puts(&b, ",\"params\":");
  buf_puts(&b, params_json);
  buf_putc(&b, '}');
  L->nresp++;
  write_msg(L, buf_cstr(&b), b.n);
  buf_free(&b);
}

// ---------------------------------------------------------------- diagnostics

static void publish(WLsp *L, WDoc *d) {
  Buf diags;
  buf_init(&diags, L->req, 1024);
  lspx_diagnostics(L, d, &diags);
  Buf params;
  buf_init(&params, L->req, 1024);
  buf_puts(&params, "{\"uri\":");
  wj_str(&params, d->uri);
  buf_printf(&params, ",\"version\":%d,\"diagnostics\":", d->version);
  buf_puts(&params, buf_cstr(&diags));
  buf_putc(&params, '}');
  send_note(L, "textDocument/publishDiagnostics", buf_cstr(&params));
  buf_free(&diags);
  buf_free(&params);
}

// ---------------------------------------------------------------- dispatch

static const char *id_of(JVal *msg, Buf *scratch) {
  JVal *id = json_get(msg, "id");
  if (!id) return nullptr;
  if (id->kind == J_STR) {
    buf_reset(scratch);
    wj_strn(scratch, id->s.p, id->s.n);
    return buf_cstr(scratch);
  }
  if (id->kind == J_NUM) {
    buf_reset(scratch);
    buf_puti64(scratch, id->inum);
    return buf_cstr(scratch);
  }
  return nullptr;
}

static i32 pos_line(JVal *p) {
  JVal *pos = json_get(p, "position");
  JVal *l = pos ? json_get(pos, "line") : nullptr;
  return (l && l->kind == J_NUM) ? (i32)l->inum : 0;
}
static i32 pos_char(JVal *p) {
  JVal *pos = json_get(p, "position");
  JVal *c = pos ? json_get(pos, "character") : nullptr;
  return (c && c->kind == J_NUM) ? (i32)c->inum : 0;
}

static void handle(WLsp *L, JVal *msg) {
  Buf scratch;
  buf_init(&scratch, L->req, 128);
  const char *id = id_of(msg, &scratch);
  JVal *m = json_get(msg, "method");
  if (!m || m->kind != J_STR) {
    if (id) send_error(L, id, -32600, "not a JSON-RPC request");
    return;
  }
  char method[96];
  snprintf(method, sizeof method, "%.*s", (int)(m->s.n < 95 ? m->s.n : 95), m->s.p);
  JVal *p = json_get(msg, "params");
  L->nreq++;

  if (!strcmp(method, "initialize")) {
    // The root comes from the editor when it has one: a workspace folder beats the
    // working directory the process happened to inherit.
    JVal *folders = p ? json_get(p, "workspaceFolders") : nullptr;
    const char *root = nullptr;
    if (folders && folders->kind == J_ARR && folders->n > 0) {
      JVal *u = json_get(folders->items[0], "uri");
      if (u && u->kind == J_STR) {
        char *path = lsp_path_of_uri(L->a, str_cstr(L->req, u->s));
        root = path;
      }
    }
    if (root && root[0] && strcmp(root, L->root)) {
      snprintf(L->root, sizeof L->root, "%s", root);
      L->F = facts_new(L->a, L->root);
    }
    // The other tools deserve the same view of the project: `.webc/compile_commands.json`
    // is what makes clangd agree with `webc build` instead of guessing.
    ceng_write_compile_commands(L);
    const char *caps =
        "{\"capabilities\":{"
        "\"positionEncoding\":\"utf-16\","
        "\"textDocumentSync\":{\"openClose\":true,\"change\":1,\"save\":{\"includeText\":true}},"
        "\"hoverProvider\":true,"
        "\"definitionProvider\":true,"
        "\"referencesProvider\":true,"
        "\"documentSymbolProvider\":true,"
        "\"workspaceSymbolProvider\":true,"
        "\"codeActionProvider\":{\"codeActionKinds\":[\"quickfix\"]},"
        "\"completionProvider\":{\"triggerCharacters\":[\"(\",\"\\\"\",\"<\",\".\"]},"
        "\"workspace\":{\"workspaceFolders\":{\"supported\":true,\"changeNotifications\":false}}"
        "},\"serverInfo\":{\"name\":\"webc\",\"version\":\"" WEBC_VERSION "\"}}";
    send_result(L, id ? id : "null", caps);
    return;
  }
  if (!strcmp(method, "shutdown")) {
    send_result(L, id ? id : "null", "null");
    return;
  }
  if (!strcmp(method, "exit")) {
    L->nreq--; // an exit is a notification
    exit(0);
  }
  if (!strcmp(method, "initialized") || !strncmp(method, "$/", 2) ||
      !strcmp(method, "workspace/didChangeConfiguration") ||
      !strcmp(method, "workspace/didChangeWatchedFiles") ||
      !strcmp(method, "workspace/didChangeWorkspaceFolders") ||
      !strcmp(method, "textDocument/didSave")) {
    // didSave may arrive with text; if it does, the buffer is the truth.
    JVal *tp = p ? json_get(p, "textDocument") : nullptr;
    JVal *u = tp ? json_get(tp, "uri") : nullptr;
    if (!strcmp(method, "textDocument/didSave") && u && u->kind == J_STR) {
      WDoc *d = lsp_doc_find(L, str_cstr(L->req, u->s));
      if (d) {
        JVal *txt = json_get(p, "text");
        if (txt && txt->kind == J_STR) {
          lsp_doc_set_text(d, txt->s);
          facts_refresh(L->F, d->path, d->text);
        }
        L->saving = true;
        publish(L, d);
        L->saving = false;
      }
    }
    return;
  }

  if (!strcmp(method, "textDocument/didOpen")) {
    JVal *td = p ? json_get(p, "textDocument") : nullptr;
    if (!td) return;
    JVal *u = json_get(td, "uri");
    JVal *t = json_get(td, "text");
    JVal *v = json_get(td, "version");
    if (!u || u->kind != J_STR || !t || t->kind != J_STR) return;
    char *path = lsp_path_of_uri(L->req, str_cstr(L->req, u->s));
    WDoc *d = lsp_doc_open(L, path, str_cstr(L->req, u->s),
                           v && v->kind == J_NUM ? (i32)v->inum : 0, t->s);
    L->saving = true; // an open is a save as far as the compiler's budget is concerned
    publish(L, d);
    L->saving = false;
    return;
  }
  if (!strcmp(method, "textDocument/didChange")) {
    JVal *td = p ? json_get(p, "textDocument") : nullptr;
    JVal *u = td ? json_get(td, "uri") : nullptr;
    JVal *ch = json_get(p, "contentChanges");
    if (!u || u->kind != J_STR || !ch || ch->kind != J_ARR || ch->n == 0) return;
    WDoc *d = lsp_doc_find(L, str_cstr(L->req, u->s));
    if (!d) return;
    JVal *txt = json_get(ch->items[ch->n - 1], "text");
    if (!txt || txt->kind != J_STR) return;
    JVal *v = json_get(td, "version");
    d->version = v && v->kind == J_NUM ? (i32)v->inum : d->version + 1;
    lsp_doc_set_text(d, txt->s);
    facts_refresh(L->F, d->path, d->text);
    publish(L, d);
    return;
  }
  if (!strcmp(method, "textDocument/didClose")) {
    JVal *td = p ? json_get(p, "textDocument") : nullptr;
    JVal *u = td ? json_get(td, "uri") : nullptr;
    if (!u || u->kind != J_STR) return;
    WDoc *d = lsp_doc_find(L, str_cstr(L->req, u->s));
    if (d) {
      d->open = false;
      // the file's truth returns to whatever is on disk
      char *path = d->path;
      Arena *a = arena_new(16 * 1024);
      FILE *f = fopen(path, "rb");
      if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = arena_alloc(a, (size_t)(n < 0 ? 0 : n) + 1);
        size_t got = fread(buf, 1, (size_t)(n < 0 ? 0 : n), f);
        fclose(f);
        buf[got] = 0;
        lsp_doc_set_text(d, str_mem(buf, got));
        facts_refresh(L->F, d->path, d->text);
      }
      arena_destroy(a);
      Buf empty;
      buf_init(&empty, L->req, 256);
      buf_puts(&empty, "{\"uri\":");
      wj_str(&empty, d->uri);
      buf_puts(&empty, ",\"diagnostics\":[]}");
      send_note(L, "textDocument/publishDiagnostics", buf_cstr(&empty));
      buf_free(&empty);
    }
    return;
  }

  // ---- position-shaped features
  if (!strcmp(method, "textDocument/hover") || !strcmp(method, "textDocument/definition") ||
      !strcmp(method, "textDocument/references") || !strcmp(method, "textDocument/completion") ||
      !strcmp(method, "textDocument/documentSymbol") || !strcmp(method, "textDocument/codeAction")) {
    JVal *td = p ? json_get(p, "textDocument") : nullptr;
    JVal *u = td ? json_get(td, "uri") : nullptr;
    if (!u || u->kind != J_STR) {
      send_error(L, id ? id : "null", -32602, "no textDocument.uri");
      return;
    }
    WDoc *d = lsp_doc_find(L, str_cstr(L->req, u->s));
    if (!d) {
      // a request about a document we were never told about: read it from disk once
      char *path = lsp_path_of_uri(L->req, str_cstr(L->req, u->s));
      Arena *a = arena_new(16 * 1024);
      FILE *f = fopen(path, "rb");
      str text = str_lit("");
      if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = arena_alloc(a, (size_t)(n < 0 ? 0 : n) + 1);
        size_t got = fread(buf, 1, (size_t)(n < 0 ? 0 : n), f);
        fclose(f);
        buf[got] = 0;
        text = str_mem(buf, got);
      }
      d = lsp_doc_open(L, path, str_cstr(L->req, u->s), 0, text);
      arena_destroy(a);
    }
    u32 off = lspx_off(d, pos_line(p), pos_char(p));
    Buf out;
    buf_init(&out, L->req, 2048);
    if (!strcmp(method, "textDocument/hover")) lspx_hover(L, d, off, &out);
    else if (!strcmp(method, "textDocument/definition")) lspx_definition(L, d, off, &out);
    else if (!strcmp(method, "textDocument/references")) lspx_references(L, d, off, &out);
    else if (!strcmp(method, "textDocument/completion")) lspx_completion(L, d, off, &out);
    else if (!strcmp(method, "textDocument/documentSymbol")) lspx_symbols(L, d, &out);
    else {
      JVal *rg = json_get(p, "range");
      u32 b = off, e = off;
      if (rg) {
        JVal *s0 = json_get(rg, "start"), *e0 = json_get(rg, "end");
        if (s0 && e0) {
          b = lspx_off(d, (i32)json_get(s0, "line")->inum, (i32)json_get(s0, "character")->inum);
          e = lspx_off(d, (i32)json_get(e0, "line")->inum, (i32)json_get(e0, "character")->inum);
        }
      }
      lspx_actions(L, d, b, e, &out);
    }
    send_result(L, id ? id : "null", buf_cstr(&out));
    buf_free(&out);
    return;
  }

  if (!strcmp(method, "workspace/symbol")) {
    // Project-wide: models and views, which is what a person searches for by name.
    JVal *q = json_get(p, "query");
    char needle[64] = {0};
    if (q && q->kind == J_STR) snprintf(needle, sizeof needle, "%.*s", (int)q->s.n, q->s.p);
    Buf out;
    buf_init(&out, L->req, 1024);
    buf_putc(&out, '[');
    i32 n = 0;
    for (i32 i = 0; i < L->F->nmod; i++) {
      WMod *m = &L->F->mod[i];
      if (needle[0] && !strstr(m->name, needle)) continue;
      char *uri = lsp_uri_of_path(L->req, facts_file_path(L->F, m->file));
      if (n++) buf_putc(&out, ',');
      buf_puts(&out, "{\"name\":");
      wj_str(&out, m->name);
      buf_printf(&out, ",\"kind\":23,\"location\":{\"uri\":");
      wj_str(&out, uri);
      buf_printf(&out, ",\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,"
                       "\"character\":%d}}}}",
                 m->line, m->col, m->line, m->col + 1);
    }
    for (i32 i = 0; i < L->F->nview; i++) {
      WSym *v = &L->F->view[i];
      if (needle[0] && !strstr(v->name, needle)) continue;
      char *uri = lsp_uri_of_path(L->req, facts_file_path(L->F, v->file));
      if (n++) buf_putc(&out, ',');
      buf_puts(&out, "{\"name\":");
      wj_str(&out, v->name);
      buf_printf(&out, ",\"kind\":12,\"location\":{\"uri\":");
      wj_str(&out, uri);
      buf_printf(&out, ",\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,"
                       "\"character\":%d}}}}",
                 v->line, v->col, v->line, v->col + 1);
    }
    buf_putc(&out, ']');
    send_result(L, id ? id : "null", buf_cstr(&out));
    buf_free(&out);
    return;
  }

  if (id) send_error(L, id, -32601, "webc lsp: this server does not implement that method");
}

// ---------------------------------------------------------------- framing

// Read one message body. Returns false at EOF. `cap` is grown by the caller's arena.
static bool read_msg(Buf *body) {
  char line[512];
  size_t len = 0;
  bool have_len = false;
  for (;;) {
    if (!fgets(line, sizeof line, stdin)) return false;
    if (!strcmp(line, "\r\n") || !strcmp(line, "\n")) break;
    if (!strncmp(line, "Content-Length:", 15)) {
      len = (size_t)strtoul(line + 15, nullptr, 10);
      have_len = true;
    }
  }
  if (!have_len) {
    // A stream of bare JSON lines is not LSP, but it is what a shell script does:
    // accept it rather than hanging, and say so once on stderr.
    static bool warned;
    if (!warned) {
      fprintf(stderr, "webc lsp: no Content-Length header; accepting JSON lines\n");
      warned = true;
    }
    if (!fgets(line, sizeof line, stdin)) return false;
    buf_puts(body, line);
    return true;
  }
  buf_reset(body);
  buf_reserve(body, len + 1);
  size_t got = fread(body->p, 1, len, stdin);
  body->n = got;
  body->p[got] = 0;
  return got > 0;
}

// ---------------------------------------------------------------- modes

static void trace_msg(WLsp *L, const char *dir, const char *payload, size_t n, i64 us) {
  if (!L->trace) return;
  fprintf(L->trace, "{\"t\":%lld,\"dir\":\"%s\",\"bytes\":%zu,\"us\":%lld,\"msg\":",
          (long long)(wc_time_ms() - L->t_start), dir, n, (long long)us);
  char *esc = str_cstr(L->req, json_encode_str(L->req, str_mem(payload, n)));
  fprintf(L->trace, "%s}\n", esc);
}

// Render one incoming message through the dispatcher and return what it answered.
static void run_one(WLsp *L, const char *json, size_t n, Buf *captured) {
  L->capture = captured != nullptr;
  if (L->capture) buf_reset(&L->out);
  Arena *req_old = L->req;
  L->req = arena_new(64 * 1024);
  JParse jp = json_parse(L->req, str_mem(json, n));
  if (!jp.ok) {
    fprintf(stderr, "webc lsp: not JSON: %.*s\n", (int)(n > 200 ? 200 : n), json);
  } else {
    handle(L, jp.v);
  }
  if (captured) buf_write(captured, L->out.p ? L->out.p : "", L->out.n);
  L->capture = false;
  arena_destroy(L->req);
  L->req = req_old;
}

static char *slurp_file(Arena *a, const char *path, size_t *n_out) {
  FILE *f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = arena_alloc(a, (size_t)(n < 0 ? 0 : n) + 1);
  size_t got = fread(buf, 1, (size_t)(n < 0 ? 0 : n), f);
  fclose(f);
  buf[got] = 0;
  if (n_out) *n_out = got;
  return buf;
}

// ---------------------------------------------------------------- selftest

static i32 g_self_fail;
static void check(bool ok, const char *what) {
  printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) g_self_fail++;
}
static bool has(const char *hay, const char *needle) {
  return hay && needle && strstr(hay, needle) != nullptr;
}


// Open a buffer the way an editor does, and return what the server published.
static const char *open_buf(WLsp *L, const char *path, i32 ver, const char *text, Buf *cap) {
  char uri[1200];
  snprintf(uri, sizeof uri, "file://%s", path);
  Buf m;
  buf_init(&m, L->req, 8192);
  buf_printf(&m,
             "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":"
             "{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"c\",\"version\":%d,\"text\":",
             uri, ver);
  wj_str(&m, text);
  buf_puts(&m, "}}}");
  buf_reset(cap);
  run_one(L, buf_cstr(&m), m.n, cap);
  buf_free(&m);
  return buf_cstr(cap);
}

// Position of `needle` in `text`, as an editor would send it.
static void loc_of(const char *text, const char *needle, i32 *line, i32 *col) {
  const char *at = strstr(text, needle);
  i32 l = 0, c = 0;
  if (!at) at = text;
  for (const char *p = text; p < at; p++) {
    if (*p == '\n') {
      l++;
      c = 0;
    } else {
      c++;
    }
  }
  *line = l;
  *col = c;
}

static int selftest(const char *root) {
  printf("\n\033[1m  webc lsp --selftest\033[0m  root=%s\n\n", root);
  WLsp L;
  memset(&L, 0, sizeof L);
  L.a = arena_new(256 * 1024);
  L.req = arena_new(256 * 1024);
  buf_init(&L.out, L.a, 4096);
  snprintf(L.root, sizeof L.root, "%s", root);
  L.t_start = wc_time_ms();
  L.F = facts_new(L.a, root);

  // A project to talk about: the todo example when it is there, else the root's own
  // sources. The self-test is about the server, not about a fixture in a temp dir.
  char proj[1024];
  snprintf(proj, sizeof proj, "%s/examples/todo", root);
  struct stat st;
  if (stat(proj, &st) != 0) {
    printf("  --   no examples/todo under %s; testing the index as it is\n\n", root);
    check(L.F->nfile > 0, "the index found files");
    return g_self_fail ? 1 : 0;
  }
  L.F = facts_new(L.a, proj);
  check(L.F->nfile >= 3, "the index found the project's sources and headers");
  check(L.F->nmod == 1 && !strcmp(L.F->mod[0].name, "Todo"), "a model in a header is indexed");
  check(L.F->nview == 1 && !strcmp(L.F->view[0].name, "Todos"), "a view is indexed");
  check(L.F->nroute >= 6, "routes are indexed");
  bool page_route = false;
  for (i32 i = 0; i < L.F->nroute; i++)
    if (L.F->route[i].view && !strcmp(L.F->route[i].view, "Todos")) page_route = true;
  check(page_route, "a route knows the view it returns");

  char view[1200], server[1200], vuri[1300];
  snprintf(view, sizeof view, "%s/src/view.c", proj);
  snprintf(server, sizeof server, "%s/src/server.c", proj);
  snprintf(vuri, sizeof vuri, "file://%s", view);

  const char *vtxt = slurp_file(L.req, view, nullptr);
  if (!vtxt) {
    printf("  FAIL  cannot read %s\n", view);
    return 1;
  }
  // ---- didOpen publishes diagnostics; a clean file has none
  Buf m;
  buf_init(&m, L.req, 8192);
  buf_printf(&m, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":"
                 "{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"c\",\"version\":1,"
                 "\"text\":",
            vuri);
  wj_str(&m, vtxt);
  buf_puts(&m, "}}}");
  Buf cap;
  buf_init(&cap, L.req, 4096);
  run_one(&L, buf_cstr(&m), m.n, &cap);
  check(has(buf_cstr(&cap), "publishDiagnostics"), "didOpen publishes diagnostics");
  check(has(buf_cstr(&cap), "\"diagnostics\":[]"), "the todo view has no diagnostics");

  // ---- completion inside el(" → tags
  u32 off = 0;
  const char *el = strstr(vtxt, "el(\"");
  if (el) off = (u32)(el - vtxt) + 4;
  buf_reset(&m);
  buf_printf(&m,
             "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/completion\",\"params\":"
             "{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%d,\"character\":%d}}}",
             vuri, 0, 0);
  {
    // compute line/char of `off` by hand: the position must be what an editor sends
    i32 line = 0, col = 0;
    for (u32 i = 0; i < off; i++) {
      if (vtxt[i] == '\n') {
        line++;
        col = 0;
      } else {
        col++;
      }
    }
    buf_reset(&m);
    buf_printf(
        &m,
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/completion\",\"params\":"
        "{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%d,\"character\":%d}}}", vuri,
        line, col);
  }
  buf_reset(&cap);
  run_one(&L, buf_cstr(&m), m.n, &cap);
  check(has(buf_cstr(&cap), "\"label\":\"main\""), "el( completes the element vocabulary");

  // ---- completion of a field kind
  const char *kindpos = strstr(vtxt, "");
  (void)kindpos;
  {
    i32 line = 0, col = 0;
    u32 off2 = 0;
    const char *vc = nullptr;
    // the model itself lives in todo.h; ask from the view's own file instead: a
    // `page(` argument offers the view names
    vc = strstr(vtxt, "text(\"Todos\")");
    if (vc) off2 = (u32)(vc - vtxt) + 1; // on the name, not inside the string
    for (u32 i = 0; i < off2; i++) {
      if (vtxt[i] == '\n') {
        line++;
        col = 0;
      } else {
        col++;
      }
    }
    buf_reset(&m);
    buf_printf(
        &m,
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/hover\",\"params\":"
        "{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%d,\"character\":%d}}}", vuri,
        line, col);
    buf_reset(&cap);
    run_one(&L, buf_cstr(&m), m.n, &cap);
    check(has(buf_cstr(&cap), "A value, or a format"), "hover on `text` knows the macro");
  }

  // ---- hover on the view macro's own name
  {
    i32 line = 0, col = 0;
    u32 off2 = (u32)(strstr(vtxt, "view(Todos") - vtxt);
    for (u32 i = 0; i < off2; i++) {
      if (vtxt[i] == '\n') {
        line++;
        col = 0;
      } else {
        col++;
      }
    }
    buf_reset(&m);
    buf_printf(
        &m,
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/hover\",\"params\":"
        "{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%d,\"character\":%d}}}", vuri,
        line, col);
    buf_reset(&cap);
    run_one(&L, buf_cstr(&m), m.n, &cap);
    check(has(buf_cstr(&cap), "The body is the view"), "hover on `view` explains the macro");
  }

  // ---- document symbols
  buf_reset(&m);
  buf_printf(&m,
             "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/documentSymbol\",\"params\":"
             "{\"textDocument\":{\"uri\":\"%s\"}}}",
             vuri);
  buf_reset(&cap);
  run_one(&L, buf_cstr(&m), m.n, &cap);
  check(has(buf_cstr(&cap), "\"name\":\"Todos\""), "the outline names the view");
  check(has(buf_cstr(&cap), "short_time"), "the outline finds a plain C helper too");

  // ---- a server macro in a shared file: the half rule, at the caret
  char suri[1300];
  snprintf(suri, sizeof suri, "file://%s", server);
  const char *stxt = slurp_file(L.req, server, nullptr);
  char broken[4096];
  snprintf(broken, sizeof broken, "#include <webc/shared.h>\nGET(\"/nope\") { return 0; }\n");
  buf_reset(&m);
  buf_printf(&m,
             "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":"
             "{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"c\",\"version\":1,\"text\":",
             vuri);
  wj_str(&m, broken);
  buf_puts(&m, "}}}");
  buf_reset(&cap);
  run_one(&L, buf_cstr(&m), m.n, &cap);
  check(has(buf_cstr(&cap), "\"code\":\"half\""),
        "a route macro in a shared file says \"server half\", not \"expected parameter declarator\"");

  // ---- a view that does not exist, with the fix that a person would make
  char pagebuf[1024];
  snprintf(pagebuf, sizeof pagebuf,
           "#include <webc/server.h>\n#include \"app.h\"\nGET(\"/\") { return page(Nope, 0); }\n");
  buf_reset(&m);
  buf_printf(&m,
             "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":"
             "{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"c\",\"version\":2,\"text\":",
             suri);
  wj_str(&m, pagebuf);
  buf_puts(&m, "}}}");
  buf_reset(&cap);
  run_one(&L, buf_cstr(&m), m.n, &cap);
  check(has(buf_cstr(&cap), "\"code\":\"no-view\""), "page(Nope) is a WebC sentence");
  check(has(buf_cstr(&cap), "declare_view(Nope)"), "and it names the fix");

  // ---- code actions come from the diagnostics, not from a second guess
  {
    const char *np = strstr(pagebuf, "Nope");
    i32 line = 2, col = (i32)(np - pagebuf) - 1;
    buf_reset(&m);
    buf_printf(&m,
               "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"textDocument/codeAction\",\"params\":"
               "{\"textDocument\":{\"uri\":\"%s\"},\"range\":{\"start\":{\"line\":%d,"
               "\"character\":0},\"end\":{\"line\":%d,\"character\":99}},\"context\":{"
               "\"diagnostics\":[]}}}",
               suri, line, line);
    buf_reset(&cap);
    run_one(&L, buf_cstr(&m), m.n, &cap);
    check(has(buf_cstr(&cap), "quickfix"), "the diagnostic becomes a quickfix");
    (void)col;
  }

  // ---- an unclosed block is a real error, at the caret, with a one-key fix
  {
    char ub[512];
    snprintf(ub, sizeof ub, "#include <webc/shared.h>\nview(Broken) {\n  el(\"main\") {\n");
    buf_reset(&m);
    buf_printf(&m,
               "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":"
               "{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"c\",\"version\":3,\"text\":",
               vuri);
    wj_str(&m, ub);
    buf_puts(&m, "}}}");
    buf_reset(&cap);
    run_one(&L, buf_cstr(&m), m.n, &cap);
    check(has(buf_cstr(&cap), "unclosed-block"), "an unclosed `el(…) {` is reported");
  }

  // ---- and the file goes back to its own text
  buf_reset(&m);
  buf_printf(&m,
             "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":"
             "{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"c\",\"version\":4,\"text\":",
             vuri);
  wj_str(&m, vtxt);
  buf_puts(&m, "}}}");
  buf_reset(&cap);
  run_one(&L, buf_cstr(&m), m.n, &cap);
  check(has(buf_cstr(&cap), "\"diagnostics\":[]"), "restoring the text clears them");
  (void)stxt;
  (void)off;


  // ---- editing, and the reverse direction: who else mentions this name
  {
    Buf cap2;
    buf_init(&cap2, L.req, 4096);
    Buf eb;
    buf_init(&eb, L.req, 4096);
    buf_printf(&eb, "%s\nview(Second, VecTodo *v) { el(\"p\") { text(\"x\"); } }\n", vtxt);
    open_buf(&L, view, 20, buf_cstr(&eb), &cap2);
    check(has(buf_cstr(&cap2), "\"diagnostics\":[]"), "a second view in the same file is clean");

    buf_reset(&eb);
    buf_printf(&eb, "%s\nview(Broken, VecTodo *v) { el(\"p\") { on_click(nobody); } }\n", vtxt);
    open_buf(&L, view, 21, buf_cstr(&eb), &cap2);
    check(has(buf_cstr(&cap2), "no-action"), "a change of text changes the diagnostics");

    open_buf(&L, view, 22, vtxt, &cap2);
    check(has(buf_cstr(&cap2), "\"diagnostics\":[]"), "and putting it back clears them again");
    buf_free(&eb);

    // the other direction: `Todos` is drawn in view.c, served in server.c, announced
    // in the shared header
    char curbuf[1200];
    snprintf(curbuf, sizeof curbuf, "file://%s", server);
    const char *stxt = slurp_file(L.req, server, nullptr);
    open_buf(&L, server, 23, stxt, &cap2);
    i32 rl, rc;
    loc_of(stxt, "page(Todos", &rl, &rc);
    Buf rm;
    buf_init(&rm, L.req, 2048);
    buf_printf(&rm,
               "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"textDocument/references\",\"params\":"
               "{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%d,\"character\":%d},"
               "\"context\":{\"includeDeclaration\":true}}}",
               curbuf, rl, rc + 5);
    buf_reset(&cap2);
    run_one(&L, buf_cstr(&rm), rm.n, &cap2);
    check(has(buf_cstr(&cap2), "todo.h"), "references on Todos reaches the header it is declared in");
    buf_free(&rm);
    buf_free(&cap2);
  }

  // ---- the four checks that need a project rather than a buffer in isolation
  {
    Buf cap2;
    buf_init(&cap2, L.req, 4096);
    // a model with no Vec: `db_all` asks for the collection by convention
    char nv[1024];
    snprintf(nv, sizeof nv,
             "#include <webc/server.h>\n"
             "WEBC_MODEL(Widget, (id, i64, .pk = true))\n"
             "GET(\"/w\") { return json(200, db_all(Widget)); }\n");
    char nvpath[1200];
    snprintf(nvpath, sizeof nvpath, "%s/src/lsp_scratch.c", proj);
    const char *pub = open_buf(&L, nvpath, 10, nv, &cap2);
    check(has(pub, "\"code\":\"no-vec\""), "db_all(T) without Vec(T) says which line to add");

    // an action nothing declares: a wiring that cannot fire
    char na[512];
    snprintf(na, sizeof na, "#include <webc/shared.h>\nview(V) { el(\"b\") { on_click(nothing_here); } }\n");
    pub = open_buf(&L, nvpath, 11, na, &cap2);
    check(has(pub, "\"code\":\"no-action\""), "on_click(name) with no action(name) is reported");

    // a server-only module in a shared file
    char hm[512];
    snprintf(hm, sizeof hm, "#include <webc/mail.h>\n#include <webc/shared.h>\nview(V2) { el(\"p\") { text(\"x\"); } }\n");
    pub = open_buf(&L, view, 12, hm, &cap2); // a shared source: both halves compile it
    check(has(pub, "\"code\":\"half-module\""), "webc/mail.h in a shared file is a half error");

    // path parameters, from the route the caret sits in
    char route[512];
    snprintf(route, sizeof route,
             "#include <webc/server.h>\n"
             "GET(\"/things/:thing_id\") { i64 id = path_i64(\"\"); return json(200, id); }\n");
    char rpath[1200];
    snprintf(rpath, sizeof rpath, "%s/src/lsp_scratch2.c", proj);
    open_buf(&L, rpath, 13, route, &cap2);
    i32 tl, tc;
    loc_of(route, "path_i64(\"", &tl, &tc);
    Buf cm;
    buf_init(&cm, L.req, 1024);
    char ruri[1200];
    snprintf(ruri, sizeof ruri, "file://%s", rpath);
    buf_printf(&cm,
               "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"textDocument/completion\",\"params\":"
               "{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%d,\"character\":%d}}}",
               ruri, tl, tc + 10);
    buf_reset(&cap2);
    run_one(&L, buf_cstr(&cm), cm.n, &cap2);
    check(has(buf_cstr(&cap2), "thing_id"), "path_i64( completes the route's own :placeholders");

    // the buffer above replaced view.c's facts; put the real ones back
    open_buf(&L, view, 15, vtxt, &cap2);

    // and a name that does resolve: definition lands on the view
    char suri2[1200];
    snprintf(suri2, sizeof suri2, "file://%s", server);
    const char *stxt2 = slurp_file(L.req, server, nullptr);
    open_buf(&L, server, 14, stxt2, &cap2);
    i32 dl, dc;
    loc_of(stxt2, "page(Todos", &dl, &dc);
    buf_init(&cm, L.req, 1024);
    buf_printf(&cm,
               "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"textDocument/definition\",\"params\":"
               "{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%d,\"character\":%d}}}",
               suri2, dl, dc + 5);
    buf_reset(&cap2);
    run_one(&L, buf_cstr(&cm), cm.n, &cap2);
    check(has(buf_cstr(&cap2), "view.c"), "definition on page(Todos) lands in view.c");

    buf_free(&cap2);
    buf_free(&cm);
  }


  // ---- the second engine, and the tree of projects
  {
    Buf cap2;
    buf_init(&cap2, L.req, 4096);
    Buf eb;
    buf_init(&eb, L.req, 4096);
    // plain C is the compiler's to judge; the DSL's own bytes are not
    buf_printf(&eb, "%s\nstatic void broken(void) { struct Todo *q = 0; (void)q->nope; }\n", vtxt);
    open_buf(&L, view, 30, buf_cstr(&eb), &cap2);
    check(has(buf_cstr(&cap2), "\"code\":\"c\""), "a plain C mistake is reported in the compiler's words");

    buf_reset(&eb);
    buf_printf(&eb, "%s\nstatic void f(VecTodo *v) { page(Nope, v); }\n", vtxt);
    open_buf(&L, view, 31, buf_cstr(&eb), &cap2);
    check(has(buf_cstr(&cap2), "no-view") || has(buf_cstr(&cap2), "\"code\":\"half\""),
          "a DSL mistake is not repeated in generated-symbol words");
    check(!has(buf_cstr(&cap2), "view_Nope"), "and the compiler does not get to talk about view_Nope");

    // The framework repository is a tree of projects: a buffer in one example must
    // resolve against that example's facts, not against every example at once.
    open_buf(&L, view, 32, vtxt, &cap2);
    WLsp L2;
    memset(&L2, 0, sizeof L2);
    L2.a = arena_new(64 * 1024);
    L2.req = arena_new(64 * 1024);
    buf_init(&L2.out, L2.a, 4096);
    snprintf(L2.root, sizeof L2.root, "%s", root);
    L2.F = facts_new(L2.a, root);
    check(L2.F->nproj > 1, "the framework workspace indexes each example as its own project");
    if (L2.F->nproj > 1) {
      const char *stxt3 = slurp_file(L2.req, server, nullptr);
      if (stxt3) {
        Buf cap3;
        buf_init(&cap3, L2.req, 4096);
        const char *pub3 = open_buf(&L2, server, 33, stxt3, &cap3);
        check(has(pub3, "\"diagnostics\":[]"),
              "a route file's view is found even when its view file was never opened");
        buf_free(&cap3);
      }
    }
    buf_free(&eb);
    buf_free(&cap2);
  }

  printf("\n  %s — %d request(s), %d message(s) written\n\n",
         g_self_fail ? "\033[31mFAILED\033[0m" : "\033[32mthe language server answers\033[0m",
         L.nreq, L.nresp);
  return g_self_fail ? 1 : 0;
}

// ---------------------------------------------------------------- replay

// A session file has to survive moving the checkout, so the root is `@ROOT@` inside
// it and the machine's own path is substituted going in and out. A gate that only
// passes on the machine that wrote it is a gate that fails the first person who
// clones the repository.
static char *subst(Arena *a, const char *s, const char *from, const char *to) {
  Buf b;
  buf_init(&b, a, strlen(s) + 64);
  size_t fl = strlen(from);
  for (const char *p = s; *p;) {
    if (fl && !strncmp(p, from, fl)) {
      buf_puts(&b, to);
      p += fl;
      continue;
    }
    buf_putc(&b, *p++);
  }
  return buf_cstr(&b);
}

// A session file is one JSON message per line: the requests the editor sends, and
// the responses it should get back. `--record` rewrites it, `--replay` diffs it, so a
// protocol change shows up as a diff in review — the same bargain `api.txt` makes.
// Notifications are not recorded: their content is asserted in --selftest, where an
// assertion can say *why* it matters.
static int replay(const char *file, bool record) {
  Arena *a = arena_new(1 << 20);
  size_t n = 0;
  char *text = slurp_file(a, file, &n);
  if (!text) {
    fprintf(stderr, "webc lsp: cannot read %s\n", file);
    return 1;
  }
  WLsp L;
  memset(&L, 0, sizeof L);
  L.a = a;
  L.req = arena_new(256 * 1024);
  buf_init(&L.out, a, 4096);
  char cwd[768];
  if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
  snprintf(L.root, sizeof L.root, "%s", cwd);
  L.t_start = wc_time_ms();
  L.F = facts_new(L.a, L.root);

  // split, keeping the order of both kinds
  char **in = arena_alloc(a, sizeof(char *) * 512);
  char **want = arena_alloc(a, sizeof(char *) * 512);
  i32 nin = 0, nwant = 0;
  for (char *p = text; p && *p;) {
    char *eol = strchr(p, '\n');
    if (eol) *eol = 0;
    if (p[0] && p[0] != '#' && strstr(p, "\"method\"") && nin < 512) in[nin++] = p;
    else if (p[0] && p[0] != '#' && strstr(p, "\"id\"") && nwant < 512) want[nwant++] = p;
    if (!eol) break;
    p = eol + 1;
  }

  Buf got;
  buf_init(&got, a, 8192);
  for (i32 i = 0; i < nin; i++) {
    Buf cap;
    buf_init(&cap, L.req, 4096);
    char *in_json = subst(L.req, in[i], "@ROOT@", L.root);
    run_one(&L, in_json, strlen(in_json), &cap);
    // keep only the id-bearing messages: a notification is not part of the contract
    char *c = buf_cstr(&cap);
    while (c && *c) {
      char *nl = strchr(c, '\n');
      if (nl) *nl = 0;
      if (c[0] && strstr(c, "\"id\"")) {
        buf_puts(&got, subst(L.req, c, L.root, "@ROOT@"));
        buf_putc(&got, '\n');
      }
      if (!nl) break;
      c = nl + 1;
    }
    buf_free(&cap);
  }

  if (record) {
    FILE *f = fopen(file, "wb");
    if (!f) {
      fprintf(stderr, "webc lsp: cannot write %s\n", file);
      return 1;
    }
    fprintf(f, "# webc lsp session. Regenerate: webc lsp --replay %s --record\n", file);
    fprintf(f, "# One JSON message per line: a request (it has \"method\"), then the\n"
               "# response it must produce (it has \"id\" and \"result\"). `@ROOT@` is the\n"
               "# project root, substituted on both sides so the file travels with the repo.\n");
    i32 written = 0;
    for (i32 i = 0; i < nin; i++) {
      Buf cap;
      buf_init(&cap, L.req, 4096);
      char *in_json = subst(a, in[i], "@ROOT@", L.root);
      run_one(&L, in_json, strlen(in_json), &cap);
      fprintf(f, "%s\n", subst(a, in[i], L.root, "@ROOT@"));
      char *c = buf_cstr(&cap);
      while (c && *c) {
        char *nl = strchr(c, '\n');
        if (nl) *nl = 0;
        if (c[0] && strstr(c, "\"id\"")) {
          fprintf(f, "%s\n", subst(a, c, L.root, "@ROOT@"));
          written++;
        }
        if (!nl) break;
        c = nl + 1;
      }
      buf_free(&cap);
    }
    fclose(f);
    printf("  recorded %d request(s) and %d response(s) to %s\n", nin, written, file);
    return 0;
  }

  i32 bad = 0;
  char *c = buf_cstr(&got);
  i32 i = 0;
  for (;;) {
    char *line = nullptr;
    if (c && *c) {
      char *nl = strchr(c, '\n');
      if (nl) *nl = 0;
      line = c;
      c = nl ? nl + 1 : nullptr;
    }
    if (!line && i >= nwant) break;
    const char *expect = (i < nwant) ? want[i] : nullptr;
    if (!line || !expect || strcmp(line, expect)) {
      if (bad < 3) {
        printf("  \033[31mFAIL\033[0m response %d\n      want: %s\n      got:  %s\n", i,
               expect ? expect : "(none)", line ? line : "(none)");
      }
      bad++;
    }
    i++;
    if (!line) break;
  }
  printf("\n\033[1m  webc lsp --replay\033[0m  %s\n\n  %d request(s), %d response(s) compared\n", file,
         nin, i);
  if (bad) {
    printf("\n  \033[31m%d response(s) changed\033[0m — if that is deliberate, record again\n\n", bad);
    return 1;
  }
  printf("  ok   the session's responses are unchanged\n\n");
  return 0;
}

// ---------------------------------------------------------------- bench

static int bench(const char *root) {
  WLsp L;
  memset(&L, 0, sizeof L);
  L.a = arena_new(256 * 1024);
  L.req = arena_new(256 * 1024);
  buf_init(&L.out, L.a, 4096);
  snprintf(L.root, sizeof L.root, "%s", root);
  L.F = facts_new(L.a, root);
  char proj[1024];
  snprintf(proj, sizeof proj, "%s/examples/todo/src/view.c", root);
  size_t n = 0;
  char *text = slurp_file(L.req, proj, &n);
  if (!text) {
    fprintf(stderr, "webc lsp: cannot read %s\n", proj);
    return 1;
  }
  char abs[1024];
  realpath(proj, abs);
  WDoc *d = lsp_doc_open(&L, abs, "", 1, str_mem(text, n));
  printf("\n\033[1m  webc lsp --bench\033[0m  %s (%zu bytes, %d regions)\n\n", abs, n, d->scan->n);
  const char *what[] = {"diagnostics", "completion", "hover"};
  printf("  budget: any request under 100 ms\n\n");
  for (i32 k = 0; k < 3; k++) {
    i64 t0 = wc_time_ms();
    const i32 R = 200;
    for (i32 i = 0; i < R; i++) {
      buf_reset(&L.out);
      if (k == 0) {
        lspx_diagnostics(&L, d, &L.out);
      } else if (k == 1) {
        const char *el = strstr(text, "el(\"");
        u32 off = el ? (u32)(el - text) + 4 : 0;
        lspx_completion(&L, d, off, &L.out);
      } else {
        const char *v = strstr(text, "view(Todos");
        u32 off = v ? (u32)(v - text) : 0;
        lspx_hover(&L, d, off, &L.out);
      }
    }
    f64 ms = (f64)(wc_time_ms() - t0) / R;
    printf("  %-12s %7.3f ms   %zu bytes\n", what[k], ms, L.out.n);
    // The diagnostics number is two engines: the server's own scan, and a compiler
    // asked once per pass. Both belong in the budget, and the split is what tells you
    // which one to look at when it is exceeded.
    if (k == 0)
      printf("  %-12s %7.3f ms   (webc) + %7.3f ms (%s)\n", "", ms - (f64)ceng_last_ms(),
             (f64)ceng_last_ms(), ceng_available() ? "cc" : "no compiler");
  }
  printf("\n");
  return 0;
}

// ---------------------------------------------------------------- entry

int cmd_lsp(i32 argc, char **argv) {
  const char *root = ".";
  const char *trace = nullptr;
  const char *replay_file = nullptr;
  bool selftest_mode = false, record = false, bench_mode = false;
  for (i32 i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "--stdio")) continue;
    else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
    else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace = argv[++i];
    else if (!strcmp(argv[i], "--replay") && i + 1 < argc) replay_file = argv[++i];
    else if (!strcmp(argv[i], "--record")) record = true;
    else if (!strcmp(argv[i], "--selftest")) selftest_mode = true;
    else if (!strcmp(argv[i], "--bench")) bench_mode = true;
    else {
      fprintf(stderr,
              "webc lsp: unknown option '%s'\n"
              "  webc lsp [--root DIR] [--trace FILE]      the server, on stdio\n"
              "  webc lsp --selftest [--root DIR]          a scripted session, asserted\n"
              "  webc lsp --replay FILE [--record]         a recorded session, diffed\n"
              "  webc lsp --bench [--root DIR]             what a request costs\n",
              argv[i]);
      return 1;
    }
  }
  char absroot[1024];
  char *rp = realpath(root, nullptr);
  if (rp) {
    snprintf(absroot, sizeof absroot, "%s", rp);
    free(rp);
    root = absroot;
  }
  if (selftest_mode) return selftest(root);
  if (bench_mode) return bench(root);
  if (replay_file) return replay(replay_file, record);

  WLsp L;
  memset(&L, 0, sizeof L);
  L.a = arena_new(512 * 1024);
  L.req = arena_new(256 * 1024);
  buf_init(&L.out, L.a, 4096);
  snprintf(L.root, sizeof L.root, "%s", root);
  L.t_start = wc_time_ms();
  L.F = facts_new(L.a, root);
  if (trace) {
    L.trace = fopen(trace, "wb");
    if (!L.trace) fprintf(stderr, "webc lsp: cannot write %s\n", trace);
  }
  if (getenv("WEBC_TRACE"))
    fprintf(stderr, "webc lsp: %d files indexed under %s\n", L.F->nfile, root);

  Buf body;
  buf_init(&body, L.a, 8192);
  while (read_msg(&body)) {
    i64 t0 = wc_time_ms();
    // nullptr: write straight to the wire. Only the test modes capture.
    run_one(&L, buf_cstr(&body), body.n, nullptr);
    if (L.trace) trace_msg(&L, "in", buf_cstr(&body), body.n, (wc_time_ms() - t0) * 1000);
    fflush(stdout);
  }
  if (L.trace) fclose(L.trace);
  return 0;
}