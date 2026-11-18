# Decisions

One entry per decision that changed the shape of the project, in the order they were taken. The
site's `decisions` page is generated from this file, one card per `## D<n>` heading.

## D1 — Licence: Apache-2.0

Taken before the repository was published, and before the first outside patch arrived.

Apache-2.0 over MIT for the explicit patent grant and the NOTICE obligation: a framework that ships
an HTTP server, an auth layer, a template engine and a database driver is the kind of thing a
company's legal review reads before adopting, and the patent clause is what makes that review short.
The cost is a slightly longer licence; the benefit is that the answer to "can we ship this?" is one
word.

The licence text in `LICENSE` is the canonical Apache License 2.0 from
`https://www.apache.org/licenses/LICENSE-2.0.txt`, with only the copyright line in the appendix
filled in. `NOTICE` carries the attribution and the list of libraries that are linked but never
vendored. Every C source file carries `// SPDX-License-Identifier: Apache-2.0` on line 1, so the
licence survives a file being copied out of the tree; `sh scripts/spdx.sh --check` fails when a file
is missing it.

## D2 — The name stays WebC, and there is no npm package

"WebC" is also the name of 11ty's deprecated single-file web-component format and of Motion Canvas's
compiler. The name is kept anyway: this is a C framework, not a web-component format, and one
sentence of README answers the collision better than a rename would.

npm and Node packaging are explicitly **out of scope**. WebC ships one static binary and a framework
tree; there is no package to install into `node_modules` and no intention to make one. `node` appears
in the requirements only as an optional local dependency of the JS-bridge and `check` test suites.
## D3 — Linux is a supported platform, and the epoll backend is the default there

The scheduler had two backends from the beginning — kqueue and epoll — but only kqueue was ever
built, so the epoll one was dead code that had never met a compiler. Shipping a framework whose
README says "Linux is experimental" is a promise to fix it later at a worse price, and the price
turned out to be small: the split was three `#include`s, the `WC_UCTX_*` register fields for
`aarch64` and `x86_64`, and the link flags.

`WC_EPOLL_NETPOLL` is now defined automatically on `__linux__` (`WC_NO_EPOLL_NETPOLL` opts out), so
an app built with the documented house-rule command — `cc -std=c23 -I include app.c
build/libwebc.a` — links on Linux without knowing that the flag exists. `-lsqlite3`, `-lz` and
`-lpthread` are link flags on every platform, and TLS is found through `pkg-config` instead of the
Homebrew prefix. The suite runs green on `ubuntu:24.04` (aarch64 and x86_64, clang 21) and the CI
matrix leg is required, not `continue-on-error`: a red row that nobody reads is worse than no row.

What Linux does *not* get yet is proof that the artefact works: `release.yml` has built linux
`x86_64` and `aarch64` rows since before the port, but nothing ever unpacked the tarball and ran
it. That smoke test is the next piece of release work; until it exists, a Linux user's supported
path is the checkout.

## D4 — The compiler floor is clang 21, because the suite uses `#embed`

`#embed` is C23 and it is the only sane way to put the JS bridge and the wasi module into the CLI
binary without a code generator step. Clang has had it since 19, GCC 15 shipped it, but the test
suite is the binding constraint: `tests/dx3_test.c` embeds a file, so a compiler without `#embed`
cannot run `make test` at all. The floor is therefore stated as **clang 21**, which is what the
tree is developed and measured on and what CI pins. Saying "18" and then failing on the first test
would be worse than asking for a newer compiler.

## D5 — LeakSanitizer is off by default on Linux, and the leaks became a debt gate

The suite builds with `-fsanitize=address,undefined`. On Linux that includes LeakSanitizer, which
macOS's ASan does not have, so two lesson suites that had always passed on Darwin reported leaks
the moment they ran on glibc — 25 KB in 9 allocations, all of them in `db_exec` and the migration
path. They are real leaks, they are pre-existing, and they are not a port regression.

Turning them into a red gate on day one would have blocked the entire Linux leg on work that has
nothing to do with Linux. `cmd_test()` therefore sets `ASAN_OPTIONS=detect_leaks=0` on Linux unless
the caller set it, and `make test-leaks` runs the same suite with `WEBC_TEST_LEAKS=1` to reproduce
the report on demand. The debt is written down here rather than hidden: the goal is `make test-leaks`
green and `make test` without the override.

## D6 — One number, in `VERSION`, and `webc new` writes a different one

The tree said `0.1.0` in eleven places: the CLI's `--version`, the LSP and MCP `serverInfo`
handshakes, the site's header and footer, the editor extension's `package.json`, the scaffold
templates, and a Homebrew formula. Any release would have updated eight of them.

The number now lives in `VERSION` at the repository root and reaches the CLI as
`-DWEBC_VERSION` (with a `0.0.0-dev` fallback in `<webc/core.h>` so a hand-written `cc` line still
compiles), the site through `website/build.py`, and the extension through
`tools/check-version.py`, which is a gate rather than a convention: it fails when the file, the
built binary, the committed pages, `editors/vscode/package.json` or an exact tag disagree, and it
also refuses a *literal* equal to the current version inside any C source, because that is a
number that looks right today and wrong on release day.

`APP_VERSION_DEFAULT` stays a literal and stays `0.1.0`: it is the version `webc new` writes into a
fresh app's `webc.json`, which is a different question from the framework's own number. Collapsing
the two would have every generated project claim to be the toolchain.

`Formula/webc.rb` is gone. It pointed at `webc-darwin-arm64.tar.gz` (the artefacts are
`darwin-aarch64`), at a `FILL_ME_IN` tag and at sixty-four zeroes, and a half-filled formula in a
public repository is worse than no formula: `brew` users get the same `curl | sh` line. A tap is
worth doing on the day there is a tap, generated from the published digest.

## D7 — The installer refuses a tarball nobody vouched for

`install.sh` fetched a tarball and unpacked it into `~/.local` without ever looking at the
`.sha256` the release publishes beside it. The digest names the *platform* file
(`webc-darwin-aarch64.tar.gz`) while the download lands as `webc.tar.gz`, so `shasum -c` cannot be
used directly — the digests are compared instead (`sha256sum` where it exists, `shasum -a 256` on
the macOS boxes that only have that), and a missing `.sha256` is a refusal rather than a warning:
`set -eu` turns a digest it failed to compute into an abort, not an empty string that matches.

That check is now tested, because a comparison nobody has seen fail is a comparison that does not
work: `scripts/test-install.sh` builds a fake release in a temp directory, serves it over
`file://` (through `WEBC_BASE_URL`, which also makes mirrors and air-gapped installs possible) and
drives three cases — matching digest installs and the installed CLI reports the version, one
changed byte is refused with exit 1 and nothing unpacked, and an unpublished digest is refused
too. `make install-test`, in CI.
