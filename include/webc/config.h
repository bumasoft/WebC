// SPDX-License-Identifier: Apache-2.0
// WebC — configuration from the environment and a JSON file (SPEC §6.6).
//
//   WEBC_MODEL(Cfg, (port, i32, .doc = "listen port"),
//                   (database_url, str, .required = true),
//                   (debug, bool))
//
//   Cfg cfg = config_load(Cfg, "app.json", getenv("WEBC_ENV"));
//
// The schema *is* the model: no generated file, no second declaration of the same
// fields, and `webc doctor` can print it (every model in the build is visible).
// Precedence, highest first:
//
//   1. an environment variable named after the field — `WEBC_PORT`, `PORT`
//   2. the JSON file for the current environment — `{"port": 8080}` or
//      `{"defaults": {...}, "production": {...}}` with WEBC_ENV=production
//   3. the field's `.def` qualifier, or the zero value
//
// A JSON file that is present but unreadable or invalid is an error, not a silent
// fallback: a service that starts on the wrong port because a brace was missing is
// a service you will spend an evening on.
#ifndef WEBC_CONFIG_H
#define WEBC_CONFIG_H

#include <webc/core.h>
#include <webc/model.h>

// Load into `out` (zeroed first). `path` may be null; `env` selects the section of
// a sectioned file and may be null.
WErrRes config_load_model(Arena *a, const WModel *m, void *out, const char *path, const char *env);

// `Cfg cfg = config_load(Cfg, "app.json", getenv("WEBC_ENV"));` — a statement
// expression, so the declaration stays one line and the type is named once.
#define config_load(T_, path_, env_)                                                               \
  ({                                                                                               \
    T_ _cfg_ = {0};                                                                                \
    WErrRes _e_ = config_load_model(wc_arena_get(), T_##_modelof(), &_cfg_, (path_), (env_));       \
    if (!_e_.ok) panic("config: %.*s", WERR_FMT(_e_.e));                              \
    _cfg_;                                                                                         \
  })

#define config_try(T_, var_, path_, env_)                                                          \
  T_ var_ = {0};                                                                                   \
  WErrRes WC_UNIQ(__COUNTER__) =                                                                   \
      config_load_model(wc_arena_get(), T_##_modelof(), &var_, (path_), (env_))

// The schema as text, for `webc doctor` and for a README that cannot go stale.
str config_schema_doc(Arena *a, const WModel *m);

#endif // WEBC_CONFIG_H
