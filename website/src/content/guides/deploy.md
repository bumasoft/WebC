A WebC app deploys as **one binary and one directory**. There is no runtime to install, no node_modules,
no `PATH` to arrange for a second interpreter: the server is a static-ish C binary, the browser half is a
`.wasm` file it serves, and the assets are the files you wrote.

```sh
webc build --release      # .webc/app, .webc/app.wasm
webc deploy               # whatever `webc.json` says: a tarball, a directory, a container
```

## What is in the build

| Artifact | What it is |
| --- | --- |
| `.webc/app` | the server binary: routes, database, views, the SSE links |
| `.webc/app.wasm` | the browser half — only when `webc.json` has a `client` list |
| `public/` | served as-is by `mw_static`, with cache headers |
| `partials/` | authored HTML included by name (`partial("site/head")`), read once at first use |
| `migrations/` | applied at boot, in filename order |

`webc size` reports what each one costs and refuses to guess; `make budget` in the framework repo is
the same number as a gate.

## The three ways to run it

```sh
./.webc/app                              # foreground
PORT=8080 DATABASE_URL=postgres://user:pw@localhost:5432/app ./.webc/app
webc serve                               # a release build, served the way a deployment is
```

`webc serve` is the useful one in front of people: gzip on, byte ranges answered, graceful drain on
SIGTERM (in-flight requests finish, streams are closed with a frame the client understands), and the
dev reloader off.

## The ops files: `webc deploy --pack`

The binary is the whole deploy; the files around it are the same in every app except for the name
and the port. The lines that are *not* the same are the ones a copy-paste from a blog gets wrong:

```sh
webc deploy --pack docker,fly,systemd,caddy,nginx   # or: --pack all
```

writes, into `dist/`, the ritual stamped from the manifest it just built:

| File | What it is |
| --- | --- |
| `Dockerfile` | the image around the binary. The base is an `ARG`, not a choice — a base image is a promise about someone else's machine |
| `fly.toml` | fly.io: the port, HTTPS, and a note on why a page with streams wants `min_machines_running = 1` |
| `<name>.service` | a hardened systemd unit: `DynamicUser`, `ProtectSystem=strict`, and the one `ReadWritePaths=` a sqlite file needs |
| `Caddyfile.webc` | the proxy in front — `flush_interval -1` is the line a live page cannot live without |
| `nginx-webc.conf` | the same for nginx: `proxy_buffering off`, a read timeout in hours, the websocket headers |
| `DEPLOY.md` | what each file is and the commands that use it |

The files are the app's the moment they land: edit them, commit them, diff them on the next deploy.
No pack writes an *active* healthcheck — the health route is a convention the app implements (below),
not something the framework serves, and a probe that works in the template and fails on the platform
is worse than none.

## CI: `webc ci`

The gate the docs promise, as a workflow:

```sh
webc ci            # .github/workflows/webc.yml
webc ci --gitlab   # .gitlab-ci.yml
```

It runs `webc check` and `webc test`, attaches a `dist/` artifact on tags, adds the wasi-sdk step by
itself when the app has a client half — and pins the framework with one `WEBC_REF` at the top, the
way any dependency is pinned. Bumping it is an edit the app makes deliberately.

## Configuration

Environment first, then a JSON file, then the default — and the model is the schema for the file, so a
typo in `config.json` is a startup error naming the field rather than a value that silently did not
apply.

```sh
PORT=8080
DATABASE_URL=postgres://user:pw@host/app
REDIS_URL=redis://host:6379/0
WEBC_SECRET=<32 bytes, hex>          # signs sessions and cookies
WEBC_LOG=info                        # error|warn|info|debug
WEBC_QUERY_CACHE=1
```

A secret that is missing in production is a refusal to start, not a random key per boot — the failure
mode of the latter is sessions that log everyone out on every deploy.

## Health, metrics, and looking inside

```sh
curl localhost:8080/health       # liveness: the route every `webc new` scaffold ships
curl localhost:8080/readyz       # readiness: the database and Redis, if configured
curl localhost:8080/metrics      # Prometheus text format
```

`/health` is app code every scaffold carries (`GET("/health") { return res_text(200, "ok"); }` is
the whole probe), and `/readyz` is the same idea for the database and Redis, by hand where an app
wants it. `/metrics` and `/debug` are the framework's. Point a platform's probe at a route the app
actually serves: the deploy packs ship no active healthcheck for exactly this reason, and a probe
that works in a template and fails on the platform is worse than none.

