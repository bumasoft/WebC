// examples/minimal — the hello-world the wasm bundle budget is measured on
// (SPEC §10: ≤ 24 KB gzip, bridge included). One signal, one component, one action:
// whatever this weighs is the floor a WebC frontend app costs.
//
// It is also the smallest honest client app: a view that reads a state slot, an action
// that changes it, and a button that names the action where the button is written. The
// markup is a designed page rather than three bare tags — that costs bytes in `main.c`,
// not in the bundle — and the stylesheet is a plain file beside it (`public/style.css`),
// because a page that looks like nobody meant it teaches the wrong thing about the floor.
#include <webc/web.h>

state(i64, clicks, 0);

ACTION(inc)   // the action below, declared for the view that wires it

view(Counter) {
  el("main") {
    class_("card");
    el("header") {
      class_("head");
      el("span") { class_("mark"); attr("aria-hidden", "true"); text("W"); }
      el("h1") { text("Hello, WebC"); }
    }
    // The number is the page. `aria-live` is on the pair, not on the figure: a screen
    // reader should hear "3 clicks", and a bare "3" is a number with no unit in it.
    el("p") {
      class_("readout");
      attr("aria-live", "polite");
      el("span") { class_("count"); text("%lld", (long long)get(clicks)); }
      el("span") { class_("unit"); text(get(clicks) == 1 ? "click" : "clicks"); }
    }
    el("button") { on_click(inc); text("+1"); }
    el("p") { class_("foot"); text("one signal · one view · one action"); }
  }
}

action(inc) { set(clicks, get(clicks) + 1); }

client_main {
  mount(Counter, "#root");
  return 0;
}