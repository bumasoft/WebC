"""A small, strict-enough Markdown renderer for the WebC docs site.

Only what `docs/*.md` actually uses: ATX headings (with the `<a name="x"></a>` anchors the
reference writes), fenced code, tables, nested lists, blockquotes, rules, and inline code /
emphasis / links. No dependency, no HTML pass-through except the anchors and the handful of
raw tags the docs contain on purpose (`<br>`, `<module>`, `<id ...>` inside prose are escaped
by hand where they are not meant as HTML).

The contract: `render(text)` returns `(html, headings)` where a heading is
`{"level": 2, "id": "core", "text": "Core"}` — enough to build a table of contents, an
in-page anchor, search entries and cross-page links.
"""

from __future__ import annotations

import html as _html
import re

FENCE = re.compile(r"^(\s*)(`{3,}|~{3,})\s*([\w+-]*)")
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*$")
RULE = re.compile(r"^\s*(-{3,}|\*{3,}|_{3,})\s*$")
UL = re.compile(r"^(\s*)([-*+])\s+(.*)$")
OL = re.compile(r"^(\s*)(\d{1,3})[.)]\s+(.*)$")
TABLE_SEP = re.compile(r"^\s*\|?\s*:?-{2,}:?\s*(\|\s*:?-{2,}:?\s*)*\|?\s*$")
ANCHOR = re.compile(r'^<a\s+name="([^"]+)"\s*>\s*</a>\s*')

# Fenced-block languages the highlighter knows. Anything else renders as plain text.
LANGS = {
    "c": "c",
    "h": "c",
    "console": "console",
    "sh": "bash",
    "bash": "bash",
    "shell": "bash",
    "json": "json",
    "js": "js",
    "javascript": "js",
    "html": "html",
    "css": "css",
    "sql": "sql",
    "toml": "toml",
    "make": "make",
    "text": "text",
    "": "text",
}


def slug(text: str) -> str:
    """A stable anchor for a heading: lowercase, alphanumerics, single dashes."""
    text = re.sub(r"<[^>]+>", "", text)
    text = text.replace("`", "")
    text = text.lower()
    text = re.sub(r"[^a-z0-9]+", "-", text)
    return text.strip("-") or "section"


def _esc(text: str) -> str:
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


class _Inline:
    """Inline rendering: code spans first (they win over everything), then the rest."""

    def __init__(self, link):
        self.link = link  # a function (href) -> href, so pages can rewrite `#anchor`

    def render(self, text: str) -> str:
        spans: list[str] = []

        def stash(m):
            spans.append(f"<code>{_esc(m.group(2).strip())}</code>")
            return f"\x00{len(spans) - 1}\x00"

        # ``code with ` in it`` then `code`
        text = re.sub(r"``(.+?)``", lambda m: stash(_CodeMatch(m.group(1))), text)
        text = re.sub(r"`([^`]+?)`", lambda m: stash(_CodeMatch(m.group(1))), text)

        out = _esc(text)

        # links: [text](url)
        def link(m):
            label, href = m.group(1), m.group(2).strip()
            href = self.link(href)
            ext = href.startswith("http") and "github.com/marian" not in href
            attrs = ' target="_blank" rel="noreferrer"' if ext else ""
            return f'<a href="{_esc(href)}"{attrs}>{label}</a>'

        # images: ![alt](src) — an <img>, checked before the link rule so the `!` is not left
        # dangling in front of an <a>. The source goes through the same resolver as a link, so
        # `media/shot.png` and `../assets/media/shot.png` both work from a page or a repo file.
        def image(m):
            alt, src = m.group(1), m.group(2).strip()
            return (f'<img src="{_esc(self.link(src))}" alt="{_esc(alt)}" '
                    'loading="lazy" decoding="async">')

        out = re.sub(r"!\[([^\]]*)\]\(([^)\s]+)\)", image, out)

        out = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", link, out)

        # emphasis: **bold**, *italic*, ~~strike~~
        out = re.sub(r"\*\*\*(.+?)\*\*\*", r"<strong><em>\1</em></strong>", out)
        out = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", out)
        out = re.sub(r"(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])", r"<em>\1</em>", out)
        out = re.sub(r"~~(.+?)~~", r"<del>\1</del>", out)

        # bare URLs in angle brackets and out in the open
        out = re.sub(r"&lt;(https?://[^&\s]+)&gt;", r'<a href="\1" target="_blank" rel="noreferrer">\1</a>', out)

        # two trailing spaces = a hard break (kept from the source)
        out = out.replace("\x02", "<br>")

        def unstash(m):
            return spans[int(m.group(1))]

        return re.sub(r"\x00(\d+)\x00", unstash, out)


