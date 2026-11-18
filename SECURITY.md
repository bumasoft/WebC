# Security policy

WebC is a web framework: an HTTP server, a router, a template renderer that escapes, a cookie
session, password hashing, four database drivers, and a WebAssembly client half. It runs inside
other people's applications, so a bug here is a bug in everything built on it.

## Supported versions

WebC is pre-1.0. Only the latest `0.1.x` release gets fixes, and until 1.0 the surface can change in
a minor version. Fixes go to `main` and into the next patch release; there are no maintenance
branches yet.

## Reporting a vulnerability

**Do not open a public issue, a discussion, or a pull request.** Use GitHub's private channel:

> **Security** tab → **Report a vulnerability** →
> <https://github.com/bumasoft/WebC/security/advisories/new>

That opens a draft advisory only you and the maintainer can see, which is also where the fix is
discussed and where credit is given. If you cannot use it, contact the maintainer through
<https://github.com/bumasoft> — and say only that you have a report, with no details, so the channel
can move somewhere private first.

A report is most useful with:

- what the bug lets an attacker do, and what they must be able to reach to do it;
- the smallest application that shows it — a route, a handler, a request, ideally a test;
- the version (`webc version`), the platform, and the compiler;
- whether it is WebC's bug or the application's. Both matter, but they are answered differently.

## What to expect

| When | What |
| --- | --- |
| within 3 days | an acknowledgement, and a first read on whether it is in scope |
| within 10 days | an assessment: affected versions, severity, and what a fix looks like |
| at disclosure | a patched release, an advisory, and credit — unless you would rather stay anonymous |

If a fix will take longer than that, you will hear why rather than nothing. Please give the fix a
chance to ship before you publish: coordinated disclosure is the default here, and an advisory that
names a version with a patch in it is worth more to users than a warning alone.

## In scope

The framework's own code. The shapes of failure that matter most:

- **HTML escaping and the renderer.** `text()`, `attr()`, `richtext()` and the attribute machinery
  are the only thing standing between a model field and a browser. A way to get unescaped bytes into
  the document, or out of the CSP that the server sets, is a serious bug.
- **The HTTP layer.** Request parsing, chunked bodies, `max_body`, header limits, keep-alive and
  pipelining, path decoding, `Range` on `res_file()`, and the multipart parser (`max_parts`,
  per-part `max_bytes`). Smuggling, a request that is read as something other than what a proxy
  in front of it read, or a body that exceeds a documented limit.
- **Sessions and auth.** The session cookie is **signed, not encrypted** — anything put in it is
  readable by the user, which is documented and intended. What is not intended: forging a cookie,
  replaying one past its expiry, an `auth_hash_password` / `auth_check_password` bug, PBKDF2
  iteration downgrade, a timing leak in the comparison, or a session that survives a rotation it
  should not.
- **The database drivers.** WebC implements the PostgreSQL, MySQL/MariaDB and Redis wire protocols
  by hand, so their parsing and authentication is ours: a server response that makes the driver do
  something other than what it says, SCRAM/TLS negotiation bugs, and anything that turns a query
  parameter back into SQL.
- **TLS and gzip.** The optional OpenSSL server path (`include/webc/tls.h`), and the compression
  code's bounds checking in both directions.
- **The browser half.** The wasm bridge: the command buffer, the `js()` seam, hydration and the
  patch stream. A model value that reaches the DOM without passing through the same escaping the
  server half uses is a bug even when the server-rendered page was safe.
- **The dev server and local tools.** `webc serve --watch` (its reload port), the language server,
  and the MCP server all bind locally and trust local input far more than a production server
  should. They still should not execute what a request asks them to. `webc serve` binds whatever
  `WEBC_ADDR` says — including `:8080`, which is every interface — so the framework's job is to be
  safe there, not to expect a firewall.

## Out of scope

- Deploying in dev mode, or exposing the reload port / LSP / MCP to a network. They are development
  tools; running them in production is the misconfiguration, not the bug.
- An application's own authorisation logic, or a handler that reads a `str` from a request and
  trusts it. WebC hands you typed values and escaping that is on by default; it does not know who
  should see what.
- Resource exhaustion that the documented limits already bound (`max_body`, `max_parts`,
  connection limits). A *bypass* of one of those limits is in scope.
- Anything that requires an attacker to already have the process, the database, or the filesystem.
- Reports produced by a scanner with no working exploit and no affected path. Say what an attacker
  gains; "the header is missing" is a bug only when it is one.

## How this project already tries

Not a guarantee — a list of what to read first when you are looking for somewhere to dig:

- Version and build: `webc version`, `webc doctor` (what the binary was built from, and where it
  looks for the framework tree).
- The gate: CI runs the suite on macOS and Linux with AddressSanitizer, plus libFuzzer targets over
  the parsers. `make fuzz` is the local entry point.
- The decision record: [`docs/decisions.md`](docs/decisions.md) — including the two that are
  security-adjacent (the installer refuses an unvouched tarball; LeakSanitizer is off by default on
  Linux, so `make test-leaks` is the debt gate rather than a silent one).
- Dependencies: SQLite, zlib and optionally OpenSSL, linked but never vendored — no third-party
  code is copied into this tree (`NOTICE`). The rest, including the TLS-adjacent protocol work, is
  in `src/` and is fair game.

## Credit

Advisories name the reporter unless they ask otherwise. There is no bounty; there is a fix, a
release note, and the record in `CHANGELOG.md`. Thank you for taking the time — a report that never
needed to be a report is still worth sending.