You already know most of WebC. The model is the same one every component framework uses — declared
state, a render that reads it, events that write it — and the differences that matter are in *which
spelling* does which job. This page is the dictionary: the name you know, the name WebC uses, and the
sentence of difference underneath.

# Coming from React, Solid and Next

## The one mapping that explains most of it

| You know | WebC | The difference under the name |
| --- | --- | --- |
| `useState` (per component instance) | `state_own` | per *render position* — a row of a list, a mounted view, a `comp()` call site |
| `useState` (app-wide) | `state` | one per translation unit; right for the app's own state |
| `useReducer` | `state` + an `action` | the action is a named function, not a switch over string types |
| `useMemo` | `computed` / `computed_own` | topological, runs once per batch |
| `useEffect` | `effect_own` + `mount_own` | no closure: the effect body is file-scope C, started where the view draws it |
| `useEffect` cleanup | `on_cleanup` | registered per run, and when the position dies |
| props | `PART(Name, (title, Text), …)` | typed arguments the compiler checks at every call site |
| `children` | `WKids` + `kids_of(body, ctx)` | children are created by the parent — same rule as React's |
| context | the view's context struct | `render(Name, .section = sec, …)` fills it by field name, checked by the compiler |
| JSX | `el("div") { … }` | markup in C: attributes are typed macros, a wrong type is a compile error |
| `className` | `class_()` / `class_fmt()` | `class_fmt("row row-%d", i)` formats; `class_("50% off")` is a value, not a format |
| `key={id}` lists | `for_each(item, rows)` | keyed by the model's `.pk`; the action receives the item, never a stale pointer |
| conditional render | `if` in the view | a branch that appears or disappears is a supported render; `show()` is the no-node toggle |
| `useRef` + focus | `named()` + `focus_hook()` | hooks by name; `reveal_hook()` scrolls into view |
| controlled input | `el("input") { bind_str(sig); }` | one line, no matching line in `client_main` |
| Suspense / loading states | `WRes` states | `res_loading` / `res_failed` / `res_ensure` — four states, no wrappers |
| error boundary | `mw_recover()` + `try` | a panic is a 500 with the request named; a `fail()` is a value |
| react-router | `route("/todos/:id", …)` | the same path syntax the server router uses; `push_url` / `on_nav` |
| fetch / SWR / TanStack Query | `webc_get` / `res_get_json` | a verb, not a shim: the bridge is the only code that touches `fetch` |
| Server Components | the shared file set | *one* view compiles for both halves; there is no client/server boundary to mark — `webc.json`'s file sets are it |
| `"use client"` | (inverted) | server-only names (`db_*`, `req`, `body`) are simply not declared in a shared file — the half is a compile error, not a directive |
| Server Actions | `action()` + `post_to()` | the route is the contract: it works with JavaScript off and gets sharper with it on |
| Next middleware | the middleware chain | `webc_use(mw_*)` in `webc_boot()`; the framework's chain is installed unless you say otherwise |
| Next route handlers | `GET()` / `POST()` | typed extractors, a `Response` value; file-scope because C23 removed nested functions |
| loaders (`useLoaderData`) | the route fills the context | `return render(Page, .site = site, .list = rows);` — a typo is a compiler error naming the field |
| `<Link>` | `href(url_to("story", …))` | named routes build their URLs; `push_url` navigates without a reload |
| Zod / schema validation | `WEBC_MODEL` qualifiers | `.max = 500` validates the JSON body, the form post *and* the client half — one declaration |
| tRPC / typed API | `webc openapi` | the boundary as OpenAPI 3.1, generated from the same facts |
| Testing Library | `webc test` | `ASSERT_HTML(r, "ul > li")` — the document, not substrings; `TestClient` is the browser with a cookie jar |
| Fast Refresh | the wasm swap | `webc dev` swaps the module under the page: DOM, signals, scroll and caret survive |
| DevTools | `WebC.diagnostics()` / `WebC.why(x, y)` | the ops stream and the click inspector, in the console |

## Solid, specifically

Fine-grained reactivity is where Solid and WebC agree most — and the one place WebC is *more*
explicit about the price:

- `createSignal` → `state` / `state_own` / `state_in`; the table in the [mental model] decides which.
- `createEffect` → `effect` / `effect_own`.
- `For` keyed → `for_each` — but the whole view runs again after an action and diffs; the *opt-out*
  is a **live binding**: `text_live` writes one node, and an `action_live` skips the render entirely.
  That is Solid's per-node update, declared at the node instead of bought everywhere.
- `createResource` → `WRes` + `res_get_json`, with `res_compute` for CPU-shaped work, chunked.

## A component, both spellings

The React you would write, then the WebC that is the same idea:

```jsx
function Stepper({ label }) {
  const [qty, setQty] = useState(0);
  return (
    <div className="stepper">
      <button onClick={() => setQty(qty - 1)}>-</button>
      <span>{qty}</span>
      <button onClick={() => setQty(qty + 1)}>+</button>
      <label>{label}</label>
    </div>
  );
}
```

```c
// src/client.c — the browser half: state, behaviour and the view it updates
#include <webc/web.h>

state_own(i64, qty, 0);              // one value per render position

PART(Stepper, (label, Text));
void Stepper(Text label) {
  el("div") { class_("stepper");
    el("button") { named("dec"); text("−"); }
    el("span")   { text_live("%lld", qty); }   // one node, written in place
    el("button") { named("inc"); text("+"); }
    text(label);
  }
}

action(inc) { set(qty, get(qty) + 1); }   // the clicked button's position
action(dec) { set(qty, get(qty) - 1); }

view(Order) {
  el("main") {
    comp(Stepper, "Chairs");   // its own qty
    comp(Stepper, "Tables");   // and this one's
  }
}

client_main {
  bind("inc", "click", inc);
  bind("dec", "click", dec);
  mount(Order, "#root");
  return 0;
}
```

One file where React has one component, and the difference is the contract: the view
names its elements (`named("inc")`), the client names the behaviour (`bind`), and
`webc check --wiring` refuses to let the two disagree. Two `comp(Stepper, …)` calls keep
separate quantities because the *call site* is the owner — no `key` needed, no re-mount
dance. In a fullstack app the same view lives in the shared half and the actions in the
client half; the [components] guide is the long version.

## What has no equivalent (and why that is the pitch)

- **No virtual DOM.** The browser half patches the real DOM through a line protocol you can read
  (`WebC.diagnostics().ops`); the server half renders the same view for the first paint and the
  client hydrates it — measured at zero created nodes.
- **No bundler, no npm.** `webc.json` names the files; `webc build` compiles each half; the
  standard library is opt-in headers whose byte cost is measured per module (`webc size --modules`).
- **Markup is C.** That is not an apology — a `class_()` takes a `str`, a `Text` or a number, and
  the wrong one is a compile error, not a hydration warning at 2am.
- **No suspense streaming.** The live answer is SSE: whole documents, coalesced, behind `live`
  rooms — the [real-time guide] is that story.
- **Strings belong to a scope.** `fmt()` lives in the request or the render; `persist()` is the one
  escape hatch. The [mental model] page makes this one idea do most of the work.

## Where to go next

- [The mental model] — five ideas, and everything else follows from them.
- [Quickstart] — ten minutes to a running app with a database.
- The [signals] and [components] guides for the state table in full.
- [Testing] — what a suite looks like when a handler is a function you can call.
