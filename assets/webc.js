// webc.js — the WebC browser bridge.
//
// The C side never touches the DOM: it emits a line-based command stream
//
//   create <id> <parent> <tag>      #tag is "#text" for text nodes
//   attr <id> <name> <value...>     # value is the rest of the line, escaped
//   listen <id> <event>
//   text <id> <value...>
//   raw <id> <parent> <markup...>   #parsed as HTML: what raw() renders on the server
//   attrs-clear <id>
//   remove <id>
//
// and this file replays it. `hydrate` first adopts the server-rendered markup by
// its data-wc anchors, so the first paint costs no DOM creation at all.
'use strict';

const WEBC = (() => {
  const nodes = new Map();      // id -> Node
  const rawSlots = new Map();   // id -> where a raw() node's markup lives
  let lastTouched = null;       // the last node adopted or created: where raw() starts
  let container = null;
  // The container the operations being applied belong to. A module may mount several
  // views, each naming its own element; the `mount` op at the head of a batch says
  // which one this batch is for. `container` is the default (WebC.start({el})).
  let cur = null;
  const views = [];             // per view: { container, mounted }
  let instance = null;          // wasm instance, when running compiled code
  let dispatching = false;
  let hydrating = false;        // there is server markup to adopt, not to duplicate
  let startOpts = {};           // what start() was called with: a swap reuses its imports

  // ---------------------------------------------------------------- listeners
  //
  // One real DOM listener per event type, on the document, and a map from node id to the
  // events that node asked for. The alternative — a listener on the element the render
  // happened to attach it to — dies on the next patch: an element that is re-created (a
  // branch that toggled, a list that reordered) leaves a button that looks live and has no
  // listener, so a click "works, then doesn't, then works" with no error anywhere. Node ids
  // are re-pointed by every `create` op, and an id never changes meaning, so keying on the
  // id is what makes delegation correct here.
  //
  // Attached to the document, not the container: `portal()` moves a subtree *outside* the
  // container, and its buttons still have to work.
  const listened = new Map();   // id -> { click: true, … }
  const delegating = new Set(); // event types that already have their one listener
  // Events that do not bubble. A listener on the document never sees them in the bubble
  // phase, so `bind("row", "pointerleave", …)` — the obvious way to say "the pointer left
  // the row" — silently never ran: the tooltip stayed up after the pointer was gone. They
  // still travel the *capture* phase, which is how the bridge hears them.
  const NON_BUBBLING = new Set(['pointerleave', 'pointerenter', 'mouseleave', 'mouseenter',
                                'focus', 'blur']);
  // Escape hatch: one listener per element, instead of the delegated set below.
  const directListeners = !!globalThis.WEBC_DIRECT_LISTENERS;

  function fire(id, ev, e, node) {
    // The module owns the default of an event it answers: submit/click so a form does
    // not post over the action, and the drag family because a dragover nobody cancels
    // makes the browser refuse the drop entirely — the card snaps home on release,
    // the action never runs, and nothing looks wrong anywhere. This is the path
    // listen-op events take (what bind() wires); the name-walk branch below cancels the
    // same set of events.
    if (ev === 'submit' || ev === 'click' ||
        ev === 'dragover' || ev === 'drop' || ev === 'dragenter')
      e.preventDefault();
    dispatch(id, ev, e, node);
  }

  function delegationRoot() {
    const doc = container && container.ownerDocument;
    if (doc && doc.addEventListener) return doc;
    return container && container.addEventListener ? container : null;
  }

  function delegatingListener() {
    // Two ways to answer an event, in this order:
    //
    //   1. a listener this render attached to the node (the `listen` op) — `fire()` runs it;
    //   2. a *named* control (`named("theme")`) that an element carries as data-hook — the
    //      module resolves the name when the event happens, not when the node was created,
    //      which is what makes a control rendered later (a palette, a panel behind a fetch)
    //      work at all. Without that ordering an app ships its own capture-phase dispatcher
    //      to get it, and the framework's own binding never fires.
    //
    // Innermost wins in both cases and the walk stops there: a click on a button inside a
    // clickable row is the button's, not both.
    return (e) => {
      // A leave that only crossed into the same named unit is not a leave: pointer over a
      // curve group means over several child nodes, and each hop between them fires
      // pointerleave on the one being left. Without this the tooltip blinked off and back
      // on at every boundary inside the group.
      const leaving = e.type === 'pointerleave' || e.type === 'mouseleave';
      let hook = null, hookNode = null;
      for (let n = e.target; n; n = n.parentNode) {
        const id = n.__webc;
        if (id !== undefined) {
          const want = listened.get(id);
          if (want && want[e.type]) {
            if (leaving && e.relatedTarget && n.contains && n.contains(e.relatedTarget)) return;
            fire(id, e.type, e, n);
            return;
          }
        }
        if (!hook && n.getAttribute) {
          const name = n.getAttribute('data-hook');
          if (name) { hook = name; hookNode = n; }
        }
      }
      if (leaving && hookNode && e.relatedTarget && hookNode.contains(e.relatedTarget)) return;
      // the container itself can be a node (a view whose root asked for the event)
      const cid = container && container.__webc;
      const want = cid !== undefined && listened.get(cid);
      if (want && want[e.type]) { fire(cid, e.type, e, container); return; }
      if (hook) {
        // the module owns this event's default once it answers it: click/submit so a
        // form does not post over the action, and the drag family because a dragover
        // without preventDefault makes the browser refuse the drop entirely
        if (e.type === 'click' || e.type === 'submit' ||
            e.type === 'dragover' || e.type === 'drop' || e.type === 'dragenter')
          e.preventDefault();
        action(hook, e.type);
      }
    };
  }

  // ---------------------------------------------------------------- the host verbs
  //
  // The C side declares what the host should do (net-get, key, doc-title, …) and this file
  // is the only place that touches fetch, EventSource, history or document. A verb is a
  // slot: the module keeps the id, and an answer comes back as a call into it. Nothing in
  // an application writes JavaScript for any of this any more.
  const netSlots = new Map();   // id -> { kind, ctrl } — fetch aborters and sources
  const sources = new Map();    // url -> { es, listeners: Set<id> }
  const linkSlots = new Map();  // url -> Set<id> — who hears about open/lost
  const keyMap = new Map();     // "meta+k" -> id
  const timers = new Map();     // id -> timeout handle
  const readyIds = [];          // fired once the first batch is in the document
  const navIds = new Set();     // popstate listeners: back and forward
  let navInstalled = false;

  // A name to focus or reveal: the hook the view declared, the element's id, or the field
  // it carries — whichever finds it first. `focus_hook("q")` and `focus_hook("pal_q")`
  // both work because a view names an input for a person, not for this call.
  function hookNode(name) {
    // The document, not the mount container: a portal subtree (#portal, per the boot
    // block) lives outside the container, and focus is a document-level concern anyway.
    // A name that finds nothing is worth one console line — a focus_hook() that cannot
    // reach its input fails as a page that half-works, silently.
    const root = (typeof document !== 'undefined' && document.querySelector) ? document
                                                                                                                             : (container || null);
    if (!root || !root.querySelector) return null;
    const el = root.querySelector('[data-hook="' + name + '"]') ||
               root.querySelector('#' + name) ||
               root.querySelector('[data-field="' + name + '"]');
    if (!el && typeof console !== 'undefined') {
      if (!hookNode.missed) hookNode.missed = new Set();
      if (!hookNode.missed.has(name)) {
        hookNode.missed.add(name);
        console.warn('[webc] doc verb: no element carries the hook "' + name +
                     '" — a typo, or the branch that renders it has not run yet');
      }
    }
    return el;
  }
  let docAfter = null;          // doc-* ops that need the batch's nodes to exist first

  function settle(id, text, err) {
    if (!instance) {
      if (globalThis.webcNetRecv) globalThis.webcNetRecv(id, text, err);
      return;
    }
    const g = instance.exports;
    if (typeof g.webc_net_recv !== 'function') return;
    const ok = err === undefined || err === null;
    const body = ok ? (typeof text === 'string' ? text : JSON.stringify(text)) : '';
    const why = ok ? '' : String(err);
    const bp = pushSlice(body);
    const ep = pushSlice(why);
    try {
      g.webc_net_recv(id, bp.ptr, bp.len, ep.ptr, ep.len);
    } finally {
      flushCmds();
      g.webc_free(bp.ptr);
      g.webc_free(ep.ptr);
    }
  }

  // Not `fire`: that name is the event path's (node id → dispatch). Two functions with one
  // name is a silent override in JavaScript, and the one that wins is the later one.
  function fireSlot(id) {
    if (!instance) {
      if (globalThis.webcFire) globalThis.webcFire(id);
      return;
    }
    const g = instance.exports;
    if (typeof g.webc_fire !== 'function') return;
    // A shortcut is a command about the element the reader is standing on: `alt+arrowdown`
    // moves *this* card, so the focused element goes with the slot and ev_attr() reads it,
    // exactly as it would for a click on the same node.
    const at = (typeof document !== 'undefined' && document.activeElement) || null;
    const node = at && at.__webc !== undefined ? at.__webc : -1;
    g.webc_fire(id, node);
    flushCmds();
  }

  // ---- fetch: one shape for GET and POST, and the server's own words on failure
  async function fetchSlot(id, url, init) {
    const slot = netSlots.get(id) || { kind: 'get' };
    const ctrl = typeof AbortController === 'function' ? new AbortController() : null;
    slot.ctrl = ctrl;
    netSlots.set(id, slot);
    try {
      const r = await fetch(url, Object.assign({ headers: { accept: 'application/json' } },
                                               init || {}, ctrl ? { signal: ctrl.signal } : {}));
      const text = await r.text();
      if (!r.ok) {
        let msg = 'HTTP ' + r.status;
        try { const j = JSON.parse(text); if (j && j.message) msg = j.message; } catch (e) {}
        settle(id, '', msg);
        return;
      }
      netSlots.delete(id);
      settle(id, text === '' ? 'null' : text);
    } catch (e) {
      if (e && e.name === 'AbortError') return;   // the module stopped caring
      netSlots.delete(id);
      settle(id, '', (e && e.message) || 'network error');
    }
  }

  // ---- Server-sent events. One source per url, shared by every slot that asked for it, so
  // two views on one stream cost one connection. EventSource reconnects on its own; the
  // link's state (open / lost) is reported to whoever registered for it, which is what a
  // "streaming" pill needs.
  function subscribe(id, url, eventName) {
    netSlots.set(id, { kind: 'sub', url });
    if (eventName === 'state') {
      // open/lost frames: the app asked for the link's health, which is this slot's job
      let ls = linkSlots.get(url);
      if (!ls) { ls = new Set(); linkSlots.set(url, ls); }
      ls.add(id);
    }
    const name = eventName || 'message';
    let src = sources.get(url);
    if (!src) {
      src = { frames: new Map(), names: new Set(), es: null };
      sources.set(url, src);
      if (typeof EventSource === 'function') {
        const es = new EventSource(url);
        src.es = es;
        es.addEventListener('open', () => {
          for (const lid of (linkSlots.get(url) || [])) settle(lid, '{"state":"open"}');
        });
        es.addEventListener('error', () => {
          for (const lid of (linkSlots.get(url) || [])) settle(lid, '{"state":"lost"}');
        });
      }
    }
    const deliver = (e) => {
      for (const frame of src.frames.values())
        if (frame.name === e.type) frame.fn(e.data);
    };
    src.frames.set(id, { name, fn: (text) => settle(id, text) });
    if (src.es && !src.names.has(name)) {
      src.names.add(name);
      src.es.addEventListener(name, deliver);
    }
  }

  function unsubscribe(id) {
    const slot = netSlots.get(id);
    netSlots.delete(id);
    if (!slot) return;
    if (slot.ctrl) { try { slot.ctrl.abort(); } catch (e) {} }
    if (slot.kind === 'sub') {
      const src = sources.get(slot.url);
      if (!src) return;
      src.frames.delete(id);
      if (src.frames.size === 0) {
        try { if (src.es) src.es.close(); } catch (e) {}
        sources.delete(slot.url);
      }
    }
  }

  function later(id, ms) {
    const old = timers.get(id);
    if (old) clearTimeout(old);
    const t = timers.get(id);
    if (t) clearTimeout(t);
    timers.set(id, setTimeout(() => { timers.delete(id); fireSlot(id); }, ms));
  }

  function installHostListeners() {
    const root = delegationRoot() || (typeof document !== 'undefined' ? document : null);
    if (!root || typeof root.addEventListener !== 'function') return;
    // click/submit/change are delegated by name as well as by id, so they are installed
    // whether or not this batch carried a `listen` op.
    delegate('click');
    delegate('submit');
    delegate('change');
    if (root.__webcHost) return;
    root.__webcHost = true;
    root.addEventListener('keydown', onKeyDown);
    root.addEventListener('input', onInput);
    const win = globalThis;
    if (win.addEventListener)
      win.addEventListener('popstate', () => {
        navInstalled = true;
        const g = instance && instance.exports;
        for (const id of navIds) {
          // The arrival carries its address: the module matches it against the same
          // route table the server answered, instead of re-implementing the router.
          if (g && typeof g.webc_fire_nav === 'function') {
            const p = pushSlice(String((win.location && win.location.pathname) || '/'));
            g.webc_fire_nav(id, p.ptr, p.len);
            g.webc_free(p.ptr);
            flushCmds();
          } else {
            fireSlot(id);
          }
        }
      });
  }

  // ---- keys: one listener, one modifier plus one key, no sequences
  function parseKey(spec) {
    const parts = String(spec).toLowerCase().split('+');
    const key = parts.pop();
    return { key, mod: parts.includes('meta') || parts.includes('ctrl'),
             shift: parts.includes('shift'), alt: parts.includes('alt'),
             anywhere: parts.length > 0 };
  }

  function onKeyDown(e) {
    const t = e.target;
    const typing = t && (/^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName) || t.isContentEditable);
    const name = String(e.key || '').toLowerCase();
    for (const [spec, id] of keyMap) {
      const k = parseKey(spec);
      if (k.key !== name) continue;
      const mod = !!(e.metaKey || e.ctrlKey);
      if (mod !== k.mod) continue;
      // Modifier matching is exact: `shift+d` must not swallow a plain d typed
      // into a field (a `key.length > 1` guard lets exactly that through, and
      // the stdlib dialog loses every d). Alt follows the same rule.
      if (k.shift !== !!e.shiftKey) continue;
      if (k.alt !== !!e.altKey) continue;
      // a bare key belongs to whatever the user is typing into; a modified one is a command
      if (typing && !k.anywhere && name !== 'escape') continue;
      e.preventDefault();
      fireSlot(id);
      return;
    }
  }

  // ---- fields: one delegated input listener, routed by the name in data-field
  function onInput(e) {
    const el = e.target;
    if (!el || !el.getAttribute) return;
    const name = el.getAttribute('data-field');
    if (!name) return;
    const g = instance && instance.exports;
    if (!g || typeof g.webc_field !== 'function') {
      if (globalThis.webcField)
        globalThis.webcField(el.__webc === undefined ? -1 : el.__webc, name, el.value);
      return;
    }
    const n = pushSlice(name);
    const v = pushSlice(el.value == null ? '' : String(el.value));
    try {
      g.webc_field(el.__webc === undefined ? -1 : el.__webc, n.ptr, n.len, v.ptr, v.len);
    } finally {
      flushCmds();
      g.webc_free(n.ptr);
      g.webc_free(v.ptr);
    }
  }

  // ---- the document, applied after the batch's own nodes exist
  function runDocOps() {
    const ops = docAfter;
    docAfter = null;
    if (!ops) return;
    for (const fn of ops) { try { fn(); } catch (e) { /* a hook that is not on the page */ } }
  }

  let delegatedRoot = null;

  function delegate(ev) {
    const root = delegationRoot();
    if (!root) return;
    // a different document (a test, an iframe, a fresh module) means fresh listeners; the
    // same one must keep the ones it has, or a hot swap would attach a second copy and every
    // click would run twice
    if (root !== delegatedRoot) {
      delegatedRoot = root;
      delegating.clear();
    }
    if (delegating.has(ev)) return;
    delegating.add(ev);
    // capture for click and submit: the module owns the submit once it is up, and a
    // handler that stops propagation must not be able to hide the click from the bridge.
    // capture for the non-bubbling family too, or the document never hears it at all.
    const capture = ev === 'click' || ev === 'submit' || NON_BUBBLING.has(ev);
    root.addEventListener(ev, delegatingListener(), capture);
  }

  // A new module's tree, a fresh page: node ids are about to be re-assigned.
  function resetDelegation() { listened.clear(); }
  let swapDepth = 0;            // >0 while a staged module is being built

  // A hard reload means "do not carry state into the next page". dev.js sets this flag
  // before reloading, so the swap path knows to start from the declarations instead.
  function hardReloadPending() {
    try {
      if (sessionStorage.getItem('webc.hard-reload')) {
        sessionStorage.removeItem('webc.hard-reload');
        return true;
      }
    } catch (e) {}
    return false;
  }

  // "Throw the page away and come back clean." The next start reads this flag and skips
  // state transfer, so a hard reload really forgets: no transplanted signals, no adopted
  // tree. dev.js binds it to Cmd/Ctrl-Shift-R as well.
  function hardReload(why) {
    try { sessionStorage.setItem('webc.hard-reload', '1'); } catch (e) {}
    if (globalThis.WEBC_DEBUG) console.log('[webc] hard reload: ' + (why || 'asked'));
    location.reload();
  }

  function unescape(s) {
    let out = '';
    for (let i = 0; i < s.length; i++) {
      if (s[i] !== '\\') { out += s[i]; continue; }
      const n = s[++i];
      if (n === 'n') out += '\n';
      else if (n === 'r') out += '\r';
      else if (n === '\\') out += '\\';
      else out += n === undefined ? '' : n;
    }
    return out;
  }

  // The command stream may arrive split across reads; keep a tail.
  let pending = '';
  function feed(chunk) {
    if (globalThis.WEBC_DEBUG) console.log('[webc] cmds len=' + chunk.length + '\n' + chunk.trimEnd());
    pending += chunk;
    const cut = pending.lastIndexOf('\n');
    if (cut < 0) return;
    const ready = pending.slice(0, cut);
    pending = pending.slice(cut + 1);
    apply(ready);
  }

  // SVG is not HTML: the same tag name is a different element, and a node created
  // with createElement() inside an <svg> is invisible. So every element names its
  // namespace, and `el("svg")` on the client half builds a real one.
  const SVG_NS = 'http://www.w3.org/2000/svg';
  const SVG_TAGS = new Set([
    'svg', 'g', 'path', 'circle', 'rect', 'line', 'polyline', 'polygon', 'ellipse',
    'text', 'tspan', 'defs', 'use', 'symbol', 'clipPath', 'mask', 'pattern', 'marker',
    'linearGradient', 'radialGradient', 'stop', 'filter', 'image', 'title', 'desc',
  ]);

  function createNode(doc, tag, parent) {
    if (tag === '#text') return doc.createTextNode('');
    if (tag === '#portal') return doc.createElement('div');
    const inSvg = parent && parent.namespaceURI === SVG_NS;
    if (inSvg || SVG_TAGS.has(tag)) return doc.createElementNS(SVG_NS, tag);
    return doc.createElement(tag);
  }

  function childElements(parent) {
    return Array.from(parent.children || []);
  }

  function adopt(parent, id, tag) {
    // Hydration: the SSR render already put this node in the document.
    if (!hydrating) return null;
    if (tag === '#text') {
      const kids = parent.childNodes || [];
      for (const k of kids) {
        if (k.nodeType === 3 && !k.__webc) { k.__webc = id; return k; }
      }
      return null;
    }
    const kids = childElements(parent);
    for (const el of kids) {
      if (el.getAttribute && el.getAttribute('data-wc') === String(id) && !el.__webc) {
        el.__webc = id;
        return el;
      }
    }
    return null;
  }

  // Op-by-op tracing, for `globalThis.WEBC_DEBUG = true`. When the tab wedges there is
  // no error anywhere: the last line printed here is the only evidence of which op did
  // it, which is why the counter is unbounded and the *tail* is what matters.
  // Each op's parent id 0 means "the container of this view".
  function target() {
    if (!cur) cur = container;
    return cur;
  }

  // A malformed op is a framework bug, but it must not cost the page: the loop used
  // to die on the first throw, and every element after that op was silently missing
  // from a render that reported nothing. One op with an empty attribute name
  // (`attr 247  `) took out two thirds of the tree that way. Skip it, say which op it
  // was, and keep the batch.
  const diag = { ops: 0, badOps: 0, lastBad: null, created: 0, createdEls: 0, anchors: 0, hydrated: null };
  function badOp(line, err) {
    diag.badOps++;
    diag.lastBad = line.slice(0, 200);
    if (diag.badOps <= 3 && typeof console !== 'undefined')
      console.error('[webc] bridge: skipped a malformed op ' + JSON.stringify(diag.lastBad) +
                    ' (' + (err && err.message ? err.message : err) + '). ' +
                    (diag.badOps === 1 ? 'This is a bug in the C side, not in your view: ' +
                      'report the op above with the view that produced it.' : ''));
  }

  function diagnostics() {
    return { ops: diag.ops, badOps: diag.badOps, lastBad: diag.lastBad,
             created: diag.created, createdEls: diag.createdEls,
             anchors: diag.anchors, hydrated: diag.hydrated };
  }

  function apply(text) {
    const doc = ((cur || container) && (cur || container).ownerDocument) || document;
    const dbg = !!globalThis.WEBC_DEBUG;
    // ids removed by this batch. A patch can remove a node and re-create it under the same id
    // (a re-render that replaces an element in place); the bindings belong to the id, so they
    // are dropped only if nothing in the batch claimed the id back. That is the difference
    // between a button that keeps working and a button that "works, then doesn't".
    const removedNow = new Set();
    let n = 0;
    for (const line of text.split('\n')) {
      if (!line) continue;
      const sp1 = line.indexOf(' ');
      if (sp1 < 0) continue;
      const op = line.slice(0, sp1);
      const rest = line.slice(sp1 + 1);
      if (dbg) console.log('[webc] op#' + n + ' ' + line.slice(0, 120));
      n++;
      diag.ops++;
      try {
      switch (op) {
        case 'mount': {
          // mount <view> <replace|keep> <selector> — which element this batch builds
          // into. `replace` retires the server's fallback markup on the first batch;
          // a patch (`keep`) never touches what is already there.
          const a = rest.split(' ');
          const idx = +a[0], mode = a[1];
          const sel = unescape(a.slice(2).join(' '));
          const v = views[idx] || (views[idx] = { container: null, mounted: false });
          const found = sel && doc.querySelector ? doc.querySelector(sel) : null;
          if (!found && sel && sel !== '#root' && !v.warned) {
            v.warned = true;
            console.warn('[webc] mount(…, "' + sel + '"): no such element; falling back to ' +
                         (container ? 'the WebC.start() container' : 'document.body'));
          }
          v.container = found || v.container || container || doc.body;
          if (!v.mounted) {
            v.mounted = true;
            // The fallback is the page until the module is up; once it is, the module
            // is the page (documented behaviour: appending after the fallback leaves a
            // "loading…" line under the real UI forever).
            if (mode !== 'keep' && !hydrating && v.container) {
              // The fallback goes; the id map stays. Ids are unique across the views a
              // module mounts, so clearing it here would blind every other view.
              while (v.container.firstChild) v.container.removeChild(v.container.firstChild);
            }
          }
          cur = v.container;
          break;
        }
        case 'create': {
          const a = rest.split(' ');
          const id = +a[0], parentId = +a[1], tag = a.slice(2).join(' ');
          let parent = parentId === 0 ? target() : nodes.get(parentId);
          if (!parent) parent = target();
          let node = adopt(parent, id, tag);
          if (!node) {
            // a portal is a real element that gets moved once its children exist
            node = createNode(doc, tag, parent);
            node.__webc = id;
            parent.appendChild(node);
            diag.created++;
            // Only elements matter for hydration: text nodes carry no server anchor, so every
            // one of them is created on a perfect adoption. Counting those made every page look
            // like a failed match.
            if (tag !== '#text') diag.createdEls++;
          }
          nodes.set(id, node);
          removedNow.delete(id);
          lastTouched = node;
          break;
        }
        case 'move': {
          // A reused keyed item that changed place in its list: same node, same
          // content, new position. insertBefore with the next known element as the
          // anchor (0 = the end of the parent).
          const a = rest.split(' ');
          const id = +a[0], parentId = +a[1], beforeId = +a[2];
          const el = nodes.get(id);
          let parent = parentId === 0 ? target() : nodes.get(parentId);
          if (!parent) parent = target();
          const before = beforeId ? nodes.get(beforeId) : null;
          if (el && el.parentNode) parent.insertBefore(el, before || null);
          else badOp(line, new Error('move: node or parent missing'));
          break;
        }
        case 'attr': {
          const a = rest.split(' ');
          const id = +a[0], name = a[1];
          const value = unescape(a.slice(2).join(' '));
          // an empty name is never a real attribute: setAttribute('') throws, and the
          // browser's own message names no node, so name the op instead
          if (!name) { badOp(line, new Error('empty attribute name')); break; }
          const el = nodes.get(id);
          if (!el || !el.setAttribute) break;
          el.setAttribute(name, value);
          // a value attribute is only the default: the live property is what the
          // human sees once they have typed, so set both
          if (name === 'value' && 'value' in el && el.value !== value) el.value = value;
          if (name === 'checked' && 'checked' in el) el.checked = value !== '' && value !== 'false';
          break;
        }
        case 'text': {
          const sp = rest.indexOf(' ');
          const id = +rest.slice(0, sp);
          const value = unescape(rest.slice(sp + 1));
          const el = nodes.get(id);
          if (el) el.textContent = value;
          break;
        }
        case 'raw': {
          // Markup from raw(): parsed, not escaped. The server renders it inline,
          // so there is no element to anchor; a comment marks the spot instead —
          // an invisible node keeps the exact position the server rendered.
          const a = rest.split(' ');
          const id = +a[0], parentId = +a[1];
          const markup = unescape(a.slice(2).join(' '));
          let parent = parentId === 0 ? target() : nodes.get(parentId);
          if (!parent) parent = target();
          const slot = rawSlots.get(id);
          if (!slot) {
            if (hydrating) {
              // The server already put this markup in the document. Remember where
              // it starts (right after the node we just adopted) and leave it alone
              // until the view changes it.
              const prev = lastTouched && lastTouched.parentNode === parent ? lastTouched : null;
              rawSlots.set(id, {
                parent,
                from: prev ? Array.prototype.indexOf.call(parent.childNodes, prev) + 1 : 0,
                nodes: [],
                anchor: prev ? prev.nextSibling : parent.firstChild,
                markup,
              });
              break;
            }
            const holder = doc.createComment('webc-raw');
            holder.__webc = id;
            parent.appendChild(holder);
            nodes.set(id, holder);
            const range = doc.createRange();
            range.setStartBefore(holder);
            const frag = range.createContextualFragment(markup);
            rawSlots.set(id, { parent, from: 0, nodes: Array.from(frag.childNodes), anchor: holder });
            parent.insertBefore(frag, holder);
            break;
          }
          // an update: drop what this id rendered last time, put the new markup in
          // its place. A hydrating slot has not claimed its nodes yet — claim them
          // now, they are the siblings before the next anchored node.
          if (slot.nodes.length === 0 && slot.markup !== markup) {
            let n = slot.anchor;
            while (n && !n.__webc) {
              slot.nodes.push(n);
              n = n.nextSibling;
            }
            slot.anchor = n;
          }
          for (const old of slot.nodes) if (old.parentNode) old.parentNode.removeChild(old);
          slot.nodes = [];
          slot.markup = markup;
          const range = doc.createRange();
          if (slot.anchor && slot.anchor.parentNode === slot.parent) range.setStartBefore(slot.anchor);
          else range.selectNodeContents(slot.parent), range.collapse(false);
          const frag = range.createContextualFragment(markup);
          slot.nodes = Array.from(frag.childNodes);
          // `slot.anchor` can be null: the hydrating branch records "whatever came next", and a
          // parent that did not line up has nothing next. Reading `.parentNode` off it threw out
          // of the whole batch — the one failure §1 makes impossible everywhere else, so a raw()
          // slot gets the same floor as the rest of the protocol.
          if (slot.anchor && slot.anchor.parentNode === slot.parent)
            slot.parent.insertBefore(frag, slot.anchor);
          else slot.parent.appendChild(frag);
          break;
        }
        case 'attrs-clear': {
          const el = nodes.get(+rest);
          if (el && el.attributes) {
            for (const at of Array.from(el.attributes)) {
              if (at.name !== 'data-wc') el.removeAttribute(at.name);
            }
          }
          break;
        }
        case 'listen': {
          const a = rest.split(' ');
          const id = +a[0], ev = a[1];
          const el = nodes.get(id);
          if (!el) break;
          // recorded by id, so a patch that re-creates this element keeps the binding
          const want = listened.get(id) || {};
          if (want[ev]) break;
          want[ev] = true;
          listened.set(id, want);
          if (directListeners) {
            el.addEventListener(ev, (e) => fire(id, ev, e));
            break;
          }
          delegate(ev);
          break;
        }
        case 'portal': {
          // <portal>: move this subtree into the element the selector names, so a
          // dialog escapes whatever stacking context it was written in.
          const sp = rest.indexOf(' ');
          const id = +rest.slice(0, sp);
          const sel = unescape(rest.slice(sp + 1));
          const el = nodes.get(id);
          const target = sel ? doc.querySelector(sel) : null;
          if (el && target) target.appendChild(el);
          break;
        }
        case 'remove': {
          const id = +rest;
          const el = nodes.get(id);
          if (el && el.parentNode) el.parentNode.removeChild(el);
          nodes.delete(id);
          removedNow.add(id);
          break;
        }
        // ---- the host verbs (include/webc/host.h). Each one is a declaration from C
        // that the page is the only side able to honour: a request, a stream, a timer, a
        // key, or the document around the view.
        case 'net-get': {
          const id = +rest.slice(0, rest.indexOf(' '));
          const url = unescape(rest.slice(rest.indexOf(' ') + 1));
          fetchSlot(id, url, { method: 'GET' });
          break;
        }
        case 'net-post': {
          const a = rest.split(' ');
          const body = unescape(a.slice(2).join(' '));
          fetchSlot(+a[0], unescape(a[1]), {
            method: 'POST',
            headers: { 'content-type': 'application/json', accept: 'application/json' },
            body: body || '{}',
          });
          break;
        }
        case 'net-sub': {
          const a = rest.split(' ');
          subscribe(+a[0], unescape(a[1]), unescape(a.slice(2).join(' ')));
          break;
        }
        case 'net-unsub': unsubscribe(+rest); break;
        case 'net-later': {
          const a = rest.split(' ');
          later(+a[0], +a[1]);
          break;
        }
        case 'net-link': {
          const a = rest.split(' ');
          const url = unescape(a.slice(1).join(' '));
          const set = linkSlots.get(url) || new Set();
          set.add(+a[0]);
          linkSlots.set(url, set);
          const src = sources.get(url);
          if (src && src.es && src.es.readyState === 1) settle(+a[0], '{"state":"open"}');
          break;
        }
        case 'key': {
          const sp = rest.indexOf(' ');
          keyMap.set(unescape(rest.slice(sp + 1)), +rest.slice(0, sp));
          break;
        }
        case 'nav': {
          navIds.add(+rest);
          break;
        }
        case 'ready': readyIds.push(+rest); break;
        case 'doc-attr': {
          const sp = rest.indexOf(' ');
          const name = rest.slice(0, sp);
          const value = unescape(rest.slice(sp + 1));
          (docAfter = docAfter || []).push(() => doc.documentElement &&
            doc.documentElement.setAttribute(name, value));
          break;
        }
        case 'doc-title': {
          const value = unescape(rest);
          (docAfter = docAfter || []).push(() => { doc.title = value; });
          break;
        }
        case 'doc-url': {
          const sp = rest.indexOf(' ');
          const how = rest.slice(0, sp);
          const href = unescape(rest.slice(sp + 1));
          (docAfter = docAfter || []).push(() => {
            const h = globalThis.history;
            if (!h) return;
            try {
              if (how === 'replace') h.replaceState(null, '', href);
              else h.pushState(null, '', href);
            } catch (e) { /* a sandboxed frame: the address bar is not ours */ }
          });
          break;
        }
        case 'doc-value': {
          // The field's live value, which no attribute op can reach once it has been typed
          // into: an `attr` op writes the attribute the render drew, and setting `value`
          // on an input the reader is typing in only moves the *default*. After the batch,
          // like every doc-* op, because the field may be a node this batch just created.
          const sp = rest.indexOf(' ');
          const name = sp < 0 ? rest : rest.slice(0, sp);
          const value = sp < 0 ? '' : unescape(rest.slice(sp + 1));
          (docAfter = docAfter || []).push(() => {
            const el = hookNode(name);
            if (!el || !('value' in el)) return;
            el.value = value;
          });
          break;
        }
        case 'doc-focus': {
          const name = rest;
          (docAfter = docAfter || []).push(() => {
            const el = hookNode(name);
            if (!el) return;
            // after layout: a focus() on a node the batch has not laid out does nothing
            (globalThis.requestAnimationFrame || ((f) => setTimeout(f, 0)))(() => {
              if (el.focus) el.focus();
              if (el.select) el.select();
            });
          });
          break;
        }
        case 'doc-reveal': {
          const name = rest;
          (docAfter = docAfter || []).push(() => {
            const el = hookNode(name);
            if (el && el.scrollIntoView) el.scrollIntoView({ block: 'nearest' });
          });
          break;
        }
        default:
          // A silent drop here is a page that half-works with no error: the C side
          // emitted an op this build of webc.js does not know, which means a stale
          // bridge in a cache (or a framework that grew an op the bridge did not).
          throw new Error('webc: unknown render op "' + op + '" — webc.js and app.wasm ' +
                          'are from different builds');
      }
      } catch (e) {
        badOp(line, e);
      }
    }
    for (const id of removedNow) listened.delete(id);
    // The document ops last: a title or a focus() describes the tree this batch just built,
    // and a focus() before layout silently does nothing.
    runDocOps();
    // `on_ready()` last: the tree is in the document, so a stream may open and a first fetch
    // may land without racing the render that built it.
    for (const id of readyIds.splice(0)) fireSlot(id);
    if (dbg) console.log('[webc] batch done ops=' + n);
  }

  // What the bridge sends as a payload's text: a form control's value, or a piece of
  // contentEditable. See dispatch() for why the test cannot be `value !== undefined`.
  const FIELD_TAGS = { INPUT: 1, TEXTAREA: 1, SELECT: 1, OPTION: 1, BUTTON: 1 };
  function isFieldEl(el) {
    if (!el) return false;
    if (el.isContentEditable) return true;
    return !!(el.tagName && FIELD_TAGS[el.tagName]);
  }

  // Events go back into C, which updates signals, runs effects and returns a
  // patch batch. That round trip is the whole reactivity story.
  function dispatch(id, event, e, node) {
    if (globalThis.WEBC_DEBUG) console.log('[webc] event id=' + id + ' ev=' + event);
    let payload = '';
    // "has a .value" is not the test for "is a field": <li> has one (its deprecated list
    // number, 0 by default) and <button> has one, so a pointerdown on a card sent "0" —
    // six legal characters that parsed as a pointer at 0,0, and every drag on a <li> row
    // started in the page's top-left corner with `ev_point()` reading zeros. A field is a
    // form control or something editable; anything else that carries coordinates gets them.
    const t0 = e && e.target;
    if (t0 && isFieldEl(t0)) {
      payload = String(t0.value === undefined || t0.value === null ? '' : t0.value);
    } else if (e && typeof e.clientX === 'number' && typeof e.clientY === 'number') {
      // A pointer event carries where it happened: send it, so an action can follow the
      // pointer without a script or a getBoundingClientRect of its own. `\u0001` marks
      // the payload as six numbers — a field's text never starts with it.
      let r;
      try {
        // The *element* the event reached, not e.currentTarget: listeners are delegated
        // to the document, so currentTarget is always the document and its box is the
        // whole page — which would make every element-local coordinate a page coordinate.
        const t = node && node.getBoundingClientRect ? node : null;
        r = t ? t.getBoundingClientRect() : { left: 0, top: 0, width: 0, height: 0 };
      } catch (_) {
        r = { left: 0, top: 0, width: 0, height: 0 };
      }
      payload = '\u0001' + [e.clientX - r.left, e.clientY - r.top, r.width, r.height,
                            e.clientX, e.clientY].join(',');
    } else if (t0 && t0.value !== undefined && t0.value !== null) {
      // not a field and not a pointer: whatever the element chose to publish
      payload = String(t0.value);
    }
    if (instance) {
      const ev = pushSlice(event);
      const pay = pushSlice(payload);
      dispatching = true;
      try {
        // Pointer *and* length for both strings: the C side never has to scan for a
        // NUL, and nothing it reads can run past what the page actually wrote.
        instance.exports.webc_dispatch(id, ev.ptr, ev.len, pay.ptr, pay.len);
      } finally {
        dispatching = false;
        flushCmds();
        instance.exports.webc_free(ev.ptr);
        instance.exports.webc_free(pay.ptr);
      }
    } else if (globalThis.webcDispatch) {
      // Native dev server / test harness hook.
      globalThis.webcDispatch(id, event, payload, (cmds) => feed(cmds));
    }
  }

  // Read back the batch C built. Every entry point into the module ends here, so a
  // promise settling and a click reach the DOM by the same road.
  function flushCmds() {
    if (!instance) return;
    const out = instance.exports.webc_take_cmds();
    if (!out) return;
    const len = instance.exports.webc_cmds_len();
    if (!len) return;
    if (globalThis.WEBC_DEBUG) console.log('[webc] take ptr=' + out + ' len=' + len);
    feed(new TextDecoder().decode(new Uint8Array(instance.exports.memory.buffer, out, len)));
  }

  // Call an action the view named (`named("theme")`), as if its element had been
  // clicked. The C side finds the element and calls the listeners it attached, with
  // the context they were attached with — so this is the same code a real click runs.
  function action(name, event) {
    if (!instance && globalThis.webcAction) return globalThis.webcAction(name, event || 'click');
    if (!instance) throw new Error('WebC.dispatch: the module is not running');
    const fn = instance.exports.webc_action;
    if (typeof fn !== 'function') throw new Error('WebC.dispatch: this build has no actions');
    const n = pushSlice(name);
    const ev = pushSlice(event || 'click');
    let ran;
    try {
      ran = fn(n.ptr, n.len, ev.ptr, ev.len);
    } finally {
      flushCmds();
      instance.exports.webc_free(n.ptr);
      instance.exports.webc_free(ev.ptr);
    }
    return ran;
  }

  // Call a C function the app exported with `webc_export_json(name)`. Strings and
  // objects are marshalled here, so the call site is `WebC.call('ping', {n: 1})`
  // rather than a hand-written buffer in wasm memory.
  function call(name, ...args) {
    if (!instance) throw new Error('WebC.call: the module is not running');
    const fn = instance.exports[name];
    if (typeof fn !== 'function') throw new Error('WebC.call: no export named ' + name);
    let payload = 'null';
    if (args.length === 1) payload = JSON.stringify(args[0]);
    else if (args.length > 1) payload = JSON.stringify(args);
    const p = pushSlice(payload);
    let ret;
    try {
      ret = fn(p.ptr, p.len);
    } finally {
      flushCmds();
      instance.exports.webc_free(p.ptr);
    }
    return ret;
  }

  let encoder = null;
  // A pointer is only meaningful inside the module that produced it. Carrying `jsResult`
  // across a swap made the new module free an address it never allocated, and the first
  // heap allocation afterwards came back zeroed — which is how a `js_then` call ended up
  // parsing its own arguments as NUL bytes.
  let jsResult = 0;
  let interopAtStart = 0; // how many js_call()s the module made while it was starting
  function forgetModuleState() {
    jsResult = 0;
    pending = '';
    encoder = null;
    interopAtStart = 0;
  }

  // Copy a JS string into wasm memory as (pointer, length), NUL-terminated as well so
  // a C function that wants a `const char *` still gets one.
  function pushSlice(s) {
    encoder = encoder || new TextEncoder();
    const bytes = encoder.encode(s);
    const ptr = instance.exports.webc_alloc(bytes.length + 1);
    const mem = new Uint8Array(instance.exports.memory.buffer);
    mem.set(bytes, ptr);
    mem[ptr + bytes.length] = 0;
    return { ptr, len: bytes.length };
  }

  // Copy a JS string into wasm memory, NUL-terminated, and return its pointer.
  function pushStr(s) {
    encoder = encoder || new TextEncoder();
    const bytes = encoder.encode(s);
    const ptr = instance.exports.webc_alloc(bytes.length + 1);
    const mem = new Uint8Array(instance.exports.memory.buffer);
    mem.set(bytes, ptr);
    mem[ptr + bytes.length] = 0;
    return ptr;
  }

  // How many nodes in this subtree carry the server's `data-wc` anchor. The count is what
  // decides whether `hydrate` had anything to adopt at all.
  function countAnchors(n) {
    let k = 0;
    (function walk(x) {
      const kids = (x && x.childNodes) || [];
      for (const c of kids) {
        if (c.nodeType !== 1) continue;
        if (c.getAttribute && c.getAttribute('data-wc') !== null) k++;
        walk(c);
      }
    })(n);
    return k;
  }

  // Anything in the container that the module did not claim is the server's markup: after
  // adoption every element the bridge knows carries `__webc`. Dropping the rest is what keeps
  // one failed `adopt` from leaving *both* trees on the page — 48 rows where the server sent
  // 24, with the old ones still visible and still receiving clicks.
  function dropUnadopted(root) {
    let dropped = 0;
    (function walk(n) {
      for (const c of Array.from(n.childNodes || [])) {
        if (c.nodeType !== 1) continue;
        if (c.__webc === undefined) {
          n.removeChild(c);
          dropped++;
          continue;
        }
        walk(c);
      }
    })(root);
    return dropped;
  }

  // Adoption is verified, not hoped for. Returns true when nothing had to be corrected.
  function settleHydration(root, anchors, createdBefore) {
    // Adoption is over after the first batch, and `hydrating` has to say so. The `raw` op has
    // two behaviours: while adopting, the server already put the markup in the document and the
    // slot only records where it starts; afterwards it has to *insert* it. Leaving the flag set
    // meant every raw() created by a later patch — the palette's search glyph, an aircraft
    // glyph, a toast icon — inserted nothing at all, and the element came up empty with no error
    // anywhere. The first batch is the whole adoption; past it, hydration is finished.
    hydrating = false;
    const created = diag.createdEls - createdBefore;
    if (anchors <= 0) { diag.hydrated = false; return true; }
    if (created === 0) { diag.hydrated = true; return true; }
    const dropped = dropUnadopted(root);
    diag.hydrated = false;
    if (typeof console !== 'undefined')
      console.warn('[webc] hydrate: ' + created + ' node(s) did not match the server markup (' +
                   anchors + ' anchors in it); ' + dropped + ' stale node(s) dropped. ' +
                   'Look for a raw() slot, an svg subtree, or an id that moved between the halves.');
    return false;
  }

  function hydrate(root, commands) {
    resetDelegation();
    container = root;
    views.length = 0;
    views[0] = { container: root, mounted: false };
    cur = root;
    nodes.clear();
    hydrating = true;
    const anchors = countAnchors(root);
    const createdBefore = diag.createdEls;
    apply(commands);
    installHostListeners();
    settleHydration(root, anchors, createdBefore);
  }

  async function start(opts = {}) {
    startOpts = opts;
    resetDelegation();
    container = typeof opts.el === 'string' ? document.querySelector(opts.el)
              : (opts.el || document.querySelector('#root') || document.body);
    cur = container;
    views.length = 0;
    // server markup with our anchors in it means hydrate, not render
    hydrating = !!opts.hydrate && !!(container && container.querySelector &&
               container.querySelector('[data-wc]'));
    const anchors = hydrating ? countAnchors(container) : 0;
    diag.anchors = anchors;
    const createdBefore = diag.createdEls;
    const url = opts.wasm || '/app.wasm';
    const res = await fetch(url);
    const { instance: inst } = await WebAssembly.instantiateStreaming
      ? await WebAssembly.instantiateStreaming(res, imports(opts))
      : await WebAssembly.instantiate(await res.arrayBuffer(), imports(opts));
    instance = inst;
    const exports = inst.exports;
    // a reactor module runs its constructors through _initialize, once
    if (exports._initialize) exports._initialize();
    // The page env, first of all: the edition the document is wearing and the address
    // it was rendered at, so the module's first render agrees with the server's —
    // page_env() is the same fact c->page.env is on the other half.
    if (exports.webc_env && typeof document !== 'undefined' && document.documentElement) {
      const theme = document.documentElement.getAttribute('data-theme');
      const p = pushSlice(String((globalThis.location && globalThis.location.pathname) || '/'));
      exports.webc_env(theme !== 'light', p.ptr, p.len);
      exports.webc_free(p.ptr);
    }
    // seed the module with the server's data before the first render. A string that
    // starts with '/' is a URL to fetch; anything else is the document itself — the
    // inline seed the page carries, fetched straight out of the DOM, so the first
    // render is the same bytes the server rendered.
    if (opts.data) {
      let text = opts.data;
      if (String(text)[0] === '/') {
        const res2 = await fetch(opts.data, { headers: { accept: 'application/json' } });
        if (!res2.ok) text = '';
        else text = await res2.text();
      }
      if (text && exports.webc_data) {
        // The length the module gets must be the length of the *bytes* we just wrote, not
        // `text.length`: that is UTF-16 units, so one "São Paulo" in the seed JSON made the
        // buffer 1 byte short, the parse fail, and `start({data})` do nothing at all — with
        // no error, because a JSON document that is merely truncated is not a crash.
        const slice = pushSlice(text);
        exports.webc_data(slice.ptr, slice.len);
      }
    }
    if (exports.webc_start) {
      exports.webc_start();
      const out = exports.webc_take_cmds();
      const len = exports.webc_cmds_len();
      if (out > 0 && len > 0) {
        const text = new TextDecoder().decode(new Uint8Array(exports.memory.buffer, out, len));
        feed(text);
      }
    }
    // The same verification hydrate() does, for the path that actually ships: `start()` with
    // hydrate:true replays the module's first batch into the server's markup, and a node that
    // could not be adopted leaves both trees on the page unless it is caught here.
    if (hydrating) settleHydration(container, anchors, createdBefore);
    // The host listeners: one keydown, one input, one popstate — installed whether or not
    // the app declared any, because a page may add a control this batch did not mention.
    installHostListeners();
    // The module is up: say so on <html>, and retire the no-JavaScript controls the
    // view marked with scripted_hide(). Until this line the fallback is the page;
    // after it the markup and the module are the same thing, never both.
    if (typeof document !== 'undefined' && document.documentElement) {
      document.documentElement.setAttribute('data-wasm', '1');
      const stale = document.querySelectorAll('[data-scripted-hide]');
      for (const el of stale) el.style.display = 'none';
    }
    return inst;
  }

  // ---------------------------------------------------------------- hot swap (dev)
  //
  // Replace the running module without touching the document. Everything before the
  // commit is reversible: if the new module fails to instantiate, panics in its
  // constructors, or fails to produce a batch, the old one keeps the page and nobody sees
  // anything at all.
  //
  // What survives, in order of certainty:
  //   - the document: nothing is cleared, so scroll, focus and open <details> stay put
  //   - the signals: the old module dumps its registered signals, the new one loads them
  //   - the DOM nodes: the old module dumps its rendered tree, the new one diffed against
  //     it, so a one-word edit is one `text` op instead of a rebuild
  function transfer(prev, next, dumpPtr, dumpLen, load) {
    if (!prev.exports[dumpLen] || !prev.exports[dumpPtr] || !next.exports[load]) return 0;
    const n = prev.exports[dumpLen]();
    if (!n) return 0;
    const ptr = prev.exports[dumpPtr]();
    if (!ptr) return 0;
    const bytes = new Uint8Array(prev.exports.memory.buffer, ptr, n).slice();
    const dst = next.exports.webc_alloc(bytes.length + 1);
    if (!dst) return 0;
    new Uint8Array(next.exports.memory.buffer, dst, bytes.length).set(bytes);
    const applied = next.exports[load](dst, bytes.length);
    next.exports.webc_free(dst);
    return applied;
  }

  async function swap(url, o = {}) {
    if (!instance) return start(Object.assign({}, startOpts, { wasm: url }));
    const prev = instance;
    const opts = startOpts;
    const res = await fetch(url, { cache: 'no-store' });
    if (!res.ok) throw new Error('fetch ' + url + ': ' + res.status);
    const src = await res.arrayBuffer();
    forgetModuleState();
    const { instance: next } = await WebAssembly.instantiate(src, imports(opts));
    if (next.exports._initialize) next.exports._initialize();
    // a hard reload was asked for: install the module, then let the page come again
    if (hardReloadPending()) {
      try { sessionStorage.setItem('webc.hard-reload', '1'); } catch (e) {}
      location.reload();
      return next;
    }
    // what a person would notice being lost: the caret. The container is rebuilt on a
    // `state` swap, so the focused element is a new element afterwards — find its twin by
    // name (or id) and put the caret back where it was.
    const caret = caretSnapshot();
    let signals = 0;
    let tree = 0;
    if (o.keep !== 'none') {
      signals = transfer(prev, next, 'webc_state_dump', 'webc_state_dump_len', 'webc_state_load');
      if (o.keep === 'dom' || o.keep === undefined) {
        tree = transfer(prev, next, 'webc_tree_dump', 'webc_tree_dump_len', 'webc_tree_load');
      }
    }
    // render the new module into its command buffer BEFORE committing: this is the only
    // step that can throw, and it throws where nothing has changed yet
    interopAtStart = 0;
    next.exports.webc_start();
    // A module that talks to JavaScript while it starts cannot be swapped in: the answer
    // belongs to a promise the retired module is no longer waiting for, and the page would
    // sit on its old data (or, worse, on a half-applied batch). A reload is the honest
    // answer, and it costs the developer nothing but a scroll position.
    if (interopAtStart > 0) {
      if (globalThis.WEBC_DEBUG)
        console.log('[webc] this module calls into JavaScript at start — reloading instead of swapping');
      hardReload('interop at start');
      return next;
    }
    let text = '';
    if (next.exports.webc_take_cmds && next.exports.webc_cmds_len) {
      const out = next.exports.webc_take_cmds();
      const len = next.exports.webc_cmds_len();
      if (out > 0 && len > 0)
        text = new TextDecoder().decode(new Uint8Array(next.exports.memory.buffer, out, len));
    }
    // The browser keeps its view bookkeeping, the module does not: a `mount ... replace`
    // in the new module's first batch has to clear the container the old module filled,
    // and that only happens while the view is not yet marked mounted. (With `keep: dom`
    // the batch is a `keep`, which leaves the container alone — the tree came with it.)
    for (let i = 0; i < views.length; i++) if (views[i]) views[i].mounted = false;
    // commit
    instance = next;
    if (globalThis.WEBC_DEBUG) {
      console.log('[webc] swapped (' + signals + ' signals, ' + tree + ' nodes, ' +
                  text.split('\n').length + ' ops)');
      if (text) console.log('[webc] swap ops:\n' + text.trimEnd());
    }
    if (text) feed(text);
    caretRestore(caret);
    return next;
  }

  // A caret is a position in an element, and the element does not survive a re-render:
  // its `name` (or `id`) does. That is what a swap brings back.
  function caretSnapshot() {
    const el = document.activeElement;
    if (!el || el === document.body || !container || !container.contains(el)) return null;
    const key = el.getAttribute('name') ? 'name=' + el.getAttribute('name')
              : (el.id ? 'id=' + el.id : null);
    if (!key) return null;
    let start = null, end = null;
    try { start = el.selectionStart; end = el.selectionEnd; } catch (e) {}
    return { key, start, end };
  }

  function caretRestore(snap) {
    if (!snap || !container) return;
    const found = container.querySelector('[' + snap.key.replace(/=/, '="') + '"]');
    if (!found || typeof found.focus !== 'function') return;
    try {
      found.focus({ preventScroll: true });
      if (snap.start !== null && typeof found.setSelectionRange === 'function')
        found.setSelectionRange(snap.start, snap.end);
    } catch (e) {}
  }

  function imports(opts) {
    const wasi = {
      proc_exit: (code) => { throw new Error('webc aborted: ' + code); },
      fd_close: () => 0,
      fd_seek: () => 0,
      fd_fdstat_get: () => 0,
      clock_time_get: (id, precision, out) => {
        if (instance) {
          // One i64 of nanoseconds since the epoch, little endian — that is what WASI
          // asks for. Writing seconds into the low 32 bits and `(ms%1000)*1e6` into the
          // high half (what this did) is not that number: `clock_gettime` in wasm then
          // returned whatever the two halves happened to spell, so `time()` was 1977 one
          // second and 2050 the next. CLOCK_MONOTONIC (id 1) has to be a clock too.
          const mem = new DataView(instance.exports.memory.buffer);
          const ns = id === 1
            ? BigInt(Math.round((performance.now()) * 1e6))
            : BigInt(Date.now()) * 1000000n;
          mem.setBigUint64(out, ns, true);
        }
        return 0;
      },
      fd_write: (fd, iovs, iovsLen, nwritten) => {
        if (!instance) return 0;
        const mem = new DataView(instance.exports.memory.buffer);
        let written = 0;
        let text = '';
        for (let i = 0; i < iovsLen; i++) {
          const ptr = mem.getUint32(iovs + i * 8, true);
          const len = mem.getUint32(iovs + i * 8 + 4, true);
          if (len) {
            text += new TextDecoder().decode(
              new Uint8Array(instance.exports.memory.buffer, ptr, len));
            written += len;
          }
        }
        if (text && opts.debug !== false) (fd === 2 ? console.error : console.log)('[webc]', text.trimEnd());
        mem.setUint32(nwritten, written, true);
        return 0;
      },
      random_get: (ptr, len) => {
        const b = new Uint8Array(instance ? instance.exports.memory.buffer : new ArrayBuffer(len), ptr, len);
        crypto.getRandomValues(b);
        return 0;
      },
    };
    // Any WASI call we did not name resolves to a stub that reports success:
    // the module uses no descriptors, so the rest are bookkeeping.
    const wasiProxy = new Proxy(wasi, {
      get(target, key) {
        return key in target ? target[key] : () => 0;
      },
      has() { return true; },
    });
    // js_call(path, pathLen, args, argsLen, thenId) — the one door from C into
    // JavaScript (SPEC §8). The path is dotted from globalThis and may name a function
    // (called with the JSON arguments) or a property (read). The result goes back as
    // JSON text that stays valid until the next call.
    //
    // `thenId` is how a promise comes back: non-zero means "this answers later", so the
    // value is settled into that slot on the C side — `webc_js_settle` — instead of
    // being returned. A reply that is thrown away is the failure this prevents: a
    // promise stringifies to `{}`, which looked like an empty object and not like an
    // unread answer.
    // (jsResult lives at module scope: a pointer belongs to one module's heap, and the
    // next module must not free it — see forgetModuleState)
    function resolvePath(path) {
      const parts = path.split('.');
      let base = globalThis;
      let self;
      for (let i = 0; i < parts.length; i++) {
        if (base === undefined || base === null) throw new Error('no such path: ' + path);
        self = base;
        base = base[parts[i]];
      }
      return { target: base, self };
    }
    // A failure is a JSON string whose first character is a raw 0x01 — a value no page
    // can return, so the C side can tell an error from data without an envelope. The
    // marker is appended *raw*: `JSON.stringify('\u0001x')` escapes it to the six
    // characters `\u0001x`, which the C side read as a perfectly good string and
    // reported as success. Every failed interop call was silently ok because of it.
    function jsFailure(msg) {
      return '"\u0001' + JSON.stringify(String(msg)).slice(1, -1) + '"';
    }
    function jsPut(text) {
      const bytes = new TextEncoder().encode(text);
      if (jsResult) instance.exports.webc_free(jsResult);
      jsResult = instance.exports.webc_alloc(bytes.length + 1);
      const dst = new Uint8Array(instance.exports.memory.buffer);
      dst.set(bytes, jsResult);
      dst[jsResult + bytes.length] = 0; // the C side reads it with strlen as well
      return { ptr: jsResult, len: bytes.length };
    }
    // The module that asked is the module that answers. A promise that settles after a
    // swap would otherwise write its answer into the *new* module's memory and settle a
    // thenId that belongs to a table the new module never filled: the new module's own
    // request stayed pending, and the answer landed in a slot nothing was waiting on.
    function jsSettle(thenId, text, owner) {
      if (owner && owner !== instance) {
        if (globalThis.WEBC_DEBUG) console.log('[webc] dropped an answer for a retired module');
        return;
      }
      const p = jsPut(text);
      if (typeof instance.exports.webc_js_settle === 'function')
        instance.exports.webc_js_settle(thenId, p.ptr, p.len);
      flushCmds();
    }
    const jsCall = (pathPtr, pathLen, argsPtr, argsLen, thenId) => {
      const owner = instance; // the module whose thenId this answer belongs to
      interopAtStart++;
      const mem = new Uint8Array(instance.exports.memory.buffer);
      const readStr = (ptr, len) => (len ? new TextDecoder().decode(mem.subarray(ptr, ptr + len)) : '');
      const path = readStr(pathPtr, pathLen);
      const raw = readStr(argsPtr, argsLen);
      let out;
      let value;
      try {
        const { target, self } = resolvePath(path);
        const args = raw ? JSON.parse(raw) : [];
        if (typeof target === 'function') {
          value = target.apply(self, Array.isArray(args) ? args : [args]);
        } else if (Array.isArray(args) && args.length === 0) {
          value = target; // a property read: document.title, localStorage.length
        } else {
          throw new Error(path + ' is not a function');
        }
        out = value === undefined ? '' : JSON.stringify(value);
      } catch (err) {
        // A swallowed exception is a blank screen with no explanation, so the failure
        // travels back as a value the C side can read — and it is logged *once*, here, with
        // the path, never a second time through the C side's fd_write.
        out = String(err && err.message ? err.message : err);
        console.error('js_call(' + path + '): ' + out);
        out = jsFailure(out);
        if (thenId) {
          jsSettle(thenId, out, owner);
          return 0;
        }
        return jsPut(out).ptr;
      }
      if (thenId) {
        // Promise.resolve so a plain value settles too, and always asynchronously: the
        // handler must not run while the C call that asked for it is still on the stack.
        Promise.resolve(value).then(
          (v) => { jsSettle(thenId, v === undefined ? '' : JSON.stringify(v), owner); },
          (err) => {
            const msg = String(err && err.message ? err.message : err);
            // a module that is about to be replaced by a reload: its late failure is noise
            if (!(interopAtStart > 0 && owner === instance))
              console.error('js_call(' + path + '): ' + msg);
            jsSettle(thenId, jsFailure(msg), owner);
          },
        );
        return 0;
      }
      return jsPut(out).ptr;
    };
    return {
      env: {
        webc_dev_log: (ptr, len) => {
          if (opts.debug) console.log('[webc]', ptr, len);
        },
      },
      webc: {
        js_call: jsCall,
      },
      wasi_snapshot_preview1: wasiProxy,
    };
  }

  // What is at (x, y)? "The click does nothing" is the hardest failure in a page built from
  // remote state: the element under the mouse is not the element the developer means, and
  // nothing is wrong enough to throw. This names the element and the three things that decide
  // whether a click can reach it — still hit-tested (`display`, `pointer-events`) and actually
  // wired (the node ids with a listener) — plus the app's own handles while it walks up.
  //
  //   WebC.why(420, 300)   // → { target: 'span', chain: [ … ] }, and the same in the console
  function why(x, y) {
    const doc = (container && container.ownerDocument) || document;
    if (!doc || !doc.elementFromPoint) return null;
    const el = doc.elementFromPoint(x, y);
    if (!el) return null;
    const view = doc.defaultView;
    const chain = [];
    for (let n = el; n && n !== doc; n = n.parentNode) {
      const cs = view && view.getComputedStyle ? view.getComputedStyle(n) : null;
      const want = n.__webc !== undefined ? listened.get(n.__webc) : null;
      chain.push({
        tag: (n.tagName || '#node').toLowerCase(),
        id: n.__webc,
        hook: n.getAttribute ? n.getAttribute('data-hook') : null,
        cls: n.getAttribute ? n.getAttribute('class') : null,
        display: cs ? cs.display : undefined,
        position: cs ? cs.position : undefined,
        pointerEvents: cs ? cs.pointerEvents : undefined,
        listens: want ? Object.keys(want) : [],
      });
    }
    const out = { target: chain.length ? chain[0].tag : null, chain };
    if (typeof console !== 'undefined') {
      let line = '[webc] why(' + x + ', ' + y + ') → ' + out.target;
      for (const c of chain) {
        line += '\n  ' + c.tag +
          (c.id !== undefined ? ' #' + c.id : '') +
          (c.hook ? ' hook=' + c.hook : '') +
          (c.listens.length ? ' listens=' + c.listens.join('|') : ' listens=NONE') +
          (c.display !== undefined ? ' display=' + c.display : '') +
          (c.pointerEvents !== undefined ? ' pointer-events=' + c.pointerEvents : '');
      }
      console.log(line);
    }
    return out;
  }

  return {
    apply, feed, hydrate, start, swap, nodes, diagnostics, why,
    call, dispatch: action,
    get instance() { return instance; },
  };
})();

// a classic <script> does not put `const` bindings on window: do it by hand
if (typeof globalThis !== 'undefined') globalThis.WebC = WEBC;
if (typeof module !== 'undefined' && module.exports) module.exports = WEBC;