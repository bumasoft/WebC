// hello's view: markup only, compiled for the server and the browser. The link
// names itself; the client half says what that name does. The shake is a signal
// with a live class binding, so the card never needs a script of its own.
#include "app.h"

// The page is a value: the same view serves the first paint and the form's
// answer. A view takes its context by name, so a route reads like prose.
view(Home, Greeting *g) {
  el("div") { class_("shell");
    el("header") { class_("topbar");
      el("a") { class_("brand"); href("/");
        el("span") { class_("brand-mark"); text("W"); }
        text("WebC");
      }
      el("nav") { class_("topnav"); attr("aria-label", "Example links");
        el("a") { href("/hello/world"); text("JSON API"); }
        el("a") { href("https://github.com/bumasoft/WebC"); text("Source ↗"); }
      }
    }

    el("main") { class_("hero");
      el("section") { class_("intro");
        el("p") { class_("eyebrow"); text("C23 · SERVER-RENDERED · ONE BINARY"); }
        el("h1") { text("Hello, web."); }
        el("p") { class_("lede");
          text("A small app with a real model, a real route, and a first paint "
               "that needs no JavaScript. This is the whole idea.");
        }
        el("div") { class_("actions");
          el("a") { class_("button primary"); named("try-link"); href("#try"); text("Try the greeting →"); }
          el("a") { class_("button quiet"); href("/hello/world"); text("Read the JSON"); }
        }
        el("p") { class_("fineprint");
          text("One declaration gives the struct, the validation, the JSON and the form.");
        }
      }

      el("section") { class_("showcase"); attr("aria-label", "The app in one screen");
        el("div") { class_("code-card");
          el("div") { class_("card-bar");
            el("span") { class_("dot red"); }
            el("span") { class_("dot amber"); }
            el("span") { class_("dot green"); }
            el("span") { class_("filename"); text("main.c"); }
          }
          el("pre") { class_("code"); attr("aria-label", "A WebC route");
            el("code") {
              el("span") { class_("kw"); text("WEBC_MODEL"); }
              text("(Greeting, (name, Text, .required = true))\n\n");
              el("span") { class_("kw"); text("GET"); }
              text("(\"/hello/:name\") {\n");
              el("span") { class_("fn"); text("  return json"); }
              text("(200, Greeting, &g);\n}\n");
            }
          }
        }

        el("div") { class_(get(try_pulse) ? "greeting-card shake" : "greeting-card"); id_("try");
          el("p") { class_("card-kicker"); text("THE FIVE-SECOND DEMO"); }
          el("h2") { text("Say hello back."); }
          if (g->greeting && *g->greeting) {
            el("p") { class_("greeting"); attr("role", "status"); text("%s", g->greeting); }
          } else {
            el("p") { class_("muted"); text("Give the model a name. It answers in HTML and JSON."); }
          }
          el("form") { post_to("/greet");
            el("label") { attr("for", "name"); text("Your name"); }
            el("div") { class_("form-row");
              el("input") {
                id_("name"); name("name"); placeholder("Ada");
                attr("autocomplete", "off"); required();
                if (g->name && *g->name) value_(g->name);
              }
              el("button") { type_("submit"); text("Say hello →"); }
            }
          }
          el("p") { class_("fineprint");
            text("POST /greet renders this page. GET /hello/:name answers the same model as JSON.");
          }
        }
      }
    }

    el("section") { class_("proof"); attr("aria-label", "What this app proves");
      el("div") { class_("proof-item");
        el("strong") { text("0"); }
        el("span") { text("lines of JavaScript required"); }
      }
      el("div") { class_("proof-item");
        el("strong") { text("1"); }
        el("span") { text("model, shared by the page and the API"); }
      }
      el("div") { class_("proof-item");
        el("strong") { text("C23"); }
        el("span") { text("from the route to the rendered document"); }
      }
    }

    el("footer") { class_("footer");
      el("span") { text("A WebC example · deliberately small, deliberately finished."); }
      el("a") { href("/health"); text("Health check ↗"); }
    }
  }
}

