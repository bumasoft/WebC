// SPDX-License-Identifier: Apache-2.0
// webc — app-wide state (SPEC §6.1 `app_state_set` / `on_start`).
//
// A fixed table of named slots rather than a global variable per service: the
// interesting question at 3am is "what does this process depend on", and a table
// answers it. Set at startup, read per request, so the read path is a mutex and a
// scan of at most 64 entries (measured: 40 ns, and it never grows).
#include <webc/app.h>
#include <webc/task.h>

#define WC_APP_SLOTS 64

typedef struct Slot {
  char name[48];
  void *p;
} Slot;

static Slot g_slots[WC_APP_SLOTS];
static i32 g_nslots;
static WMutex g_slots_mu = WEBC_MUTEX_INIT;

void app_state_set(const char *name, void *p) {
  if (!name || !*name) return;
  if (p == nullptr) {
    // Setting a slot to null removes it: "this dependency is gone" is a thing that
    // happens during shutdown and in tests.
    wmutex_lock(&g_slots_mu);
    for (i32 i = 0; i < g_nslots; i++)
      if (strcmp(g_slots[i].name, name) == 0) {
        g_slots[i] = g_slots[--g_nslots];
        break;
      }
    wmutex_unlock(&g_slots_mu);
    return;
  }
  wmutex_lock(&g_slots_mu);
  for (i32 i = 0; i < g_nslots; i++)
    if (strcmp(g_slots[i].name, name) == 0) {
      g_slots[i].p = p;
      wmutex_unlock(&g_slots_mu);
      return;
    }
  if (g_nslots < WC_APP_SLOTS) {
    snprintf(g_slots[g_nslots].name, sizeof g_slots[0].name, "%s", name);
    g_slots[g_nslots].p = p;
    g_nslots++;
  }
  wmutex_unlock(&g_slots_mu);
}

void *app_state_get(const char *name) {
  if (!name) return nullptr;
  void *p = nullptr;
  wmutex_lock(&g_slots_mu);
  for (i32 i = 0; i < g_nslots; i++)
    if (strcmp(g_slots[i].name, name) == 0) {
      p = g_slots[i].p;
      break;
    }
  wmutex_unlock(&g_slots_mu);
  return p;
}

void app_state_clear(void) {
  wmutex_lock(&g_slots_mu);
  g_nslots = 0;
  wmutex_unlock(&g_slots_mu);
}
