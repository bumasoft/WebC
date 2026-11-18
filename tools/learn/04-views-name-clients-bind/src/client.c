// SPDX-License-Identifier: Apache-2.0
// the client half owns behaviour: an action, wired to a name the view declares.
// webc check reconciles the two — run it and read the sentence it prints.
#include <webc/web.h>
#include "app.h"

action(toggle, Task *t) { t->done = !t->done; }

client_main {
  bind("tick", "click", toggle);
  mount(Tasks, "#root");
  return 0;
}
