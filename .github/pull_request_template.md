<!--
Two things this template is for: the reader of this pull request in a year, and the reviewer today.
A description that says "fixes the bug" costs the reviewer the work of finding out which bug.
-->

## What this changes

<!-- One paragraph. What was wrong, what it does now, and what a user would notice. -->

## Why this way

<!--
What the alternatives were, and what broke when they were tried. If this is a bug fix: the cause,
not the symptom. If it is a new public name, why it earns one — it is in `api.txt` from now on.
-->

## Checks

The four that must be green (`make`, `make test`, `make lsp`, `make dx`), plus whichever of the
others this change touches. An unrun gate is not a failed gate — leave it, and say why.

| Gate | Run | Result |
| --- | --- | --- |
| `make` | [ ] | |
| `make test` | [ ] | |
| `make dx` | [ ] | `api … budget N, +0, 0 removed` |
| `make lsp` | [ ] | |
| `make site` (`git status website/` empty) | [ ] | |
| `make links` | [ ] | |
| `make snippets` | [ ] | |
| `make skill-check` | [ ] | |
| `make budget` | [ ] | |
| `make check-version` / `make install-test` | [ ] | |
| `sh scripts/spdx.sh --check` | [ ] | |
| `make fmt-check` | [ ] | |

## Sign-off

- [ ] Every commit has a `Signed-off-by` line (`git commit -s`), per [CONTRIBUTING.md](../CONTRIBUTING.md).
- [ ] New files carry `// SPDX-License-Identifier: Apache-2.0`.

## Does the paper follow the code?

- [ ] The behaviour that changed is written down in `docs/reference.md` (or it did not change).
- [ ] If `docs/` or `website/src/content/` changed: `make site` was run and `website/` is committed.
- [ ] If a `SPEC §n` citation moved, the section it names still says what the comment claims.
- [ ] If a public name was added or removed: the `api.txt` diff is in this pull request, with a reason.