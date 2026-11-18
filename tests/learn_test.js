// `webc learn` — a lesson is a real app whose own gate fails until the one edit
// the brief names. The strong claim is verified here per lesson: shipped state
// fails, the intended edit passes. A lesson that fails for the wrong reason (or
// passes before the edit) teaches the wrong thing and this is the test that
// says so.
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

function edit(file, from, to) {
  const body = fs.readFileSync(file, 'utf8');
  if (!body.includes(from)) { fail(file + ': the shipped state moved — ' + from.slice(0, 40)); return false; }
  fs.writeFileSync(file, body.replace(from, to));
  return true;
}

// the one edit each lesson names — the same sentence the brief prints
const FIXES = {
  1: (d) => edit(path.join(d, 'src/app.h'),
                 '(title, Text, .required = true, .max = 200))',
                 '(title, Text, .required = true, .max = 200),\n                 (done, bool))') &&
          edit(path.join(d, 'migrations/0001_init.sql'),
               'title text not null', 'title text not null,\n  done integer not null default 0'),
  2: (d) => edit(path.join(d, 'src/main.c'),
                 'res_text(200, "hello")',
                 'res_text(200, fmt("hello, %s", query_text("name")))'),
  3: (d) => edit(path.join(d, 'src/main.c'),
                 '      }\n    }\n  }\n}',
                 '      }\n    }\n    el("p") { class_("note"); text("%s", c->note); }\n  }\n}'),
  4: (d) => edit(path.join(d, 'src/client.c'), 'bind("tick"', 'bind("done"'),
  5: (d) => edit(path.join(d, 'src/app.h'), '(email, Text)', '(email, Text, .required = true)'),
};

(function main() {
  console.log('learn: each lesson fails as shipped and passes after its edit');
  if (!fs.existsSync(CLI)) { console.log('  SKIP: no build/webc (run make cli)'); return; }

  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'webc-learn-'));
  try {
    const list = run(['learn'], root);
    if (!list.ok || !list.ok || (list.out.match(/not started/g) || []).length !== 5)
      fail('webc learn should list five unstarted lessons\n' + list.out.slice(0, 300));

    if (run(['learn', '9'], root).ok) fail('webc learn 9 should be refused');

    for (let n = 1; n <= 5; n++) {
      const made = run(['learn', String(n)], root);
      if (!made.ok) { fail('webc learn ' + n + ' failed\n' + made.out.slice(0, 300)); continue; }
      const d = path.join(root, '.webc', 'learn');
      const dirs = fs.readdirSync(d);
      const slug = dirs.find((x) => x.startsWith(String(n).padStart(2, '0') + '-'));
      const lesson = path.join(d, slug);
      const gate = n === 4 ? 'check' : 'test';

      // shipped: the gate fails — that is the lesson
      if (run([gate], lesson).ok) fail(slug + ': passes before the edit — it teaches nothing');

      // the brief's edit: the gate passes
      if (!FIXES[n](lesson)) continue;
      if (!run([gate], lesson).ok) fail(slug + ': still failing after the intended edit');
    }

    // the list reflects the work: five started, five passed
    const done = run(['learn'], root);
    if (!/5 of 5 passed/.test(done.out)) fail('the list should say 5 of 5 passed\n' + done.out);

    if (run(['learn', '1'], root).ok) fail('re-writing a started lesson should be refused');
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
})();
