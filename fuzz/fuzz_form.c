// fuzz_form — the form and query readers: percent-decoding, `+`, repeated keys,
// absurd values against typed fields (SPEC §11).
// `webc/webc.h` is retired (see `webc lint`); name what is used.
#include <webc/form.h>

WEBC_MODEL(FzForm, (id, i64, .pk = true), (name, str, .max = 64), (ok, bool), (ratio, f64))

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
  Arena *a = arena_new(1 << 20);
  FzForm out = {0};
  str body = {(char *)data, size};
  // both directions: urlencoded into the model, and the model's validators on
  // whatever came out of it
  if (model_from_form(a, FzForm_modelof(), body, &out).ok) {
    WErrs errs = {0};
    (void)model_validate(FzForm_modelof(), &out, &errs);
  }
  arena_destroy(a);
  return 0;
}
