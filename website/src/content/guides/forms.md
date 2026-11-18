A form in WebC is a form first: it submits, it validates on the server, it redirects or re-renders.
The browser half then makes the same form faster without changing what it does — which is why an app
that never loads its module is still a working app.

Each snippet here is a file you can compile on its own.

## The form that works

```c
// src/view.c — a form, hand-written, no JavaScript required
#include <webc/shared.h>

WEBC_MODEL(Note, (id, i64, .pk = true), (body, Text, .required = true))

view(NewNote) {
  el("main") {
    el("h1") { text("Notes"); }
    el("form") { post_to("/notes");
      el("input") { name("body"); placeholder("a note"); required(); }
      el("button") { text("Add"); }
    }
  }
}
```

```c
// src/server.c — the route that receives it
#include <webc/server.h>

WEBC_MODEL(Note, (id, i64, .pk = true), (body, Text, .required = true))

POST("/notes") {
  Note n = body(Note);          // form or JSON, validated by the model
  db_save(Note, &n);
  return wc_body_is_form() ? redirect("/") : json(201, Note, &n);
}
```

`body(Note)` reads `application/x-www-form-urlencoded` or JSON, decides by the content type, and
validates with the rules the model declared. A failure answers 422 with the field that failed:

```console
$ curl -s -d 'body=' localhost:8080/notes
{"error":"invalid_body","message":"body: required","status":422}
```

## The form drawn from the model

Writing the inputs by hand is fine for two fields. For a model you already declared, `form.h` draws
them — the kind, the bounds, the label and the error all come from one declaration:

```c
// src/view.c — the same form, drawn from the model that validates it
#include <webc/shared.h>
#include <webc/form.h>

WEBC_MODEL(Signup,
           (email, Text, .required = true, .format = "email"),
           (plan,  Text, .required = true))

view(SignupPage, WForm *f) {          // the form is the view's context
  el("form") { post_to("/signup");
    // input, inputmode, min/max, pattern, label, aria-describedby
    form_fields(f);
    form_errors(f);      // one message per field, announced
    el("button") { type_("submit"); text("Sign up"); }
  }
}
```

- `f->prefix = "edit"` gives a second form on the same page its own id space — the thing a
  hand-written field set gets wrong at the second form.
- `form_field_long(f, "bio", 6)` is the one place a `textarea` is chosen, because rows is a
  decision about the page, not about the model.
- Adding a field to the struct adds a field to the form, with its label and its rules.

The handler that renders it builds the form around the draft it is validating:

```c
// src/server.c — one form, filled from the body, rendered with the errors in place
#include <webc/server.h>
#include <webc/form.h>

WEBC_MODEL(Signup,
           (email, Text, .required = true, .format = "email"),
           (plan,  Text, .required = true))
declare_view(SignupPage, WForm);

POST("/signup") {
  Signup draft = {0};
  WForm f;
  // ids, labels and bounds, from the model
  form_init(&f, Signup_modelof(), &draft);
  // the page again
  if (!body_try(Signup, &draft)) return page_with(422, SignupPage, &f);
  // the sentence on the field
  if (!form_submit(&f)) return page_with(422, SignupPage, &f);
  db_save(Signup, &draft);
  return redirect("/welcome");
}
```

## Validation, three ways

The same rules run in three places, from `model_validate()`:

| Where | How the user learns |
| --- | --- |
| the form post | a 422 and a re-render with `form_errors()` |
| the JSON API | a 422 body naming `field` and `message` |
| the browser half | the same fields, checked before the request goes out |

None of them is the only check. A server never trusts the client's copy of the rules, and a client
never sends a request it already knows will fail.

## Enhancing it

A form needs nothing to become interactive. The client half mounts the same view and the same
`post_to` — the submit becomes a `fetch` and a re-render, and the no-JS path stays as a fallback for
the first paint and for anything that breaks.

This is a whole client-only app in one file: the view, the signal it binds, the action that reads it,
and the mount.

```c
// src/client.c — a draft in a signal, two-way bound to the input
#include <webc/web.h>
// webc_post: the network, from the browser half
#include <webc/host.h>

state(Text, draft, "");                    // one value for the page

action(add_note) {
  // the model's rule, applied early: no round trip
  if (!get(draft)[0]) return;
  webc_post(str_lit("/api/notes"), fmt_s("{\"body\":\"%s\"}", get(draft)), nullptr);
  set(draft, "");                          // a render follows this action
}

view(NoteForm) {
  el("form") {
    // two-way, one line, in the view
    el("input") { name("body"); bind_str(draft); }
    el("button") { named("add"); text("Add"); }
  }
}

client_main {
  mount(NoteForm, "#root");
  bind("add", "click", add_note);          // the name the view marked
  return 0;
}
```

`bind_str` is the whole wiring: the input writes the signal on every keystroke, and a live binding
reading it writes the node back. There is no second line in `client_main` to forget.

## Uploads

`upload.h` is the file case, and it does the four things a hand-rolled upload forgets: it checks the
magic bytes rather than the extension, it makes up the name on the server side (so a client cannot
choose a path), it enforces a size before reading the whole body, and it answers with a URL that is
inside the app's own root.

```c
// src/server.c — the server half: the upload route, and nothing parsed first
#include <webc/server.h>
#include <webc/upload.h>

POST("/apply") {
  // no body(User) call: upload_take() parses the multipart body itself, once
  UploadPolicy pol = upload_policy(".data/cv", 8 * 1024 * 1024);
  pol.allow = (const char *[]){"application/pdf"};
  pol.allow_n = 1;
  Upload up = upload_take(req, "cv", &pol);
  // up.why is a sentence for the person: "the file is larger than it allows"
  if (!up.ok) return res_error(422, "bad_upload", up.why);
  return redirect("/thanks");   // up.name is the name this side made up
}
```

Three arguments and one struct: `up.why` is a sentence for the person who uploaded it, not a code,
because it is going next to the field they just filled in. A body that is not a
`multipart/form-data` upload says so — *"a `<form>` with enctype set sends one"* — which is the
mistake that actually happens.

## Accessibility is part of the form

`form_fields()` emits a `<label>` bound by `for`, an `aria-describedby` pointing at the error, and
the `inputmode` and `autocomplete` the field's kind implies. `form_errors()` puts each message in a
live region, so a screen reader hears what changed. `overlay.h` and `keys.h` follow the same rule:
the ARIA is in the component, not in each app.

## See also

- Reference [§5 JSON and forms](../reference/json-and-forms.html), and the `form` module in
  [§12 The shelf](../reference/frontend-dom-and-ssr.html).
- [Models](../guides/models.html) — the declaration these inputs come from.