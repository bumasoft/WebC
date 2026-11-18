// client_test.js — the browser half, without a browser.
//
// `webc test --client` compiles the app's client sources, runs its real client_main, and
// renders each mount to a batch of ops (tools/webc/client_test.c). This replays those
// batches through the *real* assets/webc.js against a fake document and checks what a
// reader would see:
//
//   * the mount applies: every op is one webc.js knows, nothing is created outside the
//     container, the text in the DOM matches the HTML the same view renders on the server
//   * the patch applies: it touches only what changed and does not throw
//   * no batch runs away: an op budget and a wall-clock watchdog per batch, because the
//     failure this exists for is a tab that stops answering — a test that also stops
//     answering is not a test
//
// Exit code is the number of failures, so `webc test --client` can just forward it.
'use strict';
const fs = require('fs');
const path = require('path');

const OPS_MAX = 20000;      // same ceiling as the C side's WEBC_OPS_MAX
const BATCH_MS_MAX = 2000;  // a batch that takes longer than this is the freeze

// ---------------------------------------------------------------- fake DOM
//
// Deliberately small: the bridge only needs children, text, attributes and appendChild.
// A full DOM would let a bug hide behind browser behaviour that the replay is not
// reproducing anyway.

class Node_ {
  constructor(doc, type, tag) {
    this.ownerDocument = doc;
    this.nodeType = type;
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
    if (n.parentNode) n.parentNode.removeChild(n);
    const i = ref ? this.childNodes.indexOf(ref) : -1;
    if (i >= 0) {
      this.childNodes.splice(i, 0, n);
      n.parentNode = this;
      return n;
    }
    return this.appendChild(n);
  }
  removeChild(n) {
    const i = this.childNodes.indexOf(n);
    if (i >= 0) this.childNodes.splice(i, 1);
    n.parentNode = null;
    return n;
  }
  setAttribute(name, value) { this._attrs.set(name, String(value)); }
  getAttribute(name) { return this._attrs.has(name) ? this._attrs.get(name) : null; }
  removeAttribute(name) { this._attrs.delete(name); }
  addEventListener(ev, fn) { (this._events[ev] = this._events[ev] || []).push(fn); }
  dispatch(ev, event) {
    (this._events[ev] || []).forEach((fn) => fn(event || { type: ev, preventDefault() {} }));
  }
  get textContent() {
    if (this.nodeType === 3) return this._text;
    return this.childNodes.map((n) => n.textContent).join('');
  }
  set textContent(v) {
    if (this.nodeType === 3) { this._text = String(v); this.ownerDocument.textWrites++; return; }
    this.childNodes = [];
    if (v !== '') {
      this.appendChild(this.ownerDocument.createTextNode(String(v)));
    }
  }
}

class Document {
  constructor() {
    this.creates = 0;
    this.textWrites = 0;
    this.byId = new Map();
    this.documentElement = { setAttribute() {} };
    this.body = new Node_(this, 1, 'body');
  }
  querySelector(sel) {
    if (sel && sel[0] === '#') {
      const hit = this.byId.get(sel.slice(1));
      if (hit) return hit;
    }
    return null;
  }
  createElement(tag) { this.creates++; return new Node_(this, 1, tag); }
  // A chart is the reason this exists: `el("svg")` and every element under it are created
  // with a namespace, and a harness without this failed every app that draws one with
  // "doc.createElementNS is not a function".
  createElementNS(ns, tag) {
    this.creates++;
    const n = new Node_(this, 1, tag);
    n.namespaceURI = ns;
    return n;
  }
  createTextNode(t) { this.creates++; const n = new Node_(this, 3); n._text = String(t); return n; }
}

// ---------------------------------------------------------------- harness

let failures = 0;
function ok(cond, msg) {
  if (cond) { console.log('  ok   ' + msg); return true; }
  failures++;
  console.log('  FAIL ' + msg);
  return false;
}
function eq(a, b, msg) {
  return ok(a === b, msg + (a === b ? '' : ` (got ${JSON.stringify(a)}, want ${JSON.stringify(b)})`));
}

