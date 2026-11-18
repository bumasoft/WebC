// bridge_test.js — runs the real C command stream through assets/webc.js against a
// fake document. Proves the protocol: mount builds the tree, a patch touches only
// the text node that changed, and hydration adopts SSR markup without creating
// a single element.
'use strict';
const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');

// ---------------------------------------------------------------- fake DOM

class Node_ {
  constructor(doc, type, tag) {
    this.ownerDocument = doc;
    this.nodeType = type;          // 1 element, 3 text
    this.tagName = tag ? tag.toUpperCase() : undefined;
    this.childNodes = [];
    this.parentNode = null;
    this._attrs = new Map();
    this._text = '';
    this._events = {};
    this.__webc = undefined;
  }
  get children() { return this.childNodes.filter((n) => n.nodeType === 1); }
  get attributes() { return Array.from(this._attrs, ([name, value]) => ({ name, value })); }
  appendChild(n) {
    if (n.parentNode) n.parentNode.removeChild(n);
    this.childNodes.push(n);
    n.parentNode = this;
    return n;
  }
  insertBefore(n, ref) {
    // a DocumentFragment is inserted as its children, not as itself — the `raw` op depends on it
    const parts = n && n.nodeType === 11 ? n.childNodes.slice() : [n];
    let i = ref ? this.childNodes.indexOf(ref) : -1;
    if (i < 0) i = this.childNodes.length;
    for (const part of parts) {
      if (part.parentNode) part.parentNode.removeChild(part);
      this.childNodes.splice(i++, 0, part);
      part.parentNode = this;
    }
    if (n && n.nodeType === 11) n.childNodes.length = 0;
    return n;
  }
  removeChild(n) {
    const i = this.childNodes.indexOf(n);
    if (i >= 0) this.childNodes.splice(i, 1);
    n.parentNode = null;
    return n;
  }
  setAttribute(name, value) {
    // the browser's own rule: a nameless attribute is an exception, not a no-op
    if (!/^[^\s"'>/=]+$/.test(String(name)))
      throw new Error('Failed to execute \'setAttribute\' on \'Element\': \'' + name +
                      '\' is not a valid attribute name.');
    this._attrs.set(name, String(value));
  }
  getAttribute(name) { return this._attrs.has(name) ? this._attrs.get(name) : null; }
  removeAttribute(name) { this._attrs.delete(name); }
  addEventListener(ev, fn, opts) {
    const capture = opts === true || !!(opts && opts.capture);
    const bag = capture ? (this._capEvents = this._capEvents || {}) : this._events;
    (bag[ev] = bag[ev] || []).push(fn);
  }
  // Capture from the root down, then bubble from the target up, then the document — enough of
  // the real algorithm for both per-element listeners and the bridge's delegated ones.
  dispatch(ev, event) {
    const e = event || { type: ev, defaultPrevented: false, preventDefault() { this.defaultPrevented = true; } };
    e.type = e.type || ev;
    e.target = this;
    const chain = [];
    for (let n = this; n; n = n.parentNode) chain.unshift(n);
    // the document is the root of the tree: it sees the capture phase first and the bubble
    // phase last, which is where the bridge's delegated listener lives
    fire_(this.ownerDocument, ev, e, true);
    for (const n of chain) fire_(n, ev, e, true);
    for (let i = chain.length - 1; i >= 0; i--) fire_(chain[i], ev, e, false);
    fire_(this.ownerDocument, ev, e, false);
    return e;
  }
  get textContent() {
    if (this.nodeType === 3) return this._text;
    return this.childNodes.map((n) => n.textContent).join('');
  }
  set textContent(v) {
    if (this.nodeType === 3) { this._text = String(v); this.ownerDocument.textWrites++; return; }
    this.childNodes = [];
    if (v !== '') this.appendChild(this.ownerDocument.createTextNode(String(v)));
  }
}

function fire_(n, ev, e, capture) {
  if (!n) return;
  const bag = capture ? n._capEvents : n._events;
  for (const fn of (bag && bag[ev]) || []) fn(e);
}

class Document {
  constructor() {
    this.creates = 0;
    this.textWrites = 0;
    this.byId = new Map(); // what querySelector('#x') finds
    this._events = {};
    this._capEvents = {};
  }
  addEventListener(ev, fn, opts) {
    const capture = opts === true || !!(opts && opts.capture);
    const bag = capture ? this._capEvents : this._events;
    (bag[ev] = bag[ev] || []).push(fn);
  }
  querySelector(sel) {
    if (sel && sel[0] === '#') return this.byId.get(sel.slice(1)) || null;
    return null;
  }
  createElement(tag) { this.creates++; return new Node_(this, 1, tag); }
  createElementNS(ns, tag) {
    this.creates++;
    const n = new Node_(this, 1, tag);
    n.namespaceURI = ns;
    return n;
  }
  createTextNode(t) { this.creates++; const n = new Node_(this, 3); n._text = String(t); return n; }
  createComment(t) {
    const n = new Node_(this, 8);
    n._text = String(t);
    this.comments = (this.comments || 0) + 1;
    return n;
  }
  createRange() { return new Range_(this); }
}

// Just enough of a Range for the `raw` op: the bridge puts a comment where the markup goes and
// asks the range for a fragment at that spot. `createContextualFragment` understands a flat
// sequence of elements, self-closing elements and text — which is all a raw() slot carries.
class Range_ {
  constructor(doc) { this.doc = doc; this.before = null; this.contents = null; }
  setStartBefore(n) { this.before = n; }
  selectNodeContents(n) { this.contents = n; }
  collapse() {}
  createContextualFragment(markup) {
    const frag = new Node_(this.doc, 11, '#fragment');
    const re = /<([a-zA-Z][\w-]*)((?:\s+[\w:-]+="[^"]*")*)\s*\/>|<([a-zA-Z][\w-]*)((?:\s+[\w:-]+="[^"]*")*)\s*>([\s\S]*?)<\/\3>|([^<]+)/g;
    let m;
    while ((m = re.exec(String(markup)))) {
      if (m[1]) {                                   // <path d="…"/>
        const el = new Node_(this.doc, 1, m[1]);
        for (const at of m[2].matchAll(/([\w:-]+)="([^"]*)"/g)) el.setAttribute(at[1], at[2]);
        frag.appendChild(el);
      } else if (m[3]) {                            // <svg …>…</svg>
        const el = new Node_(this.doc, 1, m[3]);
        for (const at of m[4].matchAll(/([\w:-]+)="([^"]*)"/g)) el.setAttribute(at[1], at[2]);
        const inner = m[5].trim();
        if (inner) el.appendChild(this.doc.createTextNode(inner));
        frag.appendChild(el);
      } else if (m[6] && m[6].trim()) {
        frag.appendChild(this.doc.createTextNode(m[6]));
      }
    }
    return frag;
  }
}

// ---------------------------------------------------------------- harness

let failures = 0;
function ok(cond, msg) {
  if (cond) { console.log('  ok   ' + msg); return; }
  failures++;
  console.log('  FAIL ' + msg);
}
function eq(a, b, msg) {
  ok(a === b, msg + (a === b ? '' : ` (got ${JSON.stringify(a)}, want ${JSON.stringify(b)})`));
}

function loadStreams() {
  // `webc test` builds it next to the suites; `make test` too.
  const candidates = [
    path.join(__dirname, '..', '.webc', 'tests', 'dom_batch_emit'),
    path.join(__dirname, '..', 'build', 'tests', 'dom_batch_emit'),
  ];
  const bin = candidates.find((p) => fs.existsSync(p));
  if (!bin) {
    console.error('dom_batch_emit missing; run `webc test` or `make test`');
    process.exit(1);
  }
  const out = execFileSync(bin, { encoding: 'utf8' });
  const parts = {};
  let cur = null;
  for (const line of out.split('\n')) {
    const m = /^=== (\w+)$/.exec(line);
    if (m) { cur = m[1]; parts[cur] = ''; continue; }
    if (cur) parts[cur] += line + '\n';
  }
  return parts;
}

// The bridge reaches the page's `document` for the doc verbs — a hook is a `data-hook`, an
// id, or a `data-field`, and only the page knows which. One stub, and a test that needs a
// node to be found by name puts it in `page.find`.
const page = { find: null };

function loadBridge() {
  const src = fs.readFileSync(path.join(__dirname, '..', 'assets', 'webc.js'), 'utf8');
  const mod = { exports: {} };
  new Function('module', 'exports', 'document', 'crypto', src)(
    mod, mod.exports,
    { querySelector: (sel) => (page.find ? page.find(sel) : null) },
    { getRandomValues() {} },
  );
  return mod.exports;
}

// ---------------------------------------------------------------- tests

const streams = loadStreams();
const WEBC = loadBridge();

console.log('bridge: mount');
{
  const doc = new Document();
  const root = new Node_(doc, 1, 'div');
  WEBC.hydrate(root, streams.mount);
  eq(root.children.length, 1, 'one child element');
  eq(root.children[0].tagName, 'DIV', 'div created');
  eq(root.children[0].getAttribute('class'), 'app', 'class applied');
  eq(root.children[0].children[0].tagName, 'H1', 'h1 created');
  eq(root.children[0].children[0].textContent, 'Clicks: 0', 'text rendered');
  eq(root.children[0].children[1].tagName, 'BUTTON', 'button created');
  eq(root.children[0].children[1].textContent, '+1', 'button label');
}

console.log('bridge: patch touches one node');
{
  const doc = new Document();
  const root = new Node_(doc, 1, 'div');
  WEBC.hydrate(root, streams.mount);
  doc.textWrites = 0;
  doc.creates = 0;
  const lines = streams.patch.split('\n').filter(Boolean);
  eq(lines.length, 1, 'patch is a single command');
  WEBC.apply(streams.patch);
  eq(doc.creates, 0, 'no nodes created');
  eq(doc.textWrites, 1, 'exactly one text write');
  eq(root.children[0].children[0].textContent, 'Clicks: 41', 'counter text updated');
}

console.log('bridge: a live binding writes one node and creates nothing');
{
  const doc = new Document();
  const root = new Node_(doc, 1, 'div');
  WEBC.hydrate(root, streams.live_mount);
  eq(root.children[0].children[0].textContent, '0', 'initial value rendered with the mount');
  doc.textWrites = 0;
  doc.creates = 0;
  const lines = streams.live_op.split('\n').filter(Boolean);
  eq(lines.length, 1, 'one op for one changed node, with no render behind it');
  eq(lines[0].startsWith('text '), true, 'and it is a text op, not a create');
  WEBC.apply(streams.live_op);
  eq(doc.creates, 0, 'no nodes created');
  eq(doc.textWrites, 1, 'exactly one text write');
  eq(root.children[0].children[0].textContent, '12', 'value written in place');
  eq(root.children[0].children[1].textContent, 'static', 'the sibling never moved');
  eq(WEBC.diagnostics().badOps, 0, 'the bridge understood the op');
}

console.log('bridge: hydration that cannot adopt drops the stale nodes instead of doubling');
{
  const doc = new Document();
  const root = new Node_(doc, 1, 'div');
  const html = /<ul[\s\S]*<\/ul>/.exec(streams.html)[0];
  const ul = doc.createElement('ul');
  ul.setAttribute('data-wc', '1');
  let n = 0;
  for (const m of html.matchAll(/<li data-wc="(\d+)">([^<]*)<\/li>/g)) {
    const li = doc.createElement('li');
    // the third item lost its anchor: the server's tree and this one cannot line up
    if (n++ < 2) li.setAttribute('data-wc', m[1]);
    li.appendChild(doc.createTextNode(m[2]));
    ul.appendChild(li);
  }
  root.appendChild(ul);
  doc.creates = 0;
  WEBC.hydrate(root, streams.list);
  // one list, three items: the server's three plus the ones it had to create would have
  // been six — the "48 rows where the server sent 24" symptom
  eq(root.children.length, 1, 'one list in the container, not two trees');
  eq(root.children[0].children.length, 3, 'three items, not six');
  eq(WEBC.diagnostics().hydrated, false, 'the failed adoption is reported as such');
}

console.log('bridge: a malformed op costs one attribute, not the batch');
{
  const doc = new Document();
  const root = new Node_(doc, 1, 'div');
  const before = WEBC.diagnostics().badOps;
  // op 4 is the shape that took out two thirds of a real page: an attribute pair whose
  // name came back empty (`attr 247  `). The batch has to survive it.
  const stream = [
    'mount 0 replace #root',
    'create 1 0 main',
    'create 2 1 path',
    'attr 2  M0 0 L1 1',
    'attr 2 class body',
    'create 3 1 path',
    'attr 3 class tail',
    'text 3 after',
  ].join('\n') + '\n';
  let threw = null;
  try { WEBC.hydrate(root, stream); } catch (e) { threw = e; }
  eq(threw, null, 'no exception escaped the batch');
  const main = root.children[0];
  eq(main && main.children.length, 2, 'every element after the bad op exists');
  eq(main && main.children[0].getAttribute('class'), 'body', 'a good attr on the same node still landed');
  eq(main && main.children[1].textContent, 'after', 'the tail of the batch still applied');
  const d = WEBC.diagnostics();
  eq(d.badOps - before, 1, 'exactly one bad op was counted');
  ok(!!d.lastBad && d.lastBad.indexOf('attr 2') === 0, 'the bad op text is quoted back');
}

console.log('bridge: hydration adopts SSR markup');
{
  const doc = new Document();
  const root = new Node_(doc, 1, 'div');
  // build what the server would have sent for the list view
  const ssr = streams.lines || '';
  const html = /<ul[\s\S]*<\/ul>/.exec(streams.html)[0];
  const ul = doc.createElement('ul');
  ul.setAttribute('data-wc', '1');
  for (const m of html.matchAll(/<li data-wc="(\d+)">([^<]*)<\/li>/g)) {
    const li = doc.createElement('li');
    li.setAttribute('data-wc', m[1]);
    li.appendChild(doc.createTextNode(m[2]));
    ul.appendChild(li);
  }
  root.appendChild(ul);
  doc.creates = 0;
  WEBC.hydrate(root, streams.list);
  eq(doc.creates, 0, 'adopted every node, created none');
  eq(root.children[0].children.length, 3, 'three items');
  eq(root.children[0].children[1].textContent, 'build framework', 'item text kept');
  void ssr;
}

console.log('bridge: a raw() created after adoption still inserts its markup');
{
  // Hydration is the first batch only. After it, `raw` has to INSERT markup that is not
  // "already in the document". Left believing it was still adopting, the bridge records a
  // position for every later raw() and inserted nothing — the palette's search glyph, an
  // aircraft glyph, a toast icon all came up empty, silently.
  const doc = new Document();
  const root = new Node_(doc, 1, 'div');
  WEBC.hydrate(root, 'create 1 0 p\ntext 1 adopted\n');
  WEBC.feed('create 2 0 span\nraw 3 2 <svg viewBox="0 0 1 1"><path d="M0 0"/></svg>\n');
  const span = root.children[1];
  eq(span && span.tagName, 'SPAN', 'the later patch created its element');
  ok(span.childNodes.length > 0, 'the later raw() put markup in the document');
  eq(span.children.length && span.children[0].tagName, 'SVG', 'and it is the markup that was sent');
}

console.log('bridge: escaped values');
{
  const doc = new Document();
  const root = new Node_(doc, 1, 'div');
  WEBC.hydrate(root, 'create 1 0 p\ntext 1 a\\nb  with  spaces\n');
  eq(root.children[0].textContent, 'a\nb  with  spaces', 'newline and spaces survive');
}

console.log('bridge: svg subtrees get a namespace');
{
  const doc = new Document();
  const root = new Node_(doc, 1, 'div');
  WEBC.hydrate(root, 'create 1 0 svg\nattr 1 viewBox 0 0 10 10\ncreate 2 1 path\nattr 2 d M 0,0 L 1,1\n');
  const svg = root.children[0];
  eq(svg.tagName, 'SVG', 'svg created');
  eq(svg.namespaceURI, 'http://www.w3.org/2000/svg', 'svg is in the svg namespace');
  eq(svg.children[0].namespaceURI, 'http://www.w3.org/2000/svg', 'a child inherits it');
  eq(svg.getAttribute('viewBox'), '0 0 10 10', 'viewBox is set, case and all');
}

console.log('bridge: events reach the host');
{
  const doc = new Document();
  const root = new Node_(doc, 1, 'div');
  let got = null;
  globalThis.webcDispatch = (id, ev, payload, reply) => { got = { id, ev, payload }; reply(''); };
  WEBC.hydrate(root, streams.mount);
  root.children[0].children[1].dispatch('click');
  ok(got && got.id === 4 && got.ev === 'click', 'click dispatched with node id');
  delete globalThis.webcDispatch;
}

console.log('bridge: a re-created element keeps its binding');
{
  const doc = new Document();
  const root = new Node_(doc, 1, 'div');
  let hits = 0;
  globalThis.webcDispatch = (id, ev) => { if (id === 2 && ev === 'click') hits++; };
  const mount = ['mount 0 replace #root', 'create 1 0 div', 'create 2 1 button',
    'listen 2 click'].join('\n') + '\n';
  WEBC.hydrate(root, mount);
  const before = root.children[0].children[0];
  before.dispatch('click');
  eq(hits, 1, 'the button fires once');

  // A patch that replaces the element object without repeating the bind: this is the shape
  // that made a real button work, then not work, then work again — the listener lived on
  // the element the render happened to attach it to.
  WEBC.apply(['remove 2', 'create 2 1 button'].join('\n') + '\n');
  const after = root.children[0].children[0];
  ok(after !== before, 'the element really was replaced');
  after.dispatch('click');
  eq(hits, 2, 'the new element fires the same binding');
  delete globalThis.webcDispatch;
}

console.log('bridge: a click on a child reaches the row that asked for it');
{
  const doc = new Document();
  const root = new Node_(doc, 1, 'div');
  let got = null;
  globalThis.webcDispatch = (id, ev) => { got = { id, ev }; };
  WEBC.hydrate(root, ['mount 0 replace #root', 'create 1 0 div', 'create 2 1 button',
    'listen 2 click', 'create 3 2 span'].join('\n') + '\n');
  let events = 0;
  globalThis.webcDispatch = (id, ev) => { events++; got = { id, ev }; };
  root.children[0].children[0].children[0].dispatch('click');
  eq(events, 1, 'exactly one dispatch, not one per ancestor');
  ok(got && got.id === 2, 'the button is the element that owns the binding');
  delete globalThis.webcDispatch;
}

console.log('bridge: a listen-op dragover is cancelled — without it the browser refuses the drop');
{
  // The snap-back bug: bind() events travel the listen-op path, and only that path's
  // fire() decides whether the module owns the default. A dragover it does not cancel
  // means no drop ever fires under a real mouse, and the card flies home on release.
  const doc = new Document();
  const root = new Node_(doc, 1, 'div');
  let got = null;
  globalThis.webcDispatch = (id, ev) => { got = { id, ev }; };
  WEBC.hydrate(root, ['mount 0 replace #root', 'create 1 0 article', 'listen 1 dragover'].join('\n') + '\n');
  const e = root.children[0].dispatch('dragover');
  ok(!!e.defaultPrevented, 'the dragover was cancelled (this is what allows a drop)');
  ok(got && got.id === 1 && got.ev === 'dragover', 'the drag handler ran');
  delete globalThis.webcDispatch;
}

console.log('bridge: submit is taken over, once');
{
  const doc = new Document();
  const root = new Node_(doc, 1, 'div');
  let got = null;
  globalThis.webcDispatch = (id, ev) => { got = { id, ev }; };
  WEBC.hydrate(root, ['mount 0 replace #root', 'create 1 0 form', 'listen 1 submit',
    'create 2 1 input'].join('\n') + '\n');
  const e = root.children[0].children[0].dispatch('submit');
  ok(!!e.defaultPrevented, 'the native submit was prevented');
  ok(got && got.id === 1 && got.ev === 'submit', 'the form handler ran once');
  delete globalThis.webcDispatch;
}

const pending = [];
pending.push((async () => {
  console.log('bridge: start({data}) hands the module bytes, not UTF-16 units');
  // `{"city":"Sao Paulo"}` with one accented letter is the whole bug: the length the module
  // received was `text.length`, which counts UTF-16 units, so every non-ASCII byte after the
  // first accent was cut off. A truncated JSON document is not a crash — the parse simply
  // failed, the app rendered its empty state, and `data:` looked like it did nothing.
  const json = '{"city":"S\u00e3o Paulo"}';
  const bytes = Buffer.byteLength(json, 'utf8');
  let got = null;
  const fakeExports = {
    memory: { buffer: new ArrayBuffer(65536) },
    webc_alloc: (n) => { fakeExports._alloc = n; return 1024; },
    webc_data: (ptr, len) => { got = { ptr, len, alloc: fakeExports._alloc }; },
    webc_start: () => {},
    webc_take_cmds: () => 0,
    webc_cmds_len: () => 0,
  };
  const realFetch = globalThis.fetch;
  const realWA = globalThis.WebAssembly;
  globalThis.fetch = async () => ({
    ok: true,
    text: async () => json,
    arrayBuffer: async () => new ArrayBuffer(8),
  });
  globalThis.WebAssembly = {
    instantiateStreaming: async () => ({ instance: { exports: fakeExports } }),
    instantiate: async () => ({ instance: { exports: fakeExports } }),
  };
  let err = null;
  try {
    await WEBC.start({ el: null, wasm: '/app.wasm', data: '/api/board', hydrate: false });
  } catch (e) { err = e; }
  globalThis.fetch = realFetch;
  globalThis.WebAssembly = realWA;
  ok(!err, 'start() ran with a stubbed fetch and module' + (err ? ': ' + err.message : ''));
  ok(!!got, 'the module received the data');
  eq(got && got.len, bytes, 'the length is the UTF-8 byte count');
  ok(got && got.len !== json.length, 'and not the UTF-16 length (' + json.length + ')');
  eq(got && got.alloc, bytes + 1, 'room for the bytes and the NUL');
})());

console.log('bridge: a hook that was never in the first render still fires');
{
  // The ordering rule this kills: `bind()` resolving the name when the *node* is created
  // leaves a control rendered later — a palette, a panel behind a fetch — wired and never
  // firing, and every app shipping its own capture-phase dispatcher to work around it.
  const doc = new Document();
  const root = new Node_(doc, 1, 'div');
  const seen = [];
  globalThis.webcAction = (name, ev) => { seen.push(name + ':' + ev); return 1; };
  WEBC.hydrate(root, ['mount 0 replace #root', 'create 1 0 button', 'attr 1 data-hook close_panel',
                      'create 2 1 span'].join('\n') + '\n');
  // no `listen` op anywhere: the only way this fires is the name lookup at dispatch time
  const span = root.children[0].children[0];
  span.dispatch('click');
  eq(seen.length, 1, 'the name was resolved when the click happened');
  eq(seen[0], 'close_panel:click', 'and it is the hook the element carries');
  delete globalThis.webcAction;
}

let hostCalls = null;
console.log('bridge: the host verbs reach the module, and the module reaches the page');
{
  // No wasm here: every verb's answer is a call back into the module, and the native hooks
  // stand in for it — the same seam `webcDispatch` uses, which is also what the dev server
  // drives. What is under test is the protocol and the page: the ops a real app.wasm emits,
  // and what the bridge does with them.
  const doc = new Document();
  doc.documentElement = new Node_(doc, 1, 'html');
  doc.title = '';
  const root = new Node_(doc, 1, 'div');
  doc.byId.set('root', root);
  const calls = [];
  hostCalls = calls;
  globalThis.webcNetRecv = (id, text, err) => calls.push({ fn: 'recv', id, body: text, err });
  globalThis.webcFire = (id) => calls.push({ fn: 'fire', id });
  globalThis.webcField = (node, name, value) => calls.push({ fn: 'field', node, name, value });
  const realFetch = globalThis.fetch;
  const realTimer = globalThis.setTimeout;
  let fetched = null;
  globalThis.fetch = async (url, init) => {
    fetched = { url, init };
    return { ok: true, status: 200, text: async () => '{"ok":1}' };
  };
  globalThis.setTimeout = (fn) => { fn(); return 1; };

  WEBC.hydrate(root, ['mount 0 replace #root', 'create 1 0 input', 'attr 1 data-field q',
                      'listen 1 click'].join('\n') + '\n');

  // the verbs the app declares in C, arriving as ops
  WEBC.apply(['net-get 3 /api/briefing/7', 'net-post 4 /api/flights/1/reroute {"dest":"LHR"}',
              'net-later 5 5200', 'key 6 meta+k', 'doc-title Contrail',
              'doc-url push /f/VA101', 'doc-attr data-theme light', 'ready 7',
              'key 8 shift+d'].join('\n') + '\n');

  eq(fetched && fetched.url, '/api/flights/1/reroute', 'a POST fetched the url the module asked for');
  ok(fetched && fetched.init && fetched.init.method === 'POST', 'a POST is a POST');
  eq(fetched && fetched.init && fetched.init.body, '{"dest":"LHR"}', 'with the body it was given'); 
  eq(doc.title, 'Contrail', 'doc-title set the document title');
  eq(doc.documentElement.getAttribute('data-theme'), 'light', 'doc-attr set it on <html>');
  ok(calls.some((c) => c.fn === 'fire' && c.id === 5), 'net-later fired its slot');
  ok(calls.some((c) => c.fn === 'fire' && c.id === 7), 'ready fired once the batch was in');

  // one keydown listener, matched against the spec, and never while typing in a field
  const sendKey = (key, meta, target, shift) => {
    const e = { type: 'keydown', key, metaKey: !!meta, ctrlKey: false, shiftKey: !!shift, altKey: false,
                target: target || doc.documentElement, preventDefault() { this.prevented = true; } };
    (doc._capEvents.keydown || doc._events.keydown || []).forEach((fn) => fn(e));
    return e;
  };
  const keyCalls = () => calls.filter((c) => c.fn === 'fire' && c.id === 6).length;
  const before = keyCalls();
  const hit = sendKey('k', true);
  eq(keyCalls(), before + 1, 'meta+k fired its action');
  ok(hit.prevented === true, 'and only a matching key swallows the default');
  sendKey('k', false, new Node_(doc, 1, 'input'));
  sendKey('j', false, new Node_(doc, 1, 'input'));
  eq(keyCalls(), before + 1, 'a bare key, or any key while typing, does not fire it');

  // exact modifiers: shift+d must not swallow a plain d (the stdlib dialog's
  // missing letter was this), and the shifted press is the one that fires
  const shiftCalls = () => calls.filter((c) => c.fn === 'fire' && c.id === 8).length;
  const plainD = sendKey('d', false);
  eq(shiftCalls(), 0, 'a plain d does not fire shift+d');
  ok(plainD.prevented !== true, 'and the plain key is not swallowed');
  sendKey('d', false, new Node_(doc, 1, 'input'));
  eq(shiftCalls(), 0, 'nor does d typed into an input');
  sendKey('d', false, null, true);
  eq(shiftCalls(), 1, 'shift+d fires the command');

  // one input listener, routed by the name the view put on the element
  const input = root.children[0];
  input.value = 'LH';
  (doc._capEvents.input || doc._events.input || []).forEach((fn) => fn({ type: 'input', target: input }));
  const f = calls.filter((c) => c.fn === 'field').pop();
  eq(f && f.name, 'q', 'webc_field carried the field name');
  eq(f && f.value, 'LH', 'and its text');

  // and the one write a render cannot make: the field's live value, by the name the view
  // gave it. A value attribute is only the default, so clearing a draft after the action
  // that consumed it is a doc verb or nothing.
  doc.byId.set('q', input);
  page.find = (sel) => (sel === '#q' ? input : null);
  input.value = 'left over';
  WEBC.apply(['doc-value q cleared', 'doc-value q '].join('\n') + '\n');
  eq(input.value, '', 'doc-value wrote the field the reader had typed into');
  input.value = 'left over';
  WEBC.apply('doc-value q two words\n');
  eq(input.value, 'two words', 'the rest of the line is the value');
  page.find = null;
  doc.byId.delete('q');

  globalThis.fetch = realFetch;
  globalThis.setTimeout = realTimer;
  // the network hooks stay until the trailing block has read the answers: a fetch resolves in
  // a microtask, so deleting them here would drop exactly the call under test
}

console.log('bridge: why() says what is under a point and whether it is wired');console.log('bridge: why() says what is under a point and whether it is wired');
{
  const doc = new Document();
  const root = new Node_(doc, 1, 'div');
  WEBC.hydrate(root, ['mount 0 replace #root', 'create 1 0 button', 'listen 1 click', 'create 2 1 span']
    .join('\n') + '\n');
  const span = root.children[0].children[0];
  doc.elementFromPoint = () => span;
  const why = WEBC.why(10, 10);
  ok(why && why.target === 'span', 'names the element under the point');
  ok(why && why.chain.length === 3, 'and the chain above it, up to the container');
  ok(why && why.chain[0].id === 2, 'by node id');
  ok(why && why.chain[1].listens.join() === 'click', 'the button above it is the one that listens');
  ok(why && why.chain[0].listens.length === 0, 'the span itself listens to nothing');
  doc.elementFromPoint = () => null;
  eq(WEBC.why(10, 10), null, 'an empty point is null, not a throw');
}

console.log('bridge: portals move their subtree');
{
  const doc = new Document();
  const root = new Node_(doc, 1, 'div');
  const modal = new Node_(doc, 1, 'div');
  doc.byId.set('modal-root', modal);
  WEBC.hydrate(root, streams.portal);
  eq(root.children.length, 1, 'only the app div stays in the root');
  eq(modal.children.length, 1, 'the portal moved into #modal-root');
  eq(modal.children[0].textContent, 'dialog', 'its text came along');
  const app = root.children[0];
  eq(app.children[app.children.length - 1].textContent, 'page', 'the rest of the page is intact');
}

pending.push((async () => {
  // The answers travel back through a promise even when fetch is a stub, which is what makes
  // the module's side of a request a callback and not a return value.
  await new Promise((r) => setTimeout(r, 0));
  const get = hostCalls && hostCalls.filter((c) => c.fn === 'recv' && c.id === 3).pop();
  ok(!!get, 'a GET came back through webc_net_recv');
  eq(get && get.body, '{"ok":1}', 'with the body as bytes, not a parsed object');
  const post = hostCalls && hostCalls.filter((c) => c.fn === 'recv' && c.id === 4).pop();
  ok(!!post, 'and so did the POST');
  delete globalThis.webcNetRecv;
  delete globalThis.webcFire;
  delete globalThis.webcField;
})());

Promise.all(pending).then(() => {
  console.log(failures ? `\n${failures} failing` : '\nall bridge tests passed');
  process.exit(failures ? 1 : 0);
});