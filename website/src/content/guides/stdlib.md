WebC's floor is small on purpose. Everything else is an **opt-in header**: `#include <webc/table.h>` is
the whole install, the whole manifest and the whole cost. Nothing is in the bundle unless a file you
compile names it.

```sh
webc stdlib                  # the catalogue
webc stdlib overlay          # what one module is, and what it adds
webc stdlib overlay --css    # the stylesheet it expects
webc stdlib --weigh          # what each one costs, built
webc stdlib --used           # what this app already includes
```

## The shelf

| Module | Tier | What it is |
| --- | --- | --- |
| `ui` | markup | switch, segmented control, tab, meter, kbd · badge, stat, empty state, skeleton, icon button |
| `icons` | markup | stroke icons as data: the ones you draw are the ones that link |
| `table` | markup | sortable, paginated table: order without owning the rows |
| `overlay` | markup | dialog, drawer, menu, toast, tip — with the ARIA and the focus |
| `form` | markup | a form drawn from the model that validates it |
| `theme` | markup | design tokens and a dark edition that does not flash |
| `keys` | interaction | one table of shortcuts, bound and documented from the same rows |
| `motion` | interaction | enter animation and stagger, no script, reduced-motion honoured |
| `dnd` | interaction | pick a row up and put it down: the drop index, not the maths |
| `live` | interaction | rooms over SSE: whole documents, coalesced, a dead tab costs nothing |
| `csv` | data | in and out, quoted correctly, by header name |
| `search` | data | a search box that ANDs its words, escapes a typed `%`, and ranks the title |
| `i18n` | data | one app, two languages: catalogs, plurals, direction |
| `upload` | data | a file arrives and is checked: magic bytes, a name this side makes up |
| `time` | data | dates, ages and two-unit durations, UTC only and proud of it |
| `richtext` | data | HTML a user wrote, rendered through a policy: parsed, allow-listed, escaped |
| `ld` | data | structured data: the article, the site, the trail, the FAQ — one expression |
| `mail` | platform | a real email from one view: SMTP, or a file in the outbox |
| `jobs` | platform | work that outlives the request: a table, a lease, a retry |
| `limits` | platform | a rate limit that is one line, and a table that cannot grow |
| `pwa` | platform | installable and offline: manifest, worker, cache keyed by the build |

Taking **all twenty** costs 11.3 KB gzip on a floor of 33.9 KB — measured, not estimated, by
`webc stdlib --weigh`, which builds a probe per module. A typical app takes two or three.
`richtext` is 5.3 KB of that on its own: a parser and an allow-list in the browser half, paid only
by the page that renders a stored body in wasm.

## What a module looks like

They are not wrappers around a dependency. `overlay` is the five details that make a box on top
usable — `role`, `aria-modal`, a name, focus that moves into it after the batch, and a way out that
does not need a mouse:

```c
// src/client.c — a dialog, with its five details, once
#include <webc/web.h>
#include <webc/overlay.h>
#include <webc/icons.h>                    // aria_label()

state(bool, card_open, true);

static const WOverlay CARD = {"card_dlg",     // the panel's id: the focus target
                              "New card",     // its accessible name
                              "card_close"};  // the hook the scrim is named by

action(card_close) { set(card_open, false); }

view(Cards) {
  dialog(&CARD, get(card_open)) {          // nothing is emitted while it is closed
    el("h2") { text("New card"); }
    el("button") { class_("x"); named("card_close"); aria_label("Close"); }
  }
}

client_main {
  bind("card_close", "click", card_close);  // the scrim and the close button
  overlay_esc(card_close);                  // Escape, wherever focus is
  return 0;
}
```

The backdrop and the panel are **siblings**, not parent and child, because a scrim that wraps the
panel cannot tell a click outside from a click inside. That defect was found by an app (`examples/board`'s
new-card dialog submitted nothing and closed instead), and the fix belongs in the module.

`keys` is one table that both binds and documents:

```c
// src/client.c — one table that binds and documents
#include <webc/web.h>
#include <webc/keys.h>

state(bool, palette_open, false);
state(Text, query, "");

action(new_card)     { set(query, ""); set(palette_open, true); }
action(focus_search) { set(palette_open, true); }
action(close_panel)  { set(palette_open, false); }

static const WCmd cmds[] = {
  K("New card", "n",      new_card),
  K("Search",   "/",      focus_search),
  K("Close",    "escape", close_panel),
};

view(App) {
  el("main") {
    el("h1") { text("Cards"); }
    if (get(palette_open)) { el("p") { text("type a query"); } }
  }
}

client_main {
  keys_bind(cmds);        // every row, bound
  mount(App, "#root");
  return 0;
}
```

