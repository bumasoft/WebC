// SPDX-License-Identifier: Apache-2.0
// webc_dev.h — keeping app state across a hot reload (dev only).
//
// `webc dev` swaps the wasm module on every save instead of reloading the page. Something
// has to decide what survives that, and the honest answer is: what is named.
//
//   - Signals cross by themselves. `state(i64, count, 0)` is registered at start-up, so
//     its value is carried into the new module and a save keeps it.
//   - Everything else belongs to the app. A Vec of rows, a selected id, an open panel —
//     the framework cannot know how to write those down, and must not guess.
//
// So define these two functions in your app, and a save keeps them too:
//
//   void webc_dev_state_save(Buf *out) {
//     buf_puti64(out, g_next_id);
//     buf_putc(out, '\n');
//     for (i32 i = 0; i < len(g_notes); i++)
//       buf_printf(out, "%.*s\n", (int)at(g_notes, i)->title.n, at(g_notes, i)->title.p);
//   }
//
//   void webc_dev_state_load(str in) {
//     // parse what save() wrote; return early on anything unexpected
//   }
//
// Rules, so that a swap can never be the reason a page breaks:
//
//   - `load` runs in the *new* module before the first render. Anything it does not read
//     keeps its initial value, so an old snapshot is always safe to load.
//   - Bytes you keep must be yours: `save` writes into `out`, `load` gets bytes you copy.
//     Never keep a pointer that came from `in` — it points into a buffer that dies.
//   - Both are optional and both are weak. An app without them keeps its DOM and its
//     signals across a save and nothing else.
//
// They only ever run under `webc dev`. A production build never calls them, and a hard
// reload (Cmd/Ctrl-Shift-R) throws the snapshot away on purpose.
#ifndef WEBC_WEBC_DEV_H
#define WEBC_WEBC_DEV_H
#include <webc/core.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void webc_dev_state_save(Buf *out) __attribute__((weak));
extern void webc_dev_state_load(str in) __attribute__((weak));

#ifdef __cplusplus
}
#endif
#endif // WEBC_WEBC_DEV_H
