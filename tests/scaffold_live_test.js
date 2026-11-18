// `webc scaffold live <Model>` — the live page in one command, and the claim that
// matters: a fresh app that runs it passes the whole gate, because the generated
// routes broadcast what the generated test proves the room holds.
const { execFileSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const CLI = path.resolve(__dirname, '..', 'build', 'webc');
function fail(m) { console.log('  FAIL ' + m); process.exitCode = 1; }
function run(args, cwd) {
  try { return { ok: true, out: execFileSync(CLI, args, { cwd, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }) }; }
  catch (e) { return { ok: false, out: (e.stdout || '') + (e.stderr || '') }; }
}
(function main() {
  console.log('scaffold live: the live page passes the whole gate');
  if (!fs.existsSync(CLI)) { console.log('  SKIP: no build/webc (run make cli)'); return; }
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'webc-slive-'));
  try {
    const dir = path.join(root, 'app');
    if (!run(['new', dir, '--template=fullstack'], root).ok) { fail('webc new failed'); return; }
    const s = run(['scaffold', 'live', 'Note'], dir);
    if (!s.ok) { fail('scaffold live failed\n' + s.out.slice(0, 400)); return; }
    const src = path.join(dir, 'src', 'live_note.c');
    const tst = path.join(dir, 'tests', 'live_note_test.c');
    if (!fs.existsSync(src) || !fs.existsSync(tst)) { fail('the files were not written'); return; }
    const body = fs.readFileSync(src, 'utf8');
    if (!body.includes('live_join("notes", "all", live_json(db_all(Note)))'))
      fail('the SSE route does not seed the whole table');
    if (!body.includes('live_broadcast("notes", "all"'))
      fail('the mutations do not broadcast the room');
    // the generated test must carry a real format specifier, not a substituted value:
    // the generator once ate %lld and wrote pointer bits where the id belonged
    if (!/snprintf\(body, sizeof body, "id=%lld"/.test(fs.readFileSync(tst, 'utf8')))
      fail('the generated test lost its %lld — the generator substituted it');
    // the whole gate, on the app the scaffold just touched
    const check = run(['check', '--json'], dir);
    let ok = false;
    try { ok = JSON.parse(check.out).ok === true; } catch (e) { ok = false; }
    if (!ok) fail('the scaffolded app does not pass webc check:\n' + check.out.slice(0, 500));
    // refuses to overwrite without --force
    if (run(['scaffold', 'live', 'Note'], dir).ok) fail('re-scaffolding should say the file exists');
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
})();
