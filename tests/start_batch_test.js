// start_batch: the declarations in client_main must be in the batch start() reads back.
//
// The bug this pins was invisible to every other test: `webc_start()` runs client_main
// (whose w_flush() puts render + host ops into the command buffer), then RESETS the buffer
// and re-renders — and the re-render never drained the host-op buffer. So the start batch
// shipped DOM ops only, and every on_key / on_ready / webc_get a page declared sat in wasm
// memory until the first click delivered them. A page that was never clicked had dead keys
// and a stream that on_ready() opens but never opened.
//
// This test builds a real wasm module (the way `webc new --template=web` would), runs it
// under the real bridge in Node, and asserts the declarations are in batch #1 — the one
// start() reads — not in some later batch. Skips, with a sentence, when the wasm toolchain
// is not installed: the bridge tests cover the protocol; this covers the wiring.
const { execFileSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const CLI = path.join(ROOT, 'build', 'webc');

function fail(msg) {
  console.log('  FAIL ' + msg);
  process.exitCode = 1;
}

(function main() {
  console.log('bridge: client_main declarations are in the start batch');
  if (!fs.existsSync(CLI)) { console.log('  SKIP: no build/webc (run make cli)'); return; }

  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'webc-start-'));
  try {
    fs.mkdirSync(path.join(dir, 'src'), { recursive: true });
    fs.writeFileSync(path.join(dir, 'webc.json'), JSON.stringify({
      name: 'startbatch', mode: 'web', target: 'wasm', client: ['src/main.c'],
    }));
    fs.writeFileSync(path.join(dir, 'src', 'main.c'), `#include <webc/web.h>
#include <webc/host.h>

static void LegPage(WBuild *b, void *ctx) { (void)b; (void)ctx; }
route("/f/:number", LegPage); // the same table the server answers

action(pal) {}
action(booted) {}

// A subscription keeps its handler: the second frame must reach C exactly as the
// first did. A slot_settle() that nulls the handler before every call and restores it
// for no kind delivers one frame per page and then goes quiet.
static void on_tick(WJs r) { doc_title(r.json); }

// Back and forward arrive as a match against the route table, not a raw path.
action(navigate, WNav *nav) {
  doc_title(nav->matched ? nav_param(nav, "number") : str_lit("home"));
}

client_main {
  on_key("meta+k", pal);
  on_ready(booted);
  on_nav(navigate);
  webc_subscribe(str_lit("/stream"), str_lit("tick"), on_tick);
  return 0;
}
`);
    execFileSync(CLI, ['build', '--target', 'wasm'], { cwd: dir, stdio: 'pipe' });
    // The module is named after the project (startbatch.wasm), not app.wasm — that name
    // belongs to a serverful project's browser half.
    const wasmPath = fs.existsSync(path.join(dir, '.webc', 'app.wasm'))
      ? path.join(dir, '.webc', 'app.wasm')
      : path.join(dir, '.webc', 'startbatch.wasm');
    const wasm = fs.readFileSync(wasmPath);

    // The real bridge, a fake page, no DOM ops at all: this module declares and mounts
    // nothing, so whatever start() reads back is the declarations and nothing else.
    const WEBC = require(path.join(ROOT, 'assets', 'webc.js'));
    const el = {
      ownerDocument: {
        querySelectorAll: () => [],
        addEventListener: () => {},
      },
      querySelector: () => null,
      addEventListener: () => {},
      childNodes: [], children: [],
      firstChild: null,
    };
    globalThis.document = el.ownerDocument;
    // capture the bridge's popstate listener, the way the browser holds it
    globalThis.__pops = [];
    globalThis.addEventListener = (ev, fn) => { if (ev === 'popstate') globalThis.__pops.push(fn); };
    globalThis.location = { pathname: '/f/VA101' };
    // a fake EventSource: the test fires the frames itself
    globalThis.__esL = {};
    globalThis.EventSource = class {
      addEventListener(name, fn) { (globalThis.__esL[name] = globalThis.__esL[name] || []).push(fn); }
    };
    const batches = [];
    const realLog = console.log;
    console.log = (...a) => { const s = a.join(' '); if (s.startsWith('[webc] cmds len=')) batches.push(s); };
    globalThis.WEBC_DEBUG = true;
    const realFetch = globalThis.fetch;
    globalThis.fetch = async () => new Response(wasm, { headers: { 'content-type': 'application/wasm' } });

    WEBC.start({ el, wasm: '/app.wasm', hydrate: false }).then(() => {
      console.log = realLog;
      const first = batches[0] || '';
      if (!batches.length) return fail('start() read no batch at all');
      if (!/\bkey \d+ meta\+k/.test(first))
        return fail('the first batch has no key op — declarations wait for a click again\n    first: ' +
                    first.split('\n').slice(1, 4).join(' | '));
      if (!/\bready \d+/.test(first)) return fail('the first batch has no ready op');
      realLog('  ok   on_key and on_ready are in batch #1 (the one start() reads)');
      if (!/\bnet-sub \d+ \/stream tick/.test(first))
        return fail('the subscription declared in client_main is not in batch #1 either');
      // two frames, one second apart in content: if the handler was dropped after the
      // first (the bug), the title says the first frame's word
      const fire = (text) => globalThis.__esL.tick[0]({ type: 'tick', data: text });
      fire('"first"');
      fire('"second"');
      if (globalThis.document.title !== '"second"')
        return fail('the second tick did not reach the module (title=' +
                    JSON.stringify(globalThis.document.title) + ') — a subscription lost its handler');
      realLog('  ok   the second tick reached the module: a subscription keeps its handler');
      // a popstate: the address arrives as a route match, params read by name
      for (const fn of globalThis.__pops || []) fn();
      if (globalThis.document.title !== 'VA101')
        return fail('popstate did not deliver the route match (title=' +
                    JSON.stringify(globalThis.document.title) + ')');
      realLog('  ok   popstate delivered "/f/VA101" as nav_param(nav, "number")');
      globalThis.fetch = realFetch;
      delete globalThis.WEBC_DEBUG;
      delete globalThis.document;
    }, (e) => { console.log = realLog; fail('start() rejected: ' + e.message); });
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
})();