class _CodeMatch:
    def __init__(self, text):
        self.group = lambda n: text if n == 2 else None


def _split_row(line: str) -> list[str]:
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|"):
        line = line[:-1]
    cells, buf, i = [], "", 0
    while i < len(line):
        ch = line[i]
        if ch == "\\" and i + 1 < len(line) and line[i + 1] == "|":
            buf += "|"
            i += 2
            continue
        if ch == "|":
            cells.append(buf.strip())
            buf = ""
        else:
            buf += ch
        i += 1
    cells.append(buf.strip())
    return cells


def code_block(code: str, lang: str, title: str | None = None) -> str:
    lang = LANGS.get(lang, lang or "text")
    body = "\n".join(line.rstrip() for line in code.strip("\n").split("\n"))
    head = ""
    if title:
        head = (
            '<div class="code__bar">'
            f'<span class="code__name">{_esc(title)}</span>'
            '<button class="code__copy" type="button" data-copy aria-label="Copy code">Copy</button>'
            "</div>"
        )
    else:
        head = (
            '<div class="code__bar">'
            f'<span class="code__lang">{_esc(lang)}</span>'
            '<button class="code__copy" type="button" data-copy aria-label="Copy code">Copy</button>'
            "</div>"
        )
    return (
        f'<div class="code" data-lang="{_esc(lang)}">{head}'
        f'<pre><code class="lang-{_esc(lang)}">{_esc(body)}</code></pre></div>'
    )


