#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""check-version.py — one number, and every surface agrees on it.

The version lives in the file `VERSION` at the repository root. It reaches the world in
five places, and each one is a chance to ship a lie:

  * the CLI                    `webc --version`, the deploy manifest, the LSP/MCP handshake
                               (through -DWEBC_VERSION, so it is compiled in, not typed)
  * the documentation site     the version badge in the header and the footer
  * the editor extension       editors/vscode/package.json — the marketplace number
  * the release tag            `vX.Y.Z`, when HEAD is tagged
  * a hardcoded copy           any string literal equal to VERSION inside C sources

Exit status is 0 when they all agree, 1 when any of them does not.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
VERSION_FILE = REPO / "VERSION"
SEMVER = re.compile(r"^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$")

problems: list[str] = []
notes: list[str] = []


def fail(what: str, detail: str) -> None:
    problems.append(f"{what}: {detail}")


def main() -> int:
    if not VERSION_FILE.exists():
        fail("VERSION", "the file does not exist — the version has to live somewhere")
        return report("")
    version = VERSION_FILE.read_text().strip()
    if not SEMVER.match(version):
        fail("VERSION", f"{version!r} is not MAJOR.MINOR.PATCH[-prerelease]")
        return report(version)

    # ------------------------------------------------------------ the editor extension
    pkg = REPO / "editors" / "vscode" / "package.json"
    if pkg.exists():
        try:
            declared = json.loads(pkg.read_text()).get("version")
        except json.JSONDecodeError as e:
            fail("editors/vscode/package.json", f"not valid JSON ({e})")
        else:
            if declared != version:
                fail(
                    "editors/vscode/package.json",
                    f'says {declared!r}, VERSION says {version!r}',
                )
    else:
        notes.append("editors/vscode/package.json is absent — skipped")

    # ------------------------------------------------------------ the built CLI
    #
    # Only when it exists: check-version.py is also useful in a tree nobody has built
    # yet, and `make check-version` builds the CLI first so CI always takes this path.
    for cli in (REPO / "build" / "webc", REPO / "dist" / "webc" / "bin" / "webc"):
        if not (cli.exists() and os.access(cli, os.X_OK)):
            continue
        try:
            out = subprocess.run(
                [str(cli), "--version"], capture_output=True, text=True, timeout=60
            ).stdout.strip()
        except (OSError, subprocess.SubprocessError) as e:
            fail(str(cli.relative_to(REPO)), f"could not run it ({e})")
            continue
        # `webc --version` prints "webc 0.1.0"
        match = re.search(r"\b(\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?)\b", out)
        if not match:
            fail(str(cli.relative_to(REPO)), f"--version printed no version at all ({out!r})")
        elif match.group(1) != version:
            fail(
                str(cli.relative_to(REPO)),
                f"--version prints {match.group(1)!r}, VERSION says {version!r} "
                "(the binary predates the VERSION file? `make` rebuilds it)",
            )
        break
    else:
        notes.append("no built CLI to ask — run `make` or `make check-version`")

    # ------------------------------------------------------------ the generated site
    #
    # The pages are generated *and committed*, so a bump without `make site` leaves the
    # committed footer claiming the old number. build.py reads VERSION itself, so the only
    # way this can happen is a stale regeneration — which is exactly what it catches.
    landing = REPO / "website" / "index.html"
    if landing.exists():
        html = landing.read_text(errors="replace")
        if f"v{version}" not in html:
            fail(
                "website/index.html",
                f"has no v{version} in it — the committed pages are stale (`make site`)",
            )
    else:
        notes.append("website/index.html is absent — skipped")

    # ------------------------------------------------------------ the tag, when there is one
    #
    # HEAD is often a commit *after* the tag (the fix that lands the day after a release),
    # so this only fires when the tag points at HEAD: then the names must match exactly.
    try:
        tag = subprocess.run(
            ["git", "describe", "--tags", "--exact-match", "HEAD"],
            cwd=REPO,
            capture_output=True,
            text=True,
            timeout=30,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        tag = ""
    if tag:
        if tag.lstrip("v") != version:
            fail("git tag", f"HEAD is tagged {tag!r}, VERSION says {version!r}")

    # ------------------------------------------------------------ a hardcoded copy
    #
    # A literal equal to the current version in a C source is a number that will be wrong
    # on the next release and nobody will notice, because it will look right today.
    # APP_VERSION_DEFAULT is exempt by name: it is the version `webc new` writes into a new
    # *app*, which is a different question from the framework's own number.
    literal = re.compile(r'"' + re.escape(version) + r'"')
    exempt = re.compile(r"#define\s+APP_VERSION_DEFAULT\b")
    for base in ("src", "include", "tools"):
        for path in sorted((REPO / base).rglob("*")):
            if path.suffix not in {".c", ".h"} or not path.is_file():
                continue
            for n, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
                if literal.search(line) and not exempt.search(line):
                    rel = path.relative_to(REPO)
                    fail(str(rel) + f":{n}", f"hardcodes {version!r} — read WEBC_VERSION")

    return report(version)


def report(version: str) -> int:
    for n in notes:
        print(f"check-version: note: {n}", file=sys.stderr)
    if problems:
        print(f"check-version: {len(problems)} disagreement(s) about version {version or '?'}:",
              file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1
    print(f"check-version: {version} — VERSION, the CLI, the site, the extension and the tag agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())