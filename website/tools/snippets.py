#!/usr/bin/env python3
"""Compile every C snippet on the documentation site, exactly as it is written.

A snippet on this site is *self-complete*: a reader can copy it into a file, compile it, and
the only thing missing is the app around it — never a `#include`, a variable, a model or an
action from a neighbouring snippet. That is a property with a test, not a promise:

    python3 website/tools/snippets.py           # check every block, exit 1 on a failure
    python3 website/tools/snippets.py --list    # name every block it found
    python3 website/tools/snippets.py -v        # …and show the wrap it used

How the check works. Each ```c block (and each `<pre><code class="lang-c">` on the landing
page) is compiled on its own with `clang -std=c23 -fsyntax-only`:

  * a block that declares something at file scope (`#include`, `WEBC_MODEL`, `view(`,
    `GET(`, `action(`, `state(`, `client_main`, `TEST(` …) is a whole translation unit;
  * a block that is a few statements is the *body* of a function, and is wrapped in one.

Three rules the checker enforces on top of the compiler, because they are what "ambiguous"
meant when this was written:

  1. the first line is a comment — every snippet says what file it lives in and which half
     it compiles for (`// src/view.c — the page, compiled for both halves`);
  2. no `…` anywhere in the block, comments included: an ellipsis is a fragment pretending to
     be a snippet, and `action(save) { /* … */ }` is the shape that reads as complete and is
     not;
  3. no fragment that `return`s — a snippet with a `return` in it has to be a complete
     route or function, not the bottom half of one;
  4. no line wider than `MAX_COLS` — the site's article column fits 84 characters at the
     size the code is set in, and a line past the edge is a line the reader has to scroll
     sideways to finish reading.

Nothing else is added: no prologue header, no borrowed declarations. If a snippet grows a
dependency on a name it does not show, this fails, which is the point.
"""

from __future__ import annotations

import html
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SITE = HERE.parent                    # website/
REPO = SITE.parent                    # the repository root
CONTENT = SITE / "src" / "content"
LANDING = SITE / "src" / "landing.html"

INCLUDES = ["-I", str(REPO / "include"), "-I", str(REPO / "build" / "gen")]

# The article column fits 84 characters of code at the size the site sets it in. A longer
# line is not an error in C and is one for the reader, so it is a failure here.
MAX_COLS = 84
CFLAGS = ["-std=c23", "-fsyntax-only", "-Wall", "-Wno-everything"] + INCLUDES

# A block that declares any of these at file scope is a translation unit of its own.
TOP_LEVEL = re.compile(
    r"""^[ \t]*(?:
          \#include
        | WEBC_(?:MODEL|ROW|ENUM|DOCUMENT|MODEL_TABLE|MODEL_REF)
        | Vec\( | declare_view\( | view\( | PART\(
        | action[A-Za-z_]*\( | client_main\b
        | state[_a-z]*\( | computed[_a-z]*\( | effect[_a-z]*\(
        | TEST\( | TEST_MAIN\( | TEST_SETUP\(
        | static\b | typedef\b | struct\b | enum\b
        | (?:GET|POST|PUT|PATCH|DELETE|HEAD|OPTIONS|ANY)\(
        | (?:GET|POST|PUT|PATCH|DELETE)_MW\(
        | int[ \t]+main\(
    )""",
    re.X | re.M,
)

BLOCK = re.compile(r"^```([^\n`]*)\n(.*?)^```", re.M | re.S)
PRE = re.compile(r"<pre><code class=\"lang-c\">(.*?)</code></pre>", re.S)


class Snip:
    def __init__(self, where: str, line: int, code: str):
        self.where = where
        self.line = line
        self.code = code

    @property
    def unit(self) -> bool:
        return bool(TOP_LEVEL.search(self.code))

    def wrapped(self) -> str:
        if self.unit:
            return self.code
        # A body may still show the `#include` it needs: the preprocessor does not care
        # where a line sits, so the includes are lifted above the wrapper and the rest is
        # the function body. Nothing else is hoisted — a declaration stays where it is
        # written, which is what makes this a real check of the block as printed.
        head = [ln for ln in self.code.split("\n") if ln.lstrip().startswith("#include")]
        body = [ln for ln in self.code.split("\n") if ln not in head]
        text = "\n".join("  " + ln if ln.strip() else ln for ln in body)
        return "\n".join(head) + "\nstatic void wc_snippet(void)\n{\n" + text + "\n}\n"


