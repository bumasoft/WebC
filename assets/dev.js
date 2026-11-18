// dev.js — the `webc dev` browser client.
//
// It is the only piece of the dev loop the developer actually sees, so it does as little
// to the page as it can get away with:
//
//   css:<href>   re-fetch one stylesheet, in place. No compile, no reload, no state lost.
//   {"t":"css"}  the same thing, as JSON.
//   {"t":"wasm"} stage the new module beside the running one and let webc.js commit it —
//                the DOM, the signals and the scroll position survive.
//   {"t":"error"} show the compiler's words over the page, and keep the last good build
//                running underneath.
//   {"t":"reload"} the whole document has to come again (a server change, a config change).
//   reload      the same thing, for an older `webc dev`.
//
// Two conveniences exist because a dev server that cannot be told "forget everything" is a
// dev server you restart: Ctrl/Cmd-Shift-R asks for a hard reload (and remembers it, so a
// swap does not quietly keep state you wanted gone), and WebC.hardReload() does the same
// from the console.
'use strict';
(function () {
  const port = window.__WEBC_DEV_PORT__ || 9999;
  const KEY = 'webc.hard-reload';
  let ws = null;
  let retry = 0;
  let overlay = null;
  let lastWasm = null;
  let building = 0;

  // A hard reload is a decision that has to survive the reload itself: the next load reads
  // this flag and tells webc.js not to transplant state into the fresh module.
  function hardReload(why) {
    try {
      sessionStorage.setItem(KEY, '1');
    } catch (e) {}
    if (why) console.info('[webc] hard reload: ' + why);
    location.reload();
  }
  window.WebC = window.WebC || {};
  window.WebC.hardReload = function () { hardReload('asked from the console'); };
  window.WebC.devState = function () {
    return { connected: !!ws && ws.readyState === 1, building: building, wasm: lastWasm };
  };

  window.addEventListener('keydown', function (e) {
    if ((e.metaKey || e.ctrlKey) && e.shiftKey && (e.key === 'R' || e.key === 'r')) {
      e.preventDefault();
      hardReload('keyboard');
    }
  });

  // ---------------------------------------------------------------- the overlay
  const CSS_TEXT =
    'position:fixed;inset:auto 0 0 0;z-index:2147483647;max-height:55vh;overflow:auto;' +
    'background:#1b1b1f;color:#f5f5f7;font:13px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;' +
    'border-top:2px solid #ff453a;padding:14px 18px;white-space:pre-wrap;box-shadow:0 -8px 30px rgba(0,0,0,.35)';

  function showError(items, text) {
    if (!overlay) {
      overlay = document.createElement('div');
      overlay.id = '__webc_dev_overlay';
      overlay.setAttribute('style', CSS_TEXT);
      overlay.addEventListener('click', function (e) {
        if (e.target && e.target.tagName === 'BUTTON') hideError();
      });
      document.body.appendChild(overlay);
    }
    overlay.textContent = '';
    const head = document.createElement('div');
    head.setAttribute('style', 'display:flex;gap:12px;align-items:center;margin-bottom:8px');
    const title = document.createElement('strong');
    title.setAttribute('style', 'color:#ff453a');
    title.textContent = 'build failed — the page you are looking at is the last good one';
    head.appendChild(title);
    const close = document.createElement('button');
    close.textContent = 'dismiss';
    close.setAttribute('style',
      'margin-left:auto;background:#2c2c30;color:#f5f5f7;border:1px solid #444;' +
      'border-radius:5px;padding:3px 10px;font:inherit;cursor:pointer');
    head.appendChild(close);
    overlay.appendChild(head);
    if (items && items.length) {
      // One row per site: a failed build reports the same diagnostic from more than one
      // compiler invocation, and the same sentence twice reads like two mistakes.
      const seen = new Set();
      for (const it of items) {
        const key = it.file + ':' + it.line + ':' + it.msg;
        if (seen.has(key)) continue;
        seen.add(key);
        const row = document.createElement('div');
        row.setAttribute('style', 'padding:2px 0');
        const where = document.createElement('span');
        where.setAttribute('style', 'color:#64d2ff');
        where.textContent = it.file + ':' + it.line + '  ';
        const what = document.createElement('span');
        what.textContent = it.msg;
        row.appendChild(where);
        row.appendChild(what);
        overlay.appendChild(row);
      }
    } else if (text) {
      const pre = document.createElement('div');
      pre.textContent = text;
      overlay.appendChild(pre);
    }
    document.documentElement.style.paddingBottom = '0';
    overlay.scrollTop = 0;
  }

  function hideError() {
    if (overlay && overlay.parentNode) overlay.parentNode.removeChild(overlay);
    overlay = null;
  }

  // ---------------------------------------------------------------- stylesheets
  function swapCss(href) {
    const links = document.querySelectorAll('link[rel=stylesheet]');
    let hit = 0;
    for (const l of links) {
      const url = l.getAttribute('href') || '';
      // match on the path: absolute and relative hrefs both have to work, and a page may
      // carry a cache-buster from a previous swap
      const path = url.split('?')[0];
      if (path === href || path.endsWith(href) || url.includes(href)) {
        const next = href + (href.includes('?') ? '&' : '?') + 't=' + Date.now();
        // a fresh <link> swaps without a flash of unstyled content: the old sheet stays
        // applied until the new one has loaded
        const clone = l.cloneNode();
        clone.href = next;
        clone.addEventListener('load', function () {
          if (l.parentNode) l.parentNode.removeChild(l);
        });
        clone.addEventListener('error', function () {
          if (clone.parentNode) clone.parentNode.removeChild(clone);
        });
        l.parentNode.insertBefore(clone, l.nextSibling);
        hit++;
      }
    }
    if (!hit) location.reload(); // a stylesheet we did not find: the honest fallback
  }

  // ---------------------------------------------------------------- the socket
  function connect() {
    ws = new WebSocket('ws://' + location.hostname + ':' + port + '/ws');
    ws.onopen = function () {
      retry = 0;
      console.log('[webc] dev connected');
      ws.send(JSON.stringify({ t: 'hello', v: 1 }));
    };
    ws.onmessage = function (m) {
      const data = m.data;
      if (data === 'reload') return hardReload('reload from the dev server');
      if (data.indexOf('css:') === 0) return swapCss(data.slice(4));
      let msg = null;
      try {
        msg = JSON.parse(data);
      } catch (e) {
        return;
      }
      if (!msg || !msg.t) return;
      if (msg.t === 'css') swapCss(msg.href);
      else if (msg.t === 'status') {
        building = msg.state === 'building' ? Date.now() : 0;
        if (msg.state === 'ok') hideError();
        if (window.WebC && window.WebC.__devStatus) window.WebC.__devStatus(msg);
      } else if (msg.t === 'error') showError(msg.items, msg.text);
      else if (msg.t === 'reload') hardReload(msg.why || 'server');
      else if (msg.t === 'wasm') {
        lastWasm = msg;
        // Staged by webc.js: fetch, instantiate, transplant state, commit one batch. If
        // there is no bridge on this page (a plain HTML file with no mount), the only way
        // to see the new module is a reload.
        if (window.WebC && window.WebC.swap) {
          window.WebC.swap(msg.url, { keep: msg.keep }).catch(function (e) {
            console.warn('[webc] module swap failed, reloading: ' + e);
            hardReload('swap failed');
          });
        } else {
          hardReload('no bridge on this page');
        }
      }
    };
    ws.onclose = function () {
      retry = Math.min(retry + 1, 10);
      setTimeout(connect, 200 * retry);
    };
    ws.onerror = function () {
      try {
        ws.close();
      } catch (e) {}
    };
  }

  // ---------------------------------------------------------------- devtools
  //
  // The ops stream, made visible — the first slice of a panel, in the only script
  // that already sits in the page. WebC.devtools() (or ?__webc_devtools) opens it:
  // the bridge's own diagnostics, live, and `why()` as a mode you arm and click.
  // No new bridge verbs, no second origin: everything shown is what the page knows.
  let panel = null, panelTimer = null, whyArmed = false;

  function panelRow(k, v, warn) {
    const r = document.createElement('div');
    r.setAttribute('style', 'display:flex;gap:10px;padding:1px 0');
    const a = document.createElement('span');
    a.setAttribute('style', 'color:#98989f;flex:0 0 92px');
    a.textContent = k;
    const b = document.createElement('span');
    if (warn) b.setAttribute('style', 'color:#ff453a');
    b.textContent = v;
    r.appendChild(a); r.appendChild(b);
    return r;
  }

  function panelBody() {
    if (!panel) return;
    const body = panel.querySelector('.__webc_dt_body');
    if (!body) return;
    body.textContent = '';
    const st = window.WebC && window.WebC.devState ? window.WebC.devState() : {};
    let d = null;
    try { d = window.WebC && window.WebC.diagnostics ? window.WebC.diagnostics() : null; }
    catch (e) {}
    body.appendChild(panelRow('dev server', st.connected ? 'connected' : 'reconnecting', !st.connected));
    body.appendChild(panelRow('last build', st.building ? 'building…' : st.wasm ? 'ok (swapped)' : 'ok'));
    if (!d) {
      const n = document.createElement('div');
      n.setAttribute('style', 'color:#98989f;padding-top:6px');
      n.textContent = 'no bridge on this page — WebC.diagnostics() is not there';
      body.appendChild(n);
      return;
    }
    body.appendChild(panelRow('ops', String(d.ops)));
    body.appendChild(panelRow('bad ops', String(d.badOps), d.badOps > 0));
    if (d.lastBad) body.appendChild(panelRow('last bad', String(d.lastBad), true));
    body.appendChild(panelRow('hydrated', String(d.hydrated), d.hydrated === false));
    body.appendChild(panelRow('anchors', String(d.anchors)));
    body.appendChild(panelRow('created els', String(d.createdEls)));
  }

  function whyClick(e) {
    if (!whyArmed) return;
    whyArmed = false;
    e.preventDefault(); e.stopPropagation();
    let out = '';
    try { out = window.WebC.why(e.clientX, e.clientY); } catch (err) { out = String(err); }
    if (panel) {
      const w = panel.querySelector('.__webc_dt_why');
      if (w) {
        // why() walks the ancestors in the console; what it returns is the summary
        w.textContent = out ? (typeof out === 'object' ? JSON.stringify(out, null, 1) : String(out))
                            : 'walked in the console — open it for the listeners above (' +
                              e.clientX + ', ' + e.clientY + ')';
        w.style.display = 'block';
      }
    }
    console.log('[webc] why(' + e.clientX + ', ' + e.clientY + ')\n' + out);
  }

  window.WebC.devtools = function () {
    if (panel) { // toggle off
      clearInterval(panelTimer);
      document.removeEventListener('click', whyClick, true);
      if (panel.parentNode) panel.parentNode.removeChild(panel);
      panel = null;
      return;
    }
    panel = document.createElement('div');
    panel.setAttribute('style',
      'position:fixed;top:12px;right:12px;z-index:2147483647;width:360px;max-height:60vh;overflow:auto;' +
      'background:#1b1b1f;color:#f5f5f7;font:12px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;' +
      'border:1px solid #3a3a40;border-radius:8px;padding:12px 14px;box-shadow:0 8px 30px rgba(0,0,0,.45)');
    const head = document.createElement('div');
    head.setAttribute('style', 'display:flex;gap:10px;align-items:center;margin-bottom:8px');
    const title = document.createElement('strong');
    title.textContent = 'webc devtools';
    head.appendChild(title);
    const whyBtn = document.createElement('button');
    whyBtn.textContent = 'why was my click eaten?';
    whyBtn.setAttribute('style',
      'margin-left:auto;background:#2c2c30;color:#f5f5f7;border:1px solid #444;border-radius:5px;' +
      'padding:3px 10px;font:inherit;cursor:pointer');
    whyBtn.addEventListener('click', function () { whyArmed = true; whyBtn.textContent = 'click the element…'; });
    head.appendChild(whyBtn);
    panel.appendChild(head);
    const body = document.createElement('div');
    body.className = '__webc_dt_body';
    panel.appendChild(body);
    const why = document.createElement('pre');
    why.className = '__webc_dt_why';
    why.setAttribute('style', 'display:none;margin-top:8px;padding-top:8px;border-top:1px solid #3a3a40;' +
      'white-space:pre-wrap;color:#64d2ff');
    panel.appendChild(why);
    document.body.appendChild(panel);
    panelBody();
    panelTimer = setInterval(panelBody, 500);
    document.addEventListener('click', whyClick, true);
    console.log('[webc] devtools: open. Click the button, then the element that ignores you.');
  };
  if (location.search.indexOf('__webc_devtools') >= 0) window.WebC.devtools();

  connect();
})();