// `webc client --ts` writes the typed fetch client from the same facts
// `webc openapi` documents. The assertions run against a real scaffold: a client
// that types a route the handler does not build (or drops one it does) is worse
// than no client, and the file is generated — so every claim here is about what
// the sources say, checked against a fresh `webc new`.
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
  if (!body.includes(text)) fail('webc.client.ts: missing ' + (what || JSON.stringify(text)));
}

(function main() {
  console.log('client: the typed fetch client, from the same facts as openapi');
  if (!fs.existsSync(CLI)) { console.log('  SKIP: no build/webc (run make cli)'); return; }

  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'webc-client-'));
  try {
    const dir = path.join(root, 'full');
    if (!run(['new', dir, '--template=fullstack'], root).ok) { fail('webc new failed'); return; }

    const gen = run(['client', '--ts'], dir);
    if (!gen.ok) { fail('webc client --ts failed\n' + gen.out.slice(0, 600)); return; }
    const file = path.join(dir, 'webc.client.ts');
    if (!fs.existsSync(file)) { fail('no webc.client.ts was written'); return; }
    const ts = fs.readFileSync(file, 'utf8');

    // the wire type, and the Input twin without the pk
    has(ts, 'export interface Note {');
    has(ts, 'id: number;', 'the pk in the wire type');
    has(ts, 'export interface NoteInput {');
    if (/interface NoteInput {[^}]*id:/.test(ts))
      fail('NoteInput: the pk does not belong in what a create sends');
    has(ts, 'body: string;', 'a required field stays required in Input');

    // the operations the template's routes actually are
    has(ts, 'listNote: () => req<Note[]>(', 'GET /api/notes is a list of Note');
    has(ts, 'createNote: (body: NoteInput) => req<Note>(', 'POST /api/notes takes Input, answers Note');
    has(ts, 'deleteNote: (id: number) => req<void>(', 'path_i64 types the :id argument');
    // pages are not the API
    if (/getRoot|req<[^>]*>\("GET", `\/`/.test(ts)) fail('the client calls the page route — pages are not the API');

    // --check: green when current, red when drifted
    if (!run(['client', '--ts', '--check'], dir).ok) fail('--check should pass right after generation');
    fs.appendFileSync(file, '\n');
    if (run(['client', '--ts', '--check'], dir).ok) fail('--check should fail when the file drifted');

    if (run(['client', '--ts'], root).ok) fail('webc client ran outside an app');
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
})();
