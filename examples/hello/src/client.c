// hello — the browser half: the one interaction the page adds. A named link, a
// bound action, a signal for the shake, and the framework's timer. The form
// stays a real POST; no-JavaScript is the whole no-JS app.
#include <webc/web.h>
#include <webc/host.h>
#include "app.h"

static Greeting C;

static void clear_pulse(WJs r) {
  (void)r;
  set(try_pulse, false);
}

action(try_greeting) {
  set(try_pulse, true);
  reveal_hook("try");  // after the batch, when the card is where it should be
  focus_hook("name");  // the field, by its id — the reader can type immediately
  webc_later(1800, clear_pulse);
}

void wc_app_data(str json) {
  // The page carries the model it was rendered against (webc_seed, boot.inline):
  // decode it once, and a POST's answer survives hydration unchanged.
  JParse p = json_parse(wc_arena_get(), json);
  if (p.ok && p.v) model_read_json_val(wc_arena_get(), Greeting_modelof(), p.v, &C);
}

client_main {
  bind("try-link", "click", try_greeting);
  mount(Home, &C, "#root");
  return 0;
}
