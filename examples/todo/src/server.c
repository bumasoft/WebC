// The server half: routes and data. It renders the page the shared view describes
// and answers the JSON the browser half (if any) would use.
//
//   webc dev                    the default database, the dev reload server
//   DATABASE_URL=sqlite:data/todo.db ./todo
#include <webc/server.h>
#include "todo.h"

// The page, seeded from the table.
GET("/") { return page(Todos, db_all(Todo)); }

// The list and one create, for a client that speaks JSON. `json(200, v)` knows the
// element type because the collection carries its descriptor.
GET("/api/todos") { return json_list(200, db_all(Todo)); }

POST("/api/todos") {
  Todo t = body(Todo);            // form or JSON, validated by the model
  t.created_at = now();
  db_save(Todo, &t);              // insert or update; a driver error is a response
  // a form post gets its redirect back to the page, a JSON call gets the row it made
  if (wc_body_is_form()) return redirect("/");
  return json(201, Todo, &t);
}

PATCH("/api/todos/:id") {
  Todo *t = db_get(Todo, path_i64("id"));
  if (!t) fail(404, "no such todo");
  t->done = !t->done;
  db_save(Todo, t);
  return json(200, Todo, t);
}

DELETE("/api/todos/:id") {
  db_delete(Todo, path_i64("id"));
  return res_empty(204);
}

// The no-JavaScript half of the UI: an HTML form cannot PATCH, so each row posts
// here, flips the row and redirects back. One line, because the form carries only
// an id and `db_get` answers null for a stale one.
POST("/todos/toggle") {
  Todo *t = db_get(Todo, body(RowId).id);
  if (t) {
    t->done = !t->done;
    db_save(Todo, t);
  }
  return redirect("/");
}

POST("/todos/delete") {
  db_delete(Todo, body(RowId).id);
  return redirect("/");
}

// Server-sent events, still a stub in an example that has no live updates yet.
GET("/api/events") { return res_sse(nullptr, nullptr); }