def collect() -> list[Snip]:
    out: list[Snip] = []
    for f in sorted(CONTENT.rglob("*.md")):
        text = f.read_text()
        for m in BLOCK.finditer(text):
            lang = m.group(1).strip()
            if lang not in ("c", "h"):
                continue
            line = text[: m.start()].count("\n") + 1
            out.append(Snip(str(f.relative_to(SITE)), line, m.group(2)))
    # The agent skill is prose a reader (or a model) copies from, so its C blocks are held
    # to the same rule as the site's: no ellipsis, a leading comment, and it compiles.
    for f in sorted((REPO / "skills").glob("*/SKILL.md")):
        text = f.read_text()
        for m in BLOCK.finditer(text):
            if m.group(1).strip() not in ("c", "h"):
                continue
            line = text[: m.start()].count("\n") + 1
            out.append(Snip(str(f.relative_to(REPO)), line, m.group(2)))
    if LANDING.exists():
        text = LANDING.read_text()
        for m in PRE.finditer(text):
            line = text[: m.start()].count("\n") + 1
            out.append(Snip("src/landing.html", line, html.unescape(m.group(1))))
    return out


def strip_noise(code: str) -> str:
    """Code with comments and string/char literals blanked, for the shape checks."""
    out, i, n = [], 0, len(code)
    while i < n:
        c = code[i]
        if c == "/" and i + 1 < n and code[i + 1] == "/":
            while i < n and code[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and code[i + 1] == "*":
            i += 2
            while i + 1 < n and not (code[i] == "*" and code[i + 1] == "/"):
                i += 1
            i += 2
        elif c in "\"'":
            q = c
            i += 1
            while i < n and code[i] != q:
                i += 2 if code[i] == "\\" else 1
            i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def check(s: Snip, clang: str) -> list[str]:
    problems: list[str] = []

    first = s.code.strip().split("\n")[0].strip()
    if not first.startswith("//"):
        problems.append("no leading comment: say which file it lives in and which half it compiles for")

    wide = [(i + 1, len(ln)) for i, ln in enumerate(s.code.split("\n")) if len(ln) > MAX_COLS]
    if wide:
        n, w = wide[0]
        more = f" (and {len(wide) - 1} more)" if len(wide) > 1 else ""
        problems.append(f"line {n} is {w} columns, the column fits {MAX_COLS}{more}")

    if "…" in s.code or "..." in s.code:
        problems.append("contains an ellipsis, comment or not: a fragment, not a file")
    bare = strip_noise(s.code)
    if not s.unit and re.search(r"\breturn\b", bare):
        problems.append("fragment with a `return`: make it a complete route or function")

    src = Path("/tmp") / f"webc-snip-{abs(hash((s.where, s.line))) % 10**8}.c"
    src.write_text(s.wrapped())
    try:
        r = subprocess.run([clang, *CFLAGS, str(src)], capture_output=True, text=True)
    finally:
        src.unlink(missing_ok=True)
    if r.returncode != 0:
        errs = [ln for ln in r.stderr.split("\n") if " error: " in ln]
        detail = errs[0].split(" error: ", 1)[-1] if errs else r.stderr.strip().split("\n")[0]
        n = f" ({len(errs)} errors)" if len(errs) > 1 else ""
        problems.append(f"does not compile{n}: {detail}")
    return problems


def main() -> int:
    show_list = "--list" in sys.argv
    verbose = "-v" in sys.argv or "--verbose" in sys.argv
    clang = shutil.which("clang") or shutil.which("cc")
    if not clang:
        print("snippets: no clang on PATH", file=sys.stderr)
        return 2

    snips = collect()
    failed = 0
    for s in snips:
        problems = check(s, clang)
        tag = "unit" if s.unit else "body"
        if problems:
            failed += 1
            print(f"FAIL {s.where}:{s.line}  ({tag})")
            for p in problems:
                print(f"     {p}")
            if verbose:
                print("\n".join("     | " + ln for ln in s.code.strip().split("\n")))
                print()
        elif show_list or verbose:
            print(f"ok   {s.where}:{s.line}  ({tag})")

    print(f"snippets: {len(snips)} C blocks, {failed} failing")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())