// hello — the server half: the routes, and nothing else. No main(), no middleware
// list, no arena: the framework boots and applies its defaults.
#include <webc/server.h>
#include "app.h"

GET("/") {
  // The first paint already has an answer: an example should never open blank.
  return page(Home, &(Greeting){.greeting = "Hello, world."});
}

GET("/hello/:name") {
  Greeting g = {.name = path("name")};
  g.greeting = fmt("Hello, %s.", g.name);
  return json(200, Greeting, &g);
}

POST("/greet") {
  Greeting g = body(Greeting);
  g.greeting = fmt("Hello, %s.", g.name);
  return page(Home, &g);
}

GET("/health") {
  return res_text(200, "ok");
}

GET("/api/greeting") {
  return json(200, Greeting, &(Greeting){.greeting = "Hello, world."});
}

// The page's own seed: the model this response was rendered against, embedded
// inline so the browser half cannot disagree with it (and no fetch lands late).
str webc_seed(Arena *a) {
  Greeting g = {.greeting = "Hello, world."};
  Request *r = webc_req_current();
  if (r && str_eq_c(r->path, "/greet")) {
    g = body(Greeting);
    g.greeting = fmt("Hello, %s.", g.name);
  }
  return model_to_json(a, Greeting_modelof(), &g);
}
