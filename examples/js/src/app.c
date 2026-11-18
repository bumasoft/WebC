// WebC — the JavaScript seam, one door at a time.
//
// Five ways across the boundary, each on its own row, each a value rather than a global:
//
//   js("path", args…)     a call, with arguments typed where C can check them
//   js("path")            a property read — no function, no arguments, just the value
//   r.ok / r.error        a failure that belongs to *that* call
//   js_then(…, fn, ctx)   a promise: the handler runs when it settles
//   WebC.call / dispatch  the page driving the app, and webc_export_json the door in
//
// Everything the page shows comes from a signal; every row writes one. Nothing here
// parses JSON by hand, writes JSON by hand, or reads a "last error" that a later call
// could have overwritten.
#include <webc/web.h>
#include <webc/js.h>

// A Text is a string this app owns for the life of the render (`fmt`, `text_of`), which
// is what a signal wants to hold — and what a format string prints with `%s`.
state(Text, title_of_page, "—");
state(Text, stored, "—");
state(Text, failure, "—");
state(Text, cores, "—");
state(Text, answer, "—");
state(i64, calls, 0);

// ---------------------------------------------------------------- a promise

// The reply to `fetch` arrives here, possibly seconds later. `r` is the resolved value —
// or the rejection, told apart by `r.ok` and explained by `r.error`.
static void on_reply(WJs r) {
  if (!r.ok) {
    set(answer, fmt("fetch failed: %s", text_of(r.error)));
    return;
  }
  JVal *v = js_value(r);
  JVal *msg = v ? json_get(v, "message") : nullptr;
  if (msg && msg->kind == J_STR) {
    // One more call from inside the handler, and *before* the reply is used: `r` points
    // into the bridge's buffer, and the next call replaces that buffer. The framework
    // copies the payload out before the handler runs, so the line below still reads the
    // reply — without the copy it would read the next call's result.
    js("localStorage.setItem", "webc.demo.reply", msg->s);
    set(answer, text_of(msg->s));
  } else
    set(answer, fmt("the reply was a %s, not an object with a message", js_kind_name(r.kind)));
}

// ---------------------------------------------------------------- what a click does

action(probe) {
  // 1. a property read: no arguments means "give me the value"
  WJs t = js("document.title");
  set(title_of_page, t.kind == JS_STR ? text_of(js_str(t, "—")) : fmt("(no title: %s)", text_of(t.error)));

  // 2. a call with two arguments, one of them a string no caller had to escape:
  //    the quote and the em dash are the framework's problem, not the app's
  WJs put = js("localStorage.setItem", "webc.demo", "a \"quoted\" value & an em dash —");
  WJs got = js("localStorage.getItem", "webc.demo");
  (void)put;
  set(stored, got.kind == JS_STR ? text_of(js_str(got, "—")) : fmt("error: %s", text_of(got.error)));

  // 3. a call that cannot work. Its failure is *this value*; the calls above keep theirs.
  WJs bad = js("no.such.thing");
  set(failure, text_of(bad.ok ? str_lit("(it worked?)") : bad.error));

  // 4. a number, read as a property and converted with a default: no string, no parse
  set(cores, fmt("this machine reports %lld", (long long)js_i64(js("navigator.hardwareConcurrency"), 0)));

  // 5. a promise. The call returns now; on_reply runs when the file has been fetched.
  //    `api.get` is one line in the page (index.html): fetch resolves with a Response,
  //    and a Response is an object this side has no handle for — by design, so no
  //    JavaScript object ever has a lifetime the C side must remember to release.
  set(answer, fmt("waiting…"));
  js_then("api.get", "/data.json", on_reply);
}

// `action_live`: this handler writes one signal and changes nothing structural, so the
// bridge sends the binding's one op and re-renders nothing. Watch it in the console —
// `WebC.diagnostics().ops` goes up by 1 per click, not by the page's ~40.
action_live(bump) { set(calls, get(calls) + 1); }

// The page calls this: `WebC.call('note', {text: '…'})`. The payload arrives decoded, so
// reading a field is a lookup and a string is a string.
webc_export_json(note) {
  JVal *v = js_value(in);
  JVal *t = v ? json_get(v, "text") : nullptr;
  set(stored, t && t->kind == J_STR ? text_of(t->s) : fmt("(no text in the payload)"));
}

// ---------------------------------------------------------------- the view

static void row(const char *label, Text value) {
  el("tr") {
    el("th") { text(label); }
    el("td") { text(value && *value ? value : "—"); }
  }
}

view(Seam) {
  el("main") {
    el("h1") { text("The JavaScript seam"); }
    el("p") { class_("lede"); text("Five doors, no JSON written by hand."); }

    el("p") { class_("actions");
      el("button") { class_("go"); on_click(probe); text("Walk through them"); }
      el("button") {
        class_("go");
        on_click(bump);
        text_live("named(\"bump\") has fired %lld time(s)", calls);
      }
    }

    el("table") {
      row("document.title", get(title_of_page));
      row("localStorage round trip", get(stored));
      row("a call that fails", get(failure));
      row("navigator.hardwareConcurrency", get(cores));
      row("fetch → js_then", get(answer));
    }

    el("p") {
      class_("hint");
      text("The two buttons under the table drive the app from the page: "
           "WebC.dispatch('bump') and WebC.call('note', {text: 'hello'}).");
    }
  }
}

// ---------------------------------------------------------------- the client half

client_main {
  mount(Seam, "#root");
  return 0;              // both buttons wire themselves, in the view
}