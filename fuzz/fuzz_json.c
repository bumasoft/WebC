// fuzz_json — libFuzzer harness for the JSON reader (SPEC §11).
//
//   make fuzz            (10 s per harness, ASan + UBSan)
//   make fuzz FUZZ_TIME=60
//
// The property is more than "it does not crash": when a document parses, the tree
// must be emit-able, and emitting → parsing → emitting again must be byte-identical.
// A parser that accepts garbage but loses it is as broken as one that crashes, and
// that is the bug this catches.
// The umbrella `webc/webc.h` is a retired spelling — `webc lint`
// flags it and the header split removed the file, so this harness has
// not compiled since. Name what is used.
#include <webc/json.h>

// A plain emitter for a parsed tree: strings via json_encode_str, numbers kept
// verbatim where the parser preserved them as integers.
static void emit(Buf *b, const JVal *v) {
  switch (v->kind) {
  case J_NULL: buf_puts(b, "null"); break;
  case J_BOOL: buf_puts(b, v->b ? "true" : "false"); break;
  case J_NUM:
    if (v->is_int)
      buf_puti64(b, v->inum);
    else
      buf_printf(b, "%.17g", v->num);
    break;
  case J_STR: {
    buf_putc(b, '"');
    for (size_t i = 0; i < v->s.n; i++) {
      unsigned char c = (unsigned char)v->s.p[i];
      switch (c) {
      case '"': buf_puts(b, "\\\""); break;
      case '\\': buf_puts(b, "\\\\"); break;
      case '\n': buf_puts(b, "\\n"); break;
      case '\r': buf_puts(b, "\\r"); break;
      case '\t': buf_puts(b, "\\t"); break;
      default:
        if (c < 0x20)
          buf_printf(b, "\\u%04x", (unsigned)c);
        else
          buf_putc(b, (char)c);
      }
    }
    buf_putc(b, '"');
    break;
  }
  case J_ARR:
    buf_putc(b, '[');
    for (i32 i = 0; i < v->n; i++) {
      if (i) buf_putc(b, ',');
      emit(b, v->items[i]);
    }
    buf_putc(b, ']');
    break;
  case J_OBJ:
    buf_putc(b, '{');
    for (i32 i = 0; i < v->n; i++) {
      if (i) buf_putc(b, ',');
      buf_putc(b, '"');
      for (size_t k = 0; k < v->keys[i].n; k++) {
        unsigned char c = (unsigned char)v->keys[i].p[k];
        if (c == '"' || c == '\\')
          buf_putc(b, '\\');
        buf_putc(b, (char)c);
      }
      buf_puts(b, "\":");
      emit(b, v->items[i]);
    }
    buf_putc(b, '}');
    break;
  }
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
  Arena *a = arena_new(1 << 20);
  str in = {(char *)data, size};
  JParse p = json_parse(a, in);
  if (p.ok && p.v) {
    Buf b1;
    buf_init(&b1, a, 64);
    emit(&b1, p.v);
    str once = buf_str(&b1);
    JParse again = json_parse(a, once);
    if (!again.ok) __builtin_trap(); // the emitter produced unparseable JSON
    Buf b2;
    buf_init(&b2, a, 64);
    emit(&b2, again.v);
    str twice = buf_str(&b2);
    if (!str_eq(once, twice)) __builtin_trap(); // the tree does not survive a round trip
  }
  arena_destroy(a);
  return 0;
}
