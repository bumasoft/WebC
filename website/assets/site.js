/* The site's behaviour: theme, copy buttons, the on-this-page rail, the mobile drawer, and
 * the search dialog. No build step, no framework, no network. */
(function () {
  "use strict";

  var root = document.documentElement;
  var ROOT = root.dataset.root || "";

  /* ------------------------------------------------------------ theme */
  function setTheme(theme) {
    root.dataset.theme = theme;
    try { localStorage.setItem("webc-theme", theme); } catch (e) {}
    var btn = document.querySelector("[data-theme-toggle]");
    if (btn) btn.setAttribute("aria-label", theme === "light" ? "Switch to the dark theme" : "Switch to the light theme");
  }
  var themeBtn = document.querySelector("[data-theme-toggle]");
  if (themeBtn) {
    themeBtn.addEventListener("click", function () {
      setTheme(root.dataset.theme === "light" ? "dark" : "light");
    });
  }

  /* ------------------------------------------------------------ copy */
  document.addEventListener("click", function (ev) {
    var btn = ev.target.closest("[data-copy]");
    if (!btn) return;
    var box = btn.closest(".code");
    var code = box && box.querySelector("code");
    if (!code) return;
    var text = code.textContent;
    var done = function () {
      btn.textContent = "Copied";
      btn.classList.add("is-done");
      setTimeout(function () {
        btn.textContent = "Copy";
        btn.classList.remove("is-done");
      }, 1400);
    };
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).then(done, function () { legacy(); });
    } else {
      legacy();
    }
    function legacy() {
      var ta = document.createElement("textarea");
      ta.value = text;
      ta.setAttribute("readonly", "");
      ta.style.position = "fixed";
      ta.style.opacity = "0";
      document.body.appendChild(ta);
      ta.select();
      try { document.execCommand("copy"); done(); } catch (e) {}
      document.body.removeChild(ta);
    }
  });

  /* ------------------------------------------------------------ on this page */
  /* The rail marks the section you are *reading*, which is the last heading that has passed the
   * top of the column — not the heading with the most pixels showing. A ratio-based observer
   * marks a heading that is merely in view, so it lags one section behind on a long page. */
  var tocLinks = Array.prototype.slice.call(document.querySelectorAll(".toc a"));
  var tocHeads = tocLinks
    .map(function (a) { return { a: a, el: document.getElementById(a.getAttribute("href").slice(1)) }; })
    .filter(function (x) { return x.el; });

  if (tocHeads.length) {
    var READ_LINE = 130, ticking = false;
    var markToc = function () {
      ticking = false;
      var current = tocHeads[0];
      for (var i = 0; i < tocHeads.length; i++) {
        if (tocHeads[i].el.getBoundingClientRect().top - READ_LINE <= 0) current = tocHeads[i];
      }
      if (current.a.classList.contains("is-active")) return;
      tocLinks.forEach(function (a) { a.classList.remove("is-active"); });
      current.a.classList.add("is-active");
    };
    var onScroll = function () {
      if (!ticking) { ticking = true; window.requestAnimationFrame(markToc); }
    };
    window.addEventListener("scroll", onScroll, { passive: true });
    window.addEventListener("resize", onScroll, { passive: true });
    markToc();
  }

  /* ------------------------------------------------------------ wide code */
  /* A code block wider than the column is cut at the edge with no sign that it continues —
   * the reader's only clue is a scrollbar that macOS draws on hover and a phone draws never.
   * So the block says so: `is-wide` paints a fade on the edge it is cut at, and the class
   * follows the block through a resize. */
  var markWide = function () {
    Array.prototype.forEach.call(document.querySelectorAll(".code"), function (box) {
      var pre = box.querySelector("pre");
      if (!pre) return;
      box.classList.toggle("is-wide", pre.scrollWidth - pre.clientWidth > 2);
      box.classList.toggle("is-scrolled", pre.scrollLeft > 2);
    });
  };
  Array.prototype.forEach.call(document.querySelectorAll(".code pre"), function (pre) {
    pre.addEventListener("scroll", function () {
      var box = pre.closest(".code");
      if (box) box.classList.toggle("is-scrolled", pre.scrollLeft > 2);
    }, { passive: true });
  });
  window.addEventListener("resize", markWide, { passive: true });
  markWide();

  /* ------------------------------------------------------------ mobile drawer */
  var menuBtn = document.querySelector("[data-menu]");
  if (menuBtn) {
    var close = function () {
      document.body.classList.remove("nav-open");
      menuBtn.setAttribute("aria-expanded", "false");
    };
    menuBtn.addEventListener("click", function () {
      var open = document.body.classList.toggle("nav-open");
      menuBtn.setAttribute("aria-expanded", open ? "true" : "false");
    });
    document.addEventListener("click", function (ev) {
      if (document.body.classList.contains("nav-open") &&
          !ev.target.closest("[data-sidebar]") && !ev.target.closest("[data-menu]")) close();
    });
    document.addEventListener("keydown", function (ev) {
      if (ev.key === "Escape") close();
    });
    document.querySelectorAll("[data-sidebar] a").forEach(function (a) {
      a.addEventListener("click", close);
    });
  }

  /* ------------------------------------------------------------ search */
  var data = window.__WEBC_SEARCH__ || [];
  var dialog = document.querySelector("[data-search-dialog]");
  var input = document.querySelector("[data-search-input]");
  var results = document.querySelector("[data-search-results]");
  var openers = document.querySelectorAll("[data-search-open]");
  var cursor = 0, hits = [];

  var isMac = /Mac|iPhone|iPad/.test(navigator.platform || navigator.userAgent);
  document.querySelectorAll(".searchbtn__kbd").forEach(function (k) {
    k.textContent = isMac ? "⌘K" : "Ctrl K";
  });

  function openSearch() {
    if (!dialog) return;
    if (typeof dialog.showModal === "function") dialog.showModal();
    else dialog.setAttribute("open", "");
    setTimeout(function () { input.focus(); input.select(); }, 10);
  }
  function closeSearch() {
    if (!dialog) return;
    if (typeof dialog.close === "function") dialog.close();
    else dialog.removeAttribute("open");
  }
  openers.forEach(function (b) { b.addEventListener("click", openSearch); });
  document.querySelectorAll("[data-search-close]").forEach(function (b) {
    b.addEventListener("click", closeSearch);
  });
  document.addEventListener("keydown", function (ev) {
    var typing = /^(INPUT|TEXTAREA|SELECT)$/.test(document.activeElement.tagName);
    if ((ev.key === "k" || ev.key === "K") && (ev.metaKey || ev.ctrlKey)) {
      ev.preventDefault(); openSearch(); return;
    }
    if (ev.key === "/" && !typing) { ev.preventDefault(); openSearch(); return; }
    if (!dialog || !dialog.open) return;
    if (ev.key === "ArrowDown") { ev.preventDefault(); move(1); }
    else if (ev.key === "ArrowUp") { ev.preventDefault(); move(-1); }
    else if (ev.key === "Enter") {
      if (hits[cursor]) { ev.preventDefault(); location.href = ROOT + hits[cursor].p; }
    } else if (ev.key === "Escape" && dialog.open) { closeSearch(); }
  });

  function move(step) {
    if (!hits.length) return;
    cursor = (cursor + step + hits.length) % hits.length;
    var nodes = results.querySelectorAll(".search__hit");
    nodes.forEach(function (n, i) { n.classList.toggle("is-cursor", i === cursor); });
    if (nodes[cursor]) nodes[cursor].scrollIntoView({ block: "nearest" });
  }

  function snippet(text, terms) {
    var lower = text.toLowerCase();
    var at = -1;
    for (var i = 0; i < terms.length; i++) {
      var j = lower.indexOf(terms[i]);
      if (j >= 0 && (at < 0 || j < at)) at = j;
    }
    if (at < 0) at = 0;
    var start = Math.max(0, at - 70);
    var out = (start > 0 ? "…" : "") + text.slice(start, start + 190) + (text.length > start + 190 ? "…" : "");
    terms.forEach(function (t) {
      if (!t) return;
      var re = new RegExp("(" + t.replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + ")", "gi");
      out = out.replace(re, "\u0001$1\u0002");
    });
    return out.replace(/&/g, "&amp;").replace(/</g, "&lt;")
      .replace(/\u0001/g, "<mark>").replace(/\u0002/g, "</mark>");
  }

  function score(entry, terms) {
    var title = entry.t.toLowerCase();
    var heads = (entry.h || []).join(" \u0000 ").toLowerCase();
    var body = (entry.b || "").toLowerCase();
    var total = 0;
    for (var i = 0; i < terms.length; i++) {
      var t = terms[i], hit = false;
      if (title.indexOf(t) >= 0) { total += title.indexOf(t) === 0 ? 60 : 34; hit = true; }
      if (heads.indexOf(t) >= 0) { total += 16; hit = true; }
      var at = body.indexOf(t);
      if (at >= 0) { total += 5; hit = true; }
      if (!hit) return 0;
    }
    return total;
  }

  function render() {
    var q = (input.value || "").trim().toLowerCase();
    if (!q) {
      results.innerHTML = '<div class="search__empty">' + data.length +
        ' pages indexed. Try <b>arena</b>, <b>model</b>, <b>comp</b>, <b>route</b> — or start typing.</div>';
      hits = []; cursor = 0;
      return;
    }
    var terms = q.split(/\s+/).filter(Boolean);
    hits = data
      .map(function (entry) { return { entry: entry, score: score(entry, terms) }; })
      .filter(function (h) { return h.score > 0; })
      .sort(function (a, b) { return b.score - a.score; })
      .slice(0, 9)
      .map(function (h) { return h.entry; });

    if (!hits.length) {
      results.innerHTML = '<div class="search__empty">Nothing for “' +
        q.replace(/&/g, "&amp;").replace(/</g, "&lt;") + '”.</div>';
      return;
    }
    cursor = 0;
    results.innerHTML = hits.map(function (e, i) {
      return '<a class="search__hit' + (i === 0 ? " is-cursor" : "") + '" href="' + ROOT + e.p + '">' +
        '<span class="search__where">' + e.g + "</span>" +
        '<div class="search__title">' + e.t.replace(/&/g, "&amp;").replace(/</g, "&lt;") + "</div>" +
        '<span class="search__snip">' + snippet(e.b || "", terms) + "</span></a>";
    }).join("");
  }

  if (input) {
    input.addEventListener("input", render);
    input.addEventListener("keydown", function (ev) { if (ev.key === "Enter") ev.preventDefault(); });
    render();
  }
})();