def render(text: str, link=None, shift: int = 0, page_title: str = "") -> tuple[str, list[dict]]:
    """Markdown -> (html, headings). `link` rewrites hrefs; `shift` demotes headings."""
    if link is None:
        link = lambda href: href
    inline = _Inline(link)

    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    out: list[str] = []
    headings: list[dict] = []
    seen_ids: dict[str, int] = {}
    i, n = 0, len(lines)

    def unique(base: str) -> str:
        if base not in seen_ids:
            seen_ids[base] = 1
            return base
        seen_ids[base] += 1
        return f"{base}-{seen_ids[base]}"

    def is_block_start(k: int) -> bool:
        if k >= n:
            return True
        line = lines[k]
        if not line.strip():
            return True
        return bool(
            FENCE.match(line)
            or HEADING.match(line)
            or RULE.match(line)
            or line.lstrip().startswith(">")
            or UL.match(line)
            or OL.match(line)
            or (line.lstrip().startswith("|") and k + 1 < n and TABLE_SEP.match(lines[k + 1]))
        )

    while i < n:
        line = lines[i]

        if not line.strip():
            i += 1
            continue

        # ---- fenced code
        fence = FENCE.match(line)
        if fence:
            indent, marker, lang = fence.group(1), fence.group(2), fence.group(3)
            title = None
            # ```c title "src/view.c" — a fence label the docs use for a filename
            rest = line[fence.end():].strip()
            m = re.match(r'^(?:title\s+|file\s+)?["\']?(.+?)["\']?$', rest) if rest else None
            if m and (rest.startswith('"') or rest.startswith("title") or "/" in rest or rest.endswith(".c")):
                title = m.group(1).strip('"\'')
            body: list[str] = []
            i += 1
            close = re.compile(r"^\s*" + re.escape(marker[0]) + "{" + str(len(marker)) + r",}\s*$")
            while i < n and not close.match(lines[i]):
                body.append(lines[i][len(indent):] if lines[i].startswith(indent) else lines[i])
                i += 1
            i += 1
            out.append(code_block("\n".join(body), lang, title))
            continue

        # ---- heading
        head = HEADING.match(line)
        if head:
            level = len(head.group(1))
            raw = head.group(2)
            anchor = ANCHOR.match(raw)
            if anchor:
                hid = anchor.group(1)
                raw = raw[anchor.end():]
            else:
                hid = unique(slug(raw))
            text_html = inline.render(raw)
            shown = re.sub(r"<[^>]+>", "", text_html)
            lvl = max(1, min(6, level + shift))
            headings.append({"level": lvl, "id": hid, "text": shown})
            anchor_link = (
                f'<a class="h-anchor" href="#{hid}" aria-label="Link to this section">#</a>'
            )
            out.append(f'<h{lvl} id="{hid}">{text_html}{anchor_link}</h{lvl}>')
            i += 1
            continue

        # ---- rule
        if RULE.match(line):
            out.append("<hr>")
            i += 1
            continue

        # ---- table
        if line.lstrip().startswith("|") and i + 1 < n and TABLE_SEP.match(lines[i + 1]):
            header = _split_row(line)
            i += 2
            rows = []
            while i < n and lines[i].lstrip().startswith("|"):
                rows.append(_split_row(lines[i]))
                i += 1
            th = "".join(f"<th>{inline.render(c)}</th>" for c in header)
            trs = []
            for row in rows:
                row = row + [""] * (len(header) - len(row))
                tds = "".join(f"<td>{inline.render(c)}</td>" for c in row[: len(header)])
                trs.append(f"<tr>{tds}</tr>")
            out.append(
                '<div class="table-wrap"><table><thead><tr>'
                + th
                + "</tr></thead><tbody>"
                + "".join(trs)
                + "</tbody></table></div>"
            )
            continue

        # ---- blockquote
        if line.lstrip().startswith(">"):
            body = []
            while i < n and (lines[i].lstrip().startswith(">") or (body and lines[i].strip())):
                body.append(re.sub(r"^\s*>\s?", "", lines[i]))
                i += 1
            inner, _ = render("\n".join(body), link=link, shift=0)
            kind = "note"
            first = body[0].strip().lower() if body else ""
            if first.startswith(("note", "tip")):
                kind = "note"
            elif first.startswith(("warning", "careful", "beware")):
                kind = "warn"
            out.append(f'<aside class="callout callout--{kind}">{inner}</aside>')
            continue

        # ---- lists
        um, om = UL.match(line), OL.match(line)
        if um or om:
            ordered = bool(om)
            base_indent = len((om or um).group(1))
            items: list[tuple[int, list[str]]] = []
            while i < n:
                m_ul, m_ol = UL.match(lines[i]), OL.match(lines[i])
                m = m_ol if ordered else m_ul
                if not m:
                    if lines[i].strip() and items and lines[i].startswith(" " * (base_indent + 2)):
                        items[-1][1].append(lines[i])
                        i += 1
                        continue
                    break
                indent = len(m.group(1))
                text_ = m.group(3)
                if indent < base_indent:
                    break
                if indent > base_indent:
                    # a nested list: attach to the previous item, rendered recursively
                    j = i
                    chunk = []
                    while j < n:
                        mm = UL.match(lines[j]) or OL.match(lines[j])
                        if not mm and not lines[j].strip():
                            break
                        if mm and len(mm.group(1)) < indent:
                            break
                        chunk.append(lines[j])
                        j += 1
                    if items:
                        items[-1][1].extend(chunk)
                    i = j
                    continue
                items.append((indent, [text_]))
                i += 1

            def render_items(entries: list[tuple[int, list[str]]]) -> str:
                tag = "ol" if ordered else "ul"
                parts = [f"<{tag}>"]
                k = 0
                while k < len(entries):
                    indent, body = entries[k]
                    # gather this item's own lines up to the next same-or-less indent
                    text_lines = [body[0]]
                    nested: list[tuple[int, list[str]]] = []
                    k += 1
                    while k < len(entries) and entries[k][0] > indent:
                        nested.append(entries[k])
                        k += 1
                    extra = body[1:]
                    txt = " ".join(x.strip() for x in text_lines if x.strip())
                    html_txt = inline.render(txt)
                    if extra:
                        sub, _ = render("\n".join(extra), link=link)
                        html_txt += sub
                    if nested:
                        html_txt += render_items(nested)
                    parts.append(f"<li>{html_txt}</li>")
                parts.append(f"</{tag}>")
                return "".join(parts)

            out.append(render_items(items))
            continue

        # ---- paragraph
        para = []
        while i < n and not is_block_start(i):
            para.append(lines[i].strip())
            i += 1
        if para:
            text_ = " ".join(para)
            out.append(f"<p>{inline.render(text_)}</p>")
            continue
        i += 1

    return "\n".join(out), headings


def plain(html: str) -> str:
    """Text of a rendered page, for the search index.

    The furniture comes out first — the `#` an `h2` links itself with, the Copy button on every
    code block, the code itself — because a snippet made of it reads like noise in the results.
    """
    text = re.sub(r"<(script|style|pre)[^>]*>.*?</\1>", " ", html, flags=re.S)
    text = re.sub(r'<a class="h-anchor".*?</a>', " ", text, flags=re.S)
    text = re.sub(r'<button class="code__copy".*?</button>', " ", text, flags=re.S)
    text = re.sub(r"<[^>]+>", " ", text)
    text = _html.unescape(text)
    return re.sub(r"\s+", " ", text).strip()