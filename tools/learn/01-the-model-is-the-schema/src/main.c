// SPDX-License-Identifier: Apache-2.0
// learn 01 — a model is the schema.
//
// The struct, the validation, the JSON codec and the table all move together, from
// one declaration. The test in tests/ names a field this model does not have: run
// `webc test` and read what the compiler says, then add the field where it belongs
// — and the column that follows it.
#include <webc/server.h>
#include "app.h"

GET("/api/tasks") {
  return json_list(200, db_all(Task));
}

POST("/api/tasks") {
  Task t = body(Task);
  db_save(Task, &t);
  return json(201, Task, &t);
}
