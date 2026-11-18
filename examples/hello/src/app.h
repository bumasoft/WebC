// hello — the shared half: one model, one view prototype, one interaction state.
// `state()` in a shared header is the same declaration on both halves: SSR draws
// the initial value, the browser owns the writes.
#pragma once
#include <webc/shared.h>

WEBC_MODEL(Greeting, (name, Text, .required = true),
                      (greeting, Text))
// A request/response shape, not a table: no database, no migration to drift.
WEBC_MODEL_INTERNAL(Greeting)

declare_view(Home, Greeting);

state(bool, try_pulse, false);
