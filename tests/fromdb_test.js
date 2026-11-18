// `webc model from-db` — the reverse of `webc migrate diff`: an existing
// database describes its tables, and the CLI writes the models (and, with
// `--write`, the baseline + the two files that link them). The strong claim:
// a fresh minimal app that imports a real database passes `webc check` whole —
// build, boot, schema — because that is the minute-one experience the command
// exists for.
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

function has(body, text, what) {
  if (!body.includes(text)) fail('missing ' + (what || JSON.stringify(text)));
}

(function main() {
  console.log('model from-db: the database describes its models');
  if (!fs.existsSync(CLI)) { console.log('  SKIP: no build/webc (run make cli)'); return; }

  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'webc-fromdb-'));
  try {
    // a database a team already has: plural tables, a varchar, a nullable int, a bool
    const db = path.join(root, 'old.db');
    execFileSync('sqlite3', [db], {
      input: `create table todos (id integer primary key autoincrement, title varchar(200) not null, done integer not null default 0, created_at text not null);
create table media_assets (id integer primary key autoincrement, path text not null, bytes integer, owner_account_id integer);`,
    });

    // stdout shape: names, kinds, qualifiers
    const gen = run(['model', 'from-db', '--database', 'sqlite:' + db], root);
    if (!gen.ok) { fail('from-db failed\n' + gen.out.slice(0, 500)); return; }
    has(gen.out, 'WEBC_MODEL(Todo,', 'the table name became the model name');
    has(gen.out, 'WEBC_MODEL(MediaAsset,', 'snake_case plural -> PascalCase singular');
    has(gen.out, '(id, i64, .pk = true)', 'the primary key');
    has(gen.out, '(title, Text, .max = 200, .required = true)', 'varchar(200) not null');
    has(gen.out, '(bytes, i64, .null = true)', 'a nullable integer opts into .null');
    has(gen.out, '(done, i64)', 'sqlite cannot tell an int from a bool — honest i64');
    has(gen.out, 'Vec(Todo)', 'the collection');
    if (/sqlite_master|_wc_/.test(gen.out)) fail('internal tables leaked into the models');

    // --write into a minimal app, then the whole gate
    const app = path.join(root, 'app');
    fs.mkdirSync(path.join(app, 'src'), { recursive: true });
    fs.mkdirSync(path.join(app, 'data'), { recursive: true });
    fs.mkdirSync(path.join(app, 'migrations'), { recursive: true });
    fs.writeFileSync(path.join(app, 'webc.json'),
      JSON.stringify({ name: 'imported', version: '0.1.0', server: [], database: 'sqlite:data/dev.db', port: 18010 }, null, 2) + '\n');
    const w = run(['model', 'from-db', '--database', 'sqlite:' + db, '--write'], app);
    if (!w.ok) { fail('--write failed\n' + w.out.slice(0, 500)); return; }
    for (const f of ['src/models.h', 'src/models.c', 'migrations/0001_baseline.sql'])
      if (!fs.existsSync(path.join(app, f))) fail(f + ' was not written');
    has(fs.readFileSync(path.join(app, 'migrations', '0001_baseline.sql'), 'utf8'),
        'CREATE TABLE IF NOT EXISTS', 'the baseline applies to the db it came from');
    const cfg = fs.readFileSync(path.join(app, 'webc.json'), 'utf8');
    if (!cfg.includes('"src/models.c"')) fail('models.c was not registered in the server set');

    // the minute-one experience: the imported app passes the whole gate
    const check = run(['check', '--json'], app);
    let stepsOk = true, detail = '';
    try {
      const j = JSON.parse(check.out);
      for (const s of j.steps || []) {
        if (['build', 'boot', 'schema'].includes(s.name) && !s.ok) { stepsOk = false; detail += s.name + ':' + (s.output || '').slice(0, 200) + ' '; }
      }
    } catch (e) { stepsOk = false; detail = check.out.slice(0, 300); }
    if (!stepsOk) fail('the imported app does not pass the gate: ' + detail);

    // a table that is not there is refused, not guessed at
    const miss = run(['model', 'from-db', '--database', 'sqlite:' + db, '--table', 'nope'], app);
    if (miss.ok) fail('a missing table should be refused');
    if (run(['model', 'from-db'], root).ok) fail('no database and no webc.json should be refused');
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
})();
