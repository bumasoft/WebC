// SPDX-License-Identifier: Apache-2.0
// learn 05 — validation is the declaration.
//
// There is no validation layer to write: a qualifier on the field IS the rule,
// and `body(Signup)` answers a 422 naming the field when it is broken. This
// signup accepts an empty email and the test knows it should not. Say so once,
// on the field.
#include <webc/server.h>
#include "app.h"

POST("/signup") {
  Signup s = body(Signup);
  db_save(Signup, &s);
  return json(201, Signup, &s);
}