The metrics are the ones that matter under load, not a generic set: request counts by route and status,
latency histograms, the database pool (open, waiting, refused), the scheduler (tasks runnable, parked),
the query cache (hit, miss, bypassed) and the room table.

`/debug` (behind `WEBC_DEBUG=1`) adds the request log with timings, the route table, the migrated
schema, and a CPU sampler that names the function a slow request was in.

## Behind a proxy

The framework reads `X-Forwarded-For` and `X-Forwarded-Proto` only when `WEBC_TRUST_PROXY=1`; that is
the setting that makes rate limits and `peer` mean the client rather than the proxy. Cookies are
`Secure` and `SameSite=Lax` when the request arrived as HTTPS.

```c
// src/main.c — the middleware a deployment adds; the framework installs the rest
#include <webc/server.h>
#include <webc/gzip.h>

void webc_boot(void) {
  // the CSP: the default policy, or the parts of it this deployment changes
  const char *img[] = {"'self'", "data:", "https://cdn.example.com"};
  webc_use(mw_headers((HeadersCfg){.img_src = img, .img_src_n = 3,
                                   .extra = "upgrade-insecure-requests"}));
  webc_use(mw_compress());         // before anything that answers
  // cancels the request's context and the query in flight
  webc_use(mw_timeout_ms(5000));
  webc_use(mw_recover());          // a panic is a 500, not a lost connection
}
```

Order matters in both directions: compression wraps whatever answers after it, and the timeout has
to be inside the headers so the 504 it writes carries them. What is *not* in that function is
already there: `webc_main()` calls `webc_install_default_middleware()` after `webc_boot()`, so the
request id, the logging, the panic recovery, the body limit, the static files, the document pass
and the 404 page are the framework's, and the app's middleware — registered first — wraps all of
it. Two of the calls above are the framework's own defaults, named here because this deployment
puts them in a particular place: a middleware whose name is already in the chain is not
installed twice, so `mw_compress()` and `mw_recover()` are *this* app's, at *this* position, and
there is still exactly one of each. That is also how `mw_headers()` above becomes the policy
instead of a second header beside `webc.json`'s.

Two keys in `webc.json` cover what would otherwise need C: `"csp"` for the policy a project without its
own `mw_headers()` gets, and `"not_found"` for the page a browser gets for a URL nobody routed.

## Databases in production

- **sqlite** is a real choice for a single-node app: `PRAGMA journal_mode=WAL` is set for you, and
  a write transaction is a file lock rather than a network hop.
- **postgres** speaks the wire protocol directly — no `libpq`, no connection-string quirks — and its
  socket parks like every other fd, so a slow query holds a connection rather than a processor.
- Migrations run at boot. In a multi-instance deploy, run them once (`webc migrate up`) and let the
  instances boot against the migrated schema; `webc check` compares models to schema and names drift.

## The size gate

Byte budgets are part of the build, not a review topic:

```console
$ webc size
  app        .webc/app         1.4 MB
  wasm       .webc/app.wasm    194 KB   (63.5 kB gzip)
  public     public/           12 KB
```

`webc size --modules` builds a probe per standard-library module, so "what does `table.h` cost me" has
an answer instead of an opinion. `make budget` fails the build when a number grows past its limit, which
is why the framework's own floor has stayed where it is.

## A checklist

1. `webc check` clean — build, tests, doctor, boot, schema, routes.
2. `partials/` shipped beside `public/` — a partial is read from disk at runtime, and a
   missing one renders as nothing (one warning in the log, nothing on the page). `webc check`
   catches a name with no file before you deploy, not after.
2. `WEBC_SECRET` set, and different from the dev one.
3. `WEBC_TRUST_PROXY=1` if anything sits in front.
4. `webc migrate up` run (or boot migrations enabled) before traffic.
5. `/readyz` wired into the platform's health check, `/metrics` into its scraper.
6. `SIGTERM` handled — `webc serve` already drains; make sure your platform sends it.

## See also

- Reference [§15 Ops](../reference/ops.html) and [§14 The CLI](../reference/the-cli.html).
- [Testing](../guides/testing.html) — what `webc check` runs before any of this.
