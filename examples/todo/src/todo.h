// The todo app's model — the one file both halves include.
//
// A `Text` field is a `const char *` the framework owns for the life of whatever
// scope produced it (the request, the render, the app). No arena, no length, no
// cast: `text("%s", t->title)`.
#pragma once
#include <webc/shared.h>

WEBC_MODEL(Todo, (id, i64, .pk = true),
                 (title, Text, .required = true, .max = 200),
                 (done, bool),
                 (created_at, Text))

// VecTodo, carrying its own element type: `for_each(t, todos)` takes its key from
// the model's `.pk` and `json(200, todos)` needs no type name.
Vec(Todo)

// The page, declared where the server and (if it had one) the client can both see it.
declare_view(Todos);
