/* A small syntax highlighter, 3 KB, no dependency.
 *
 * The generator escapes code before it writes it, so the first job here is to decode the five
 * entities it uses, tokenize the real text, and escape each token on the way out. A lexer that
 * works on escaped text gets `&quot;` where a string should start, and every language below
 * would be wrong in the same way.
 *
 * It favours the shape of real C: comments, strings, preprocessor lines, numbers, and then
 * identifiers coloured by what they look like — a keyword, an ALL_CAPS macro, a Capitalised type,
 * a name followed by `(` (a call), or nothing.
 */
(function () {
  "use strict";

  var esc = function (s) {
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  };
  var unesc = function (s) {
    return s
      .replace(/&quot;/g, '"')
      .replace(/&#39;/g, "'")
      .replace(/&lt;/g, "<")
      .replace(/&gt;/g, ">")
      .replace(/&amp;/g, "&");
  };

  function tokenize(src, rules) {
    var out = [], plain = "", i = 0;
    while (i < src.length) {
      var hit = null;
      for (var k = 0; k < rules.length; k++) {
        var r = rules[k];
        r.re.lastIndex = i;
        var m = r.re.exec(src);
        if (m && m.index === i && m[0].length) {
          hit = { cls: typeof r.cls === "function" ? r.cls(m[0], src, i) : r.cls, text: m[0] };
          break;
        }
      }
      if (hit) {
        if (plain) { out.push([null, plain]); plain = ""; }
        out.push([hit.cls, hit.text]);
        i += hit.text.length;
      } else {
        plain += src[i++];
      }
    }
    if (plain) out.push([null, plain]);
    return out;
  }

  function paint(tokens) {
    var html = "";
    for (var i = 0; i < tokens.length; i++) {
      var cls = tokens[i][0], text = esc(tokens[i][1]);
      html += cls ? '<span class="' + cls + '">' + text + "</span>" : text;
    }
    return html;
  }

  // ------------------------------------------------------------------ C
  var C_KEY = ("if else for while do switch case default break continue return goto sizeof " +
    "typedef struct union enum static extern const volatile inline register signed unsigned " +
    "void char short int long float double bool true false nullptr NULL auto alignof typeof " +
    "alignas static_assert _Generic _Atomic restrict").split(" ");

  var RULES_C = [
    { re: /\/\*[\s\S]*?\*\//y, cls: "tok-com" },
    { re: /\/\/[^\n]*/y, cls: "tok-com" },
    { re: /#[ \t]*[a-zA-Z_]\w*/y, cls: "tok-mac" },
    { re: /"(?:[^"\\\n]|\\.)*"/y, cls: "tok-str" },
    { re: /'(?:[^'\\\n]|\\.)*'/y, cls: "tok-str" },
    { re: /(?:\b0[xX][0-9a-fA-F]+|\b\d[\d_]*(?:\.\d+)?(?:[eE][+-]?\d+)?)[uUlLfF]*/y, cls: "tok-num" },
    {
      re: /[A-Za-z_]\w*/y,
      cls: function (text, src, at) {
        var after = src.slice(at + text.length).match(/^\s*(.)/);
        var next = after ? after[1] : "";
        if (C_KEY.indexOf(text) >= 0) return "tok-key";
        if (/^_*[A-Z][A-Z0-9_]*$/.test(text)) return "tok-mac";
        if (next === "(") return "tok-fn";
        if (/^[A-Z]/.test(text)) return "tok-type";
        if (/^(i8|i16|i32|i64|u8|u16|u32|u64|f32|f64|str|Text|Bytes|Arena|bool|isize|usize)$/.test(text)) return "tok-type";
        return null;
      }
    }
  ];

  // ------------------------------------------------------------------ shell / console
  var SH_KEY = ("webc make cd curl git clang gcc mkdir rm cp mv ls cat echo export sudo open " +
    "grep find python3 tar gzip ssh scp docker").split(" ");

  var RULES_SH = [
    { re: /^\s*[$\u276f]\s/y, cls: "tok-punct" },
    { re: /#[^\n]*/y, cls: "tok-com" },
    { re: /"(?:[^"\\]|\\.)*"/y, cls: "tok-str" },
    { re: /'[^']*'/y, cls: "tok-str" },
    { re: /--?[A-Za-z][\w-]*/y, cls: "tok-mac" },
    { re: /\$\w+|\$\{[^}]*\}/y, cls: "tok-type" },
    { re: /\b\d+(?:\.\d+)*\b/y, cls: "tok-num" },
    {
      re: /[A-Za-z_][\w./-]*/y,
      cls: function (text, src, at) {
        if (SH_KEY.indexOf(text) >= 0) return "tok-key";
        var before = src.slice(0, at).match(/(\S+)\s*$/);
        var lead = before ? before[1] : "";
        if (lead === "webc" || lead === "make") return "tok-prop";
        if (/^[/~.]/.test(text)) return "tok-str";
        return null;
      }
    }
  ];

  // ------------------------------------------------------------------ JSON
  var RULES_JSON = [
    { re: /"(?:[^"\\\n]|\\.)*"\s*(?=:)/y, cls: "tok-prop" },
    { re: /"(?:[^"\\\n]|\\.)*"/y, cls: "tok-str" },
    { re: /-?\b\d+(?:\.\d+)?(?:[eE][+-]?\d+)?\b/y, cls: "tok-num" },
    { re: /\b(?:true|false|null)\b/y, cls: "tok-key" },
    { re: /[{}[\],:]/y, cls: "tok-punct" }
  ];

  // ------------------------------------------------------------------ SQL
  var SQL_KEY = ("select from where insert into values update set delete create table drop " +
    "alter add index unique primary key foreign references not null default join left right " +
    "inner outer on group by order limit offset as and or in exists returning begin commit " +
    "rollback transaction integer text real blob boolean timestamp autoincrement if cascade " +
    "constraint case when then else end distinct count sum avg min max desc asc").split(" ");

  var RULES_SQL = [
    { re: /--[^\n]*/y, cls: "tok-com" },
    { re: /\/\*[\s\S]*?\*\//y, cls: "tok-com" },
    { re: /'(?:[^']|'')*'/y, cls: "tok-str" },
    { re: /\b\d+(?:\.\d+)?\b/y, cls: "tok-num" },
    {
      re: /[A-Za-z_]\w*/y,
      cls: function (text) {
        return SQL_KEY.indexOf(text.toLowerCase()) >= 0 ? "tok-key" : null;
      }
    }
  ];

  // ------------------------------------------------------------------ HTML
  var RULES_HTML = [
    { re: /<!--[\s\S]*?-->/y, cls: "tok-com" },
    { re: /<\/?[a-zA-Z][\w-]*/y, cls: "tok-key" },
    { re: /"[^"]*"/y, cls: "tok-str" },
    { re: /[a-zA-Z-]+(?==)/y, cls: "tok-attr" }
  ];

  // ------------------------------------------------------------------ CSS
  var RULES_CSS = [
    { re: /\/\*[\s\S]*?\*\//y, cls: "tok-com" },
    { re: /@[\w-]+/y, cls: "tok-mac" },
    { re: /"[^"]*"|'[^']*'/y, cls: "tok-str" },
    { re: /#[0-9a-fA-F]{3,8}\b/y, cls: "tok-num" },
    { re: /--[\w-]+/y, cls: "tok-prop" },
    { re: /[-a-zA-Z]+(?=\s*:)/y, cls: "tok-prop" },
    { re: /\b\d+(?:\.\d+)?(?:px|rem|em|vh|vw|%|s|ms|fr|ch)?\b/y, cls: "tok-num" }
  ];

  // ------------------------------------------------------------------ JavaScript
  var JS_KEY = ("var let const function return if else for while do switch case break continue " +
    "new class extends import export from default await async yield try catch finally throw " +
    "typeof instanceof null undefined true false this of in delete void").split(" ");

  var RULES_JS = [
    { re: /\/\*[\s\S]*?\*\//y, cls: "tok-com" },
    { re: /\/\/[^\n]*/y, cls: "tok-com" },
    { re: /"(?:[^"\\\n]|\\.)*"|'(?:[^'\\\n]|\\.)*'|`(?:[^`\\]|\\.)*`/y, cls: "tok-str" },
    { re: /\b\d+(?:\.\d+)?\b/y, cls: "tok-num" },
    {
      re: /[A-Za-z_$][\w$]*/y,
      cls: function (text, src, at) {
        if (JS_KEY.indexOf(text) >= 0) return "tok-key";
        if (/^\s*\(/.test(src.slice(at + text.length))) return "tok-fn";
        if (/^[A-Z]/.test(text)) return "tok-type";
        return null;
      }
    }
  ];

  var MAP = {
    c: RULES_C, h: RULES_C, bash: RULES_SH, console: RULES_SH, sh: RULES_SH,
    json: RULES_JSON, sql: RULES_SQL, html: RULES_HTML, css: RULES_CSS, js: RULES_JS
  };

  function run() {
    var blocks = document.querySelectorAll("pre > code[class*='lang-']");
    for (var i = 0; i < blocks.length; i++) {
      var el = blocks[i];
      var lang = el.className.replace("lang-", "");
      var rules = MAP[lang];
      if (!rules) continue;
      el.innerHTML = paint(tokenize(unesc(el.textContent), rules));
    }
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", run);
  } else {
    run();
  }
})();