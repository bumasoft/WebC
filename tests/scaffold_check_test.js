// The scaffolds `webc new` emits must pass the command the docs tell a developer to run.
//
// Nothing in the framework's own suite built a scaffold's *tests*, so the `web` template
// shipped a test file that did not compile — its helper was called `render`, and
// <webc/http.h> owns that word for a server response, so a fresh project failed `webc check`
// in its first minute with an error pointing at the framework instead of at the name. The
// same hole hid a second one: `doctor` called a browser-only app (a `client` set and no
// `server` set) a project with a missing entry, and `webc check` then tried to exec a binary
// that shape never links.
//
// So: scaffold each template into a temp dir and run the real CLI on it.
const { execFileSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const CLI = path.resolve(__dirname, '..', 'build', 'webc');

function fail(msg) {
  console.log('  FAIL ' + msg);
  process.exitCode = 1;
}

function run(args, cwd) {
  try {
    return { ok: true, out: execFileSync(CLI, args, { cwd, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }) };
  } catch (e) {
    return { ok: false, out: (e.stdout || '') + (e.stderr || '') };
  }
}

// wasi-sdk is an optional toolchain (README, "Requirements"): a `web` scaffold has only a
// browser half, so `webc check` on it compiles wasm and cannot pass without it. CI installs
// wasi-sdk, so the assertion runs there; a contributor without it gets a SKIP that says why
// instead of two red lines in a suite the README promised would pass.
function hasWasi() {
  const d = run(['doctor'], os.tmpdir());
  const row = d.out.split('\n').find((l) => l.includes('wasi-sdk')) || '';
  return row.length > 0 && !/missing|not installed|run `webc doctor`/i.test(row) && row.trim().split(/\s+/).length > 1;
}

(function main() {
  console.log('check: the three scaffolds pass the command the docs promise');
  if (!fs.existsSync(CLI)) { console.log('  SKIP: no build/webc (run make cli)'); return; }
  const wasi = hasWasi();
  if (!wasi) console.log('  SKIP web: no wasi-sdk, and a browser-only scaffold cannot be checked without it');

  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'webc-scaffold-'));
  try {
    for (const template of ['web', 'server', 'fullstack']) {
      if (template === 'web' && !wasi) continue;
      const dir = path.join(root, template);
      const made = run(['new', dir, '--template=' + template], root);
      if (!made.ok) { fail(template + ': webc new failed\n' + made.out.slice(0, 400)); continue; }

      // the scaffold must carry the project's own name. It shipped `__NAME__` in the
      // `<h1>` and the header comment instead: the templates are printf formats, and the
      // placeholder was never a `%s`, so the argument was silently ignored.
      const walk = (d) => fs.readdirSync(d, { withFileTypes: true })
        .flatMap((e) => (e.isDirectory() ? walk(path.join(d, e.name)) : [path.join(d, e.name)]));
      let named = false;
      for (const f of walk(dir)) {
        const text = fs.readFileSync(f, 'utf8');
        if (text.includes('__NAME__')) fail(template + ': ' + path.basename(f) + ' still says __NAME__');
        if (text.includes(template)) named = true;
      }
      if (!named) fail(template + ': the scaffold never names the project');

      // The documentation has to be *in the app*. An agent working in a scaffold reported
      // the auth API as undocumented and started hunting the framework checkout for the
      // header; `webc new` now writes the skill into `.webc/docs/`, and this is the test
      // that says so. A router with a dead end is the failure: auth is the leaf that was
      // missing, so it is the one asserted by name.
      const skill = path.join(dir, '.webc', 'docs', 'SKILL.md');
      const auth = path.join(dir, '.webc', 'docs', 'references', 'auth.md');
      if (!fs.existsSync(skill)) fail(template + ': no .webc/docs/SKILL.md — the app carries no documentation');
      else if (!fs.statSync(path.join(dir, '.webc', 'docs', 'references')).isDirectory())
        fail(template + ': .webc/docs has no references/');
      if (!fs.existsSync(auth)) fail(template + ': .webc/docs has no auth leaf');
      else if (!fs.readFileSync(auth, 'utf8').includes('auth_verify'))
        fail(template + ': the auth leaf does not document auth_verify');
      if (!fs.readFileSync(path.join(dir, 'AGENTS.md'), 'utf8').includes('.webc/docs/SKILL.md'))
        fail(template + ': AGENTS.md does not name the documentation');

      const check = run(['check'], dir);
      if (!check.ok) fail(template + ': a fresh scaffold does not pass webc check\n' +
        check.out.split('\n').filter((l) => /error|FAILED|problem/.test(l)).slice(0, 8).join('\n'));
      else if (!check.out.includes('all clear')) fail(template + ': webc check did not say all clear');

      const doctor = run(['doctor'], dir);
      if (!doctor.ok) fail(template + ': webc doctor found a problem in a fresh scaffold\n' + doctor.out);
      else if (!doctor.out.includes('no problems found')) fail(template + ': doctor is not quiet');

      // a browser-only app has no server half, so nothing may try to exec one — and its
      // entry is the client set, not a missing src/main.c
      if (template === 'web') {
        if (!check.out.includes('skipped (browser-only')) fail('web: webc check ran a boot hook a browser-only app has no binary for');
        if (doctor.out.includes('(missing)')) fail('web: doctor calls a client-only project a missing entry');
        if (!doctor.out.includes('12 client') && !doctor.out.includes('1 client'))
          fail('web: doctor does not report the client set as the entry');
      }
    }
    console.log(wasi ? '  ok   web, server and fullstack scaffold clean'
                     : '  ok   server and fullstack scaffold clean (web skipped: no wasi-sdk)');
  } catch (e) {
    fail('the CLI run itself failed: ' + (e.message || e).slice(0, 400));
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
})();