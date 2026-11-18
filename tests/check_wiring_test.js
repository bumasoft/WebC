// check_wiring: the wiring and style notes name the bugs the compiler cannot see.
//
// The last three rounds' late bugs were all stringly-typed wiring — a js() call to a
// function that never existed, a bind whose hook only sometimes exists, a control bound
// before it rendered — and none of them could be caught by a compiler, because the
// wiring IS strings. `webc check --wiring` reconciles the tables (P2) and the class
// contract (P10). This fixture drives the real CLI against the real historical bugs and
// asserts each is named; a clean fixture must be clean.
const { execFileSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const CLI = path.resolve(__dirname, '..', 'build', 'webc');

function fail(msg) {
  console.log('  FAIL ' + msg);
  process.exitCode = 1;
}

(function main() {
  console.log('check: the wiring and style notes name the things that silently break');
  if (!fs.existsSync(CLI)) { console.log('  SKIP: no build/webc (run make cli)'); return; }

  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'webc-wiring-'));
  try {
    fs.mkdirSync(path.join(dir, 'src'), { recursive: true });
    fs.mkdirSync(path.join(dir, 'public'), { recursive: true });
    fs.writeFileSync(path.join(dir, 'webc.json'), JSON.stringify({
      name: 'wiring', server: ['src/routes.c'], shared: ['src/view.c', 'src/parts.c'],
      client: ['src/client.c'],
    }));
    // the view: a control nobody bound, a class no stylesheet answers, a field spelling
    // that drifted between the halves
    fs.writeFileSync(path.join(dir, 'src', 'view.c'), `
#include <webc/web.h>
void the_view(WBuild *b) {
  (void)b;
  el("button") { named("close_panel"); class_("panel-shut"); }
  el("input") { attr("data-field", "dest"); }
  el("button") { named("boost"); class_("boosted"); }
  el("button") { named("never_bound"); }   // declared, no bind answers it
  // on_click(dead_code) — this sentence is prose, not a wiring: the action below is still dead
  // named("ghost_control") is prose too: no such control exists, and no note may invent one
}
`);
    // the client: a bind to a name no view carries (the historical error), a key spec
    // with a typo, an action nothing wires, a field the view never marked
    fs.writeFileSync(path.join(dir, 'src', 'client.c'), `
#include <webc/web.h>
action(pick) {}
action(boost) {}
action(reroute) {}
action(pal_open) {}
action(dead_code) {}                       // defined, nothing wires it
state(i64, tick, 0);                      // declared, wired by nothing
action(bump) { set(tick, get(tick) + 1); }
client_main {
  bind("row", "click", pick);            // no view declares "row"
  bind("boost", "click", boost);
  bind("close_panel", "click", pick);
  on_key("mata+k", pal_open);            // the modifier no bridge knows
  bind_field("destination", reroute);    // the view says "dest"
  bind("tick", "click", bump);
  return 0;
}
`);
    // a list: the live binding is declared once per row and resolves once per page, so
    // `state_own` is the declaration that would make the rows differ — the check says so
    fs.appendFileSync(path.join(dir, 'src', 'view.c'), `
Vec(i64)
static Veci64 g_rows;
state(i64, open_row, 0);      // one per page, read inside the list below
state_own(i64, qty, 0);       // one per row: the check must not confuse the two
static void the_list(WBuild *b) {
  (void)b;
  el("ul") {
    for_each(r, g_rows) {
      el("li") {
        text_live("%lld", open_row);
        text_live("%lld", qty);
      }
    }
  }
}
`);
    fs.appendFileSync(path.join(dir, 'src', 'client.c'), `
action(open_all) { set(open_row, 1); set(qty, get(qty) + 1); }
`);

    // a part that declares render-owned state: two calls in one function draw into one
    // position, so they share it — the note names the call sites, and a comp()'d pair is quiet
    fs.writeFileSync(path.join(dir, 'src', 'parts.h'), `
#ifndef FIXTURE_PARTS_H
#define FIXTURE_PARTS_H
#include <webc/web.h>
PART(Stepper, (label, Text))
#endif
`);
    fs.writeFileSync(path.join(dir, 'src', 'parts.c'), `
#include "parts.h"
state_own(i64, steps, 0);
void Stepper(Text label) {
  el("div") { text("%s=%lld", label, (long long)get(steps)); }
}
`);
    fs.appendFileSync(path.join(dir, 'src', 'view.c'), `
#include "parts.h"
void the_picker(WBuild *b) {
  (void)b;
  el("div") { Stepper("chairs"); Stepper("tables"); }   // one position, two calls
}
void the_bar(WBuild *b) {
  (void)b;
  el("div") { comp(Stepper, "left"); comp(Stepper, "right"); }  // two positions: quiet
}
`);

    // the stylesheet: a selector no markup will ever carry
    fs.writeFileSync(path.join(dir, 'public', 'style.css'),
                     '.panel-shut { color: red; }\n.gone-forever { color: blue; }\n');

    // the buggy fixture exits 1 (notes and errors both count); the words are in stdout
    let out = '';
    try {
      out = execFileSync(CLI, ['check', '--wiring', '--verbose'], {
        cwd: dir, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'],
      });
    } catch (e) {
      out = (e.stdout || '') + (e.stderr || '');
    }
    const wants = [
      ['bind to a name no view declares', 'the row bind'],
      ['a name no binding answers', 'close_panel or boost, seen from the other side'],
      ['an on_key spec the bridge would never match', 'mata+k'],
      ['an action nothing wires', 'reroute'],
      ['bind_field to a name no input carries', 'destination vs dest'],
      ['a class no stylesheet answers', 'boosted'],
      ['a selector no markup ever carries', 'gone-forever'],
      ['a signal written and never read', 'the leftover tick bump'],
      ['a live binding on a page-wide signal inside a list', 'the one value every row shows'],
      ['is called 2 times in the_picker()', 'a part that keeps state, called twice in one position'],
    ];
    // …and the fix it names is the declaration that makes them differ — one note, for the
    // page-wide declaration, and none for the `state_own` beside it in the same list
    const fixes = out.split('\n').filter((l) => l.includes('state_own(i64,'));
    if (fixes.length !== 1) fail('the list note does not name state_own as the fix:\n' + out);
    else if (fixes[0].includes('qty'))
      fail('a state_own declaration was reported as one value for every row:\n' + out);
    // and the prose above must not be counted as a wiring: a comment that mentions an action
    // makes it read as reachable, which is how a dead handler stays dead for a year
    if (!out.includes('dead_code'))
      fail('a comment mentioning on_click(x) masked the dead action:\n' + out);
    if (out.includes('ghost_control'))
      fail('a comment mentioning named(x) invented the control:\n' + out);
    for (const [needle, why] of wants)
      if (!out.includes(needle)) fail('the check did not name: ' + why + ' (wanted "' + needle + '")\n' +
        out.split('\n').filter((l) => l.trim().startsWith('error') || l.trim().startsWith('note')).join('\n'));
    if (!out.includes('mata+k')) fail('the bad key spec is not named with its own name');
    // …and exactly once, from the plain calls: the comp()'d pair beside it is two positions
    // already, and a note there would teach the reader to ignore the note
    const stepper = out.split('\n').filter((l) => l.includes('Stepper is called'));
    if (stepper.length !== 1) fail('the component note fired ' + stepper.length + ' time(s):\n' + out);

    // the clean fixture: everything declared, everything wired, everything styled — and the
    // hook and its class live in a *header*, which is where the multi-file pattern puts them.
    // A check that reads only the .c files calls that bind "a name no view declares".
    fs.writeFileSync(path.join(dir, 'src', 'shared.h'), `
#ifndef FIXTURE_SHARED_H
#define FIXTURE_SHARED_H
#define ROW() named("row"); class_("row");
// A per-row declaration both halves draw from: one header, one declaration site, so the
// view and the action address the same slot.
Vec(i64)
extern Veci64 g_rows;
state_own(i64, row_qty, 0);
#endif
`);
    fs.writeFileSync(path.join(dir, 'src', 'view.c'), `
#include <webc/web.h>
#include "shared.h"
void the_view(WBuild *b) {
  (void)b;
  el("button") { ROW(); text("%lld", get(shown)); }
  el("p") { text_live("%lld", live_one); }   // a live binding is a reader of its signal too
  el("input") { attr("data-field", "dest"); }
  el("ul") { for_each(r, g_rows) { el("li") { text_live("%lld", row_qty); } } }
}
`);
    fs.appendFileSync(path.join(dir, 'src', 'client.c'), '');
    fs.writeFileSync(path.join(dir, 'src', 'rows.c'), `
#include <webc/web.h>
#include "shared.h"
Veci64 g_rows;                            // the view and the action share one declaration
`);
    fs.writeFileSync(path.join(dir, 'src', 'client.c'), `
#include <webc/web.h>
#include "shared.h"
state(i64, shown, 0);                     // read in the view: not a leftover
state(i64, live_one, 0);                  // read by a text_live node, which the check counts
action(pick) { set(live_one, get(live_one) + 1); set(row_qty, get(row_qty) + 1); }
action(field_dest) {}
action(tab_pick) {}                        // wired by a bind whose hook name is computed
static const char *row_hook(long long i) { return i ? "row" : "row_all"; }
client_main {
  bind("row", "click", pick);
  bind_field("dest", field_dest);
  for (long long i = 0; i < 3; i++) bind(row_hook(i), "click", tab_pick);
  return 0;
}
`);
    fs.writeFileSync(path.join(dir, 'public', 'style.css'), '.row { color: red; }\n');
    const clean = execFileSync(CLI, ['check', '--wiring'], {
      cwd: dir, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'],
    });
    if (!clean.includes('clean')) fail('the clean fixture is not clean:\n' + clean);
    console.log('  ok   nine bugs named, prose is not a wiring, the list note names state_own, the',
                'component note counts call sites, clean fixture clean');
  } catch (e) {
    fail('the CLI run itself failed: ' + (e.stderr || e.message).slice(0, 400));
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
})();