`keys_help(cmds, box, open)` draws the sheet — a dialog of `<kbd>` rows from the same array, so a
shortcut cannot be bound and left undocumented.

## The module that is a security boundary

`richtext` is the one module that is not about drawing something: it is the third case for authored
bytes, beside `text()` (copy) and `markup()` (bytes you wrote) — HTML a **user** wrote, which is what
every CMS, wiki and comment field stores. It parses once and re-emits through `el()`/`attr()`/`text()`,
so escaping is not a second implementation:

```c
// src/posts.c — a stored body, rendered through the policy, plus what the page asks
#include <webc/server.h>
#include <webc/richtext.h>

WEBC_MODEL(Post, (id, i64, .pk = true), (body, Text))
Vec(Post)

static void article(str body) {
  i32 words;
  WRichHeading toc[16];
  head_title("The last ferry");
  wc_rich_stamp_ids(body);             // ids on every heading, idempotent
  wc_rich_headings_into(body, toc, 16, &words);
  el("article") { rich(body); }        // the fragment, sanitised on the way out
  text("%d min read", wc_rich_reading_time(body));
}
```

A tag outside the policy is dropped and its words stay; `script`, `style`, `iframe`, `svg`, `math` and
`form` go *with their contents*; every URL is entity-decoded before it is judged, so
`java&#115;cript:` is refused; `srcset` is refused whole if one candidate fails; an external link gets
`rel="noopener noreferrer"`. `rich_with(s, .allow = RICH_TIGHT, .no_images = true)` is the policy for
comments. It sanitises a *fragment*, on the way out — what the browser posted is still the app's
call — and it is a shelf module rather than a floor feature so an app that never stores HTML pays
nothing for the parser it would not use.

## The module that is a contract with a crawler

`ld` writes the JSON-LD a page states about itself, and its whole point is what it refuses to write.
A block built with `fmt()` cannot *omit* a field, so a draft ends up claiming
`"datePublished":""`, and its nesting is retyped per page:

```c
// src/article.c — the same facts as the <head>, in the shape a crawler reads
#include <webc/server.h>
#include <webc/ld.h>

WEBC_MODEL(Post, (id, i64, .pk = true), (title, Text), (author, Text))
Vec(Post)

static void head_of(Post *p, const char *desc, const char *url) {
  head_title(p->title);
  head_ld(ld_blog_posting(.url = url,
                          .headline = p->title,
                          .description = desc,
                          .author = p->author,
                          .publisher = "VictorTrades",
                          .published = "2026-10-14T20:31:00Z",
                          .word_count = 900));
}
```

`author` becomes a `Person`, `publisher` an `Organization`, `image` an `ImageObject`, and a field left
null is absent rather than empty. `head_ld()` is the door, and it is not sugar for `head_html()`: a
`<script>` body is JSON *and* HTML, so a title containing `</script>` would close the element — the
door escapes `<`, `>` and `&` as `\uXXXX`, which is the same string to a JSON reader and cannot
end a tag. `webc lint` names the hand-built version.

`ld_breadcrumbs`, `ld_faq`, `ld_graph`, `ld_website`, `ld_organization` and `ld_person` cover the
rest of what a publishing site emits; `ASSERT_LD(res, "author.name", …)` is how a test holds it to
its word.

## The honest entry

`table` renders a header, a pager, sorting and empty states without owning your rows — and it is the
one module with a gap: it composes the chrome but not the body, so a list of rows is still drawn by a
`for_each` in your view. It is on the list. `form` is the one to read for how a module is supposed to
feel: the input's kind, bounds, label, error and `aria-describedby` all come from the model
declaration, so adding a field to a struct adds a field to the form.

## CSS belongs to the module

Every module ships the stylesheet it expects: `webc stdlib <module> --css` prints it, and you either
copy it into `public/` or `@import` it. The markup a module emits carries class names, not inline
styles, so a design system can restyle it without forking.

`theme` is the exception that proves the rule: it exists to make the cascade right — the light block,
then `:root[data-theme=dark]`, then `@media (prefers-color-scheme:dark)` — so the first paint is
correct and nothing flashes.

## See also

- Reference [§12 The shelf](../reference/frontend-dom-and-ssr.html) — each module in detail.
- `examples/stdlib` — every module on one page, runnable.