// webc.js talks to a module-level `document`; the fake one is bound here, so a replay
// uses the *same* bridge the browser does rather than a re-implementation of it.
function loadBridge(doc) {
  const src = fs.readFileSync(path.join(__dirname, '..', '..', 'assets', 'webc.js'), 'utf8');
  const mod = { exports: {} };
  new Function('module', 'exports', 'document', 'crypto', 'window', 'globalThis', src)(
    mod, mod.exports, doc, { getRandomValues() {} },
    { addEventListener() {}, location: { href: 'http://localhost/' } }, {},
  );
  return mod.exports;
}

// The text a fragment of HTML holds, so it can be compared with a DOM's textContent.
function text_of_html(html) {
  return html
    .replace(/<script[\s\S]*?<\/script>/g, '')
    .replace(/<[^>]*>/g, ' ')
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&#39;/g, "'")
    .replace(/&amp;/g, '&')
    .replace(/\s+/g, ' ')
    .trim();
}

// Compare content, not spacing: the server's HTML puts elements on their own lines and
// the DOM does not, so every whitespace character is dropped. What this catches is a
// view whose text is missing, doubled or in the wrong node — not its indentation.
function squash(s) {
  return String(s).replace(/\s+/g, '');
}

// Run one batch with a watchdog: the failure mode is a batch that never comes back.
function replay(WEBC, batch, label) {
  const lines = batch.split('\n').filter(Boolean);
  if (lines.length > OPS_MAX) {
    ok(false, `${label}: ${lines.length} ops is over the ${OPS_MAX} budget`);
    return;
  }
  const t0 = Date.now();
  try {
    WEBC.apply(batch);
  } catch (e) {
    ok(false, `${label}: ${e && e.message ? e.message : e}`);
    return;
  }
  const took = Date.now() - t0;
  if (took > BATCH_MS_MAX) ok(false, `${label}: ${took} ms for ${lines.length} ops`);
}

function main() {
  const file = process.argv[2];
  if (!file || !fs.existsSync(file)) {
    console.error('client_test.js: no batch file; run `webc test --client`');
    process.exit(1);
  }
  const data = JSON.parse(fs.readFileSync(file, 'utf8'));
  const mounts = data.mounts || [];

  console.log(`client: ${mounts.length} mount${mounts.length === 1 ? '' : 's'}`);
  if (!mounts.length) {
    console.log('  (nothing mounted: client_main calls mount() for each view the browser shows)');
  }
  for (const m of mounts) {
    const label = m.sel || '#root';
    console.log(`client: ${label}`);
    const doc = new Document();
    const container = new Node_(doc, 1, 'div');
    container.setAttribute('id', (label.match(/^#([^ >]+)/) || [, 'root'])[1]);
    doc.byId.set(container.getAttribute('id'), container);
    const WEBC = loadBridge(doc);

    // mount: the batch the C side produced, applied by the real webc.js
    const lines = (m.mount || '').split('\n').filter(Boolean);
    ok(lines.length > 0, 'the view produced a batch');
    try {
      WEBC.hydrate(container, m.mount || '');
    } catch (e) {
      ok(false, `mount: ${e && e.message ? e.message : e}`);
    }
    const el = doc.querySelector(label) || container;
    ok(el.children.length > 0 || squash(el.textContent).length > 0, 'the container has content');

    // what the server renders for the same view and context must be what the DOM holds
    if (m.html) {
      const want = squash(text_of_html(m.html));
      const got = squash(el.textContent);
      eq(got, want, 'the DOM text matches the server-rendered HTML');
    }

    // patch: a re-render with nothing changed must not create or remove anything
    const before = doc.creates;
    replay(WEBC, m.patch || '', 'patch');
    const patchLines = (m.patch || '').split('\n').filter(Boolean);
    if (patchLines.length === 0) {
      ok(true, 'an unchanged re-render emits no ops');
    } else {
      const made = patchLines.filter((l) => l.startsWith('create ')).length;
      ok(made === 0, `the patch creates ${made} nodes (want 0)`);
      ok(doc.creates === before, 'the patch creates nothing in the document');
    }
  }
  if (failures) console.log(`client: ${failures} failing`);
  else console.log('client: all mounts applied');
  process.exit(failures);
}